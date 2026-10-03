#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "chat_state.h"
#include "generation_backend.h"
#include "gui_bridge.h"
#include "spsc_token_ring.h"
#include "text_splitter.h"

namespace lsxhome {

/// What the session is doing. The UI renders the form from it and closes the
/// transcript's open reply once it turns terminal (`kIdle` / `kError`).
enum class ChatPhase {
    kIdle,       // nothing in flight; the form accepts a question
    kLoading,    // the model is being brought up (first turn only)
    kGenerating, // tokens are streaming
    kError,      // the last turn failed; `error()` says why
};

/// Characters a queued question can hold, NUL terminator included. The slot is
/// fixed-size so the request ring stays pre-allocated and lock-free.
inline constexpr std::size_t kQueuedPromptCapacity = 1024;

/// Fixed-size queue slot: the request ring is pre-allocated and lock-free, so a
/// queued question must be trivially copyable and never heap-allocated.
struct QueuedPrompt {
    char text[kQueuedPromptCapacity] = {};
    std::uint32_t len = 0;
};

/// Owns the request queue, the worker thread and the answer hand-off.
///
/// Threading contract:
///  - `Submit`, `RequestStop`, `DrainInto`, `TakeInterrupted`, `phase`, `error`,
///    `Published`, `Dropped` are the UI thread's API.
///  - The worker thread owns the model, the framed history and the bridge's
///    producer side. It never touches ImGui and never reads `ChatState`.
///  - The only shared state is the lock-free `GuiBridge` plus atomics and a
///    mutex around the error string.
///
/// The model is loaded once (`Prepare`) and kept until the session dies: a
/// checkpoint load takes minutes, so it must not happen per question.
class ChatSession {
public:
    static constexpr std::size_t kQueueSlots = 8;  // power of two
    static constexpr std::size_t kMaxPromptChars = kQueuedPromptCapacity;

    /// Drain batch size: one frame's worth of streamed chunks.
    static constexpr std::size_t kDrainBatch = 512;

    /// Chunks cut from a single delta per chunker call. With the 31-byte token
    /// budget this covers ~2 KiB of delta per pass, far beyond one token.
    static constexpr std::size_t kMaxChunksPerDelta = 64;

    ChatSession(GuiBridge& bridge, GenerationBackend& backend)
        : bridge_(bridge),
          backend_(backend),
          worker_([this] { WorkerMain(); }) {}

    ~ChatSession() {
        shutdown_.store(true, std::memory_order_release);
        abort_.store(true, std::memory_order_release);
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    ChatSession(const ChatSession&) = delete;
    ChatSession& operator=(const ChatSession&) = delete;

    /// UI thread. Queues @p question. Returns false when the question is empty,
    /// longer than the fixed prompt slot, or the queue is full — the caller
    /// keeps the draft and opens no transcript turn.
    bool Submit(std::string_view question) {
        if (question.empty() || question.size() >= kMaxPromptChars) {
            return false;
        }
        QueuedPrompt prompt;
        std::memcpy(prompt.text, question.data(), question.size());
        prompt.len = static_cast<std::uint32_t>(question.size());
        if (!queue_.TryPush(prompt)) {
            return false;
        }
        // Claim the turn before the worker can finish it, so the UI never
        // observes "not busy" for a question that is already queued.
        outstanding_.fetch_add(1, std::memory_order_acq_rel);
        // A fresh turn starts with a clean abort latch; the worker resets it
        // again when the turn ends.
        abort_.store(false, std::memory_order_release);
        return true;
    }

    /// UI thread: asks the running generation to stop at the next step.
    void RequestStop() noexcept {
        abort_.store(true, std::memory_order_release);
    }

    /// UI thread: appends everything the worker has published so far into the
    /// transcript. Must run before the phase is inspected, so the tail of an
    /// answer is never closed out unread.
    void DrainInto(ChatState& state) noexcept {
        TokenPayload batch[kDrainBatch];
        for (;;) {
            const std::size_t n = bridge_.DrainAll(batch, kDrainBatch);
            if (n == 0) {
                return;
            }
            for (std::size_t i = 0; i < n; ++i) {
                state.AppendDelta(batch[i].stream_seq, batch[i].text);
            }
        }
    }

    ChatPhase phase() const noexcept {
        return phase_.load(std::memory_order_acquire);
    }

    /// UI thread: true from a successful `Submit` until that turn has finished.
    ///
    /// This is deliberately NOT `phase() == kIdle`: a freshly queued question
    /// has not been picked up by the worker yet, so the phase is still `kIdle`
    /// and keying "the turn is over" off it would close the transcript reply
    /// before the first token arrives.
    bool Busy() const noexcept {
        return outstanding_.load(std::memory_order_acquire) > 0;
    }

    /// UI thread: the last failure, or an empty string.
    std::string error() const {
        std::lock_guard<std::mutex> lock(error_mutex_);
        return error_;
    }

    /// UI thread: reads and clears the "the user stopped this" latch.
    bool TakeInterrupted() noexcept {
        return interrupted_.exchange(false, std::memory_order_acq_rel);
    }

    /// Telemetry forwarded from the shared bridge (the sidebar shows it).
    std::size_t Published() const noexcept { return bridge_.Published(); }
    std::size_t Dropped() const noexcept { return bridge_.Dropped(); }

private:
    using Queue = SpscTokenRing<QueuedPrompt, kQueueSlots>;

    void WorkerMain() {
        QueuedPrompt prompt;
        while (!shutdown_.load(std::memory_order_acquire)) {
            if (!queue_.TryPop(prompt)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            RunTurn(prompt);
        }
    }

    void RunTurn(const QueuedPrompt& prompt) {
        // Every exit path releases the turn's claim, including the early
        // failures that never reach the switch below.
        struct Release {
            std::atomic<int>& counter;
            ~Release() { counter.fetch_sub(1, std::memory_order_acq_rel); }
        } release{outstanding_};

        interrupted_.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(error_mutex_);
            error_.clear();
        }
        phase_.store(ChatPhase::kLoading, std::memory_order_release);

        if (!prepared_) {
            std::string error;
            if (!backend_.Prepare(error)) {
                Fail(error.empty() ? std::string("failed to load the model")
                                   : error);
                return;
            }
            prepared_ = true;
        }
        phase_.store(ChatPhase::kGenerating, std::memory_order_release);

        history_.push_back(ChatTurn{"user", std::string(prompt.text, prompt.len)});
        std::string answer;
        std::uint32_t seq = 0;
        std::string generate_error;
        const GenerationStatus status = backend_.Generate(
            history_,
            [&answer, &seq, this](std::string_view delta) {
                PublishDelta(delta, answer, seq);
            },
            abort_, generate_error);

        switch (status) {
            case GenerationStatus::kOk:
                phase_.store(ChatPhase::kIdle, std::memory_order_release);
                break;
            case GenerationStatus::kAborted:
                interrupted_.store(true, std::memory_order_release);
                phase_.store(ChatPhase::kIdle, std::memory_order_release);
                break;
            case GenerationStatus::kError:
                interrupted_.store(true, std::memory_order_release);
                Fail(generate_error.empty()
                         ? std::string("inference failed")
                         : generate_error);
                return;
        }
        // The exchange is framed for the next turn: the assistant answer is
        // exactly what was published to the UI.
        history_.push_back(ChatTurn{"assistant", answer});
    }

    /// Accumulates @p delta into the worker's answer copy and publishes it to
    /// the UI in token-sized chunks.
    void PublishDelta(std::string_view delta, std::string& answer,
                      std::uint32_t& seq) {
        answer.append(delta.data(), delta.size());
        std::string_view rest = delta;
        while (!rest.empty()) {
            std::string_view chunks[kMaxChunksPerDelta];
            const std::size_t n = SplitUtf8Chunks(
                rest, chunks, kMaxChunksPerDelta, kTokenTextCapacity - 1);
            if (n == 0) {
                return;
            }
            for (std::size_t i = 0; i < n; ++i) {
                TokenPayload payload =
                    MakeTokenPayload(kUnknownTokenId, seq, "");
                std::memcpy(payload.text, chunks[i].data(), chunks[i].size());
                payload.text[chunks[i].size()] = '\0';
                // Retry the same chunk instead of dropping it: an answer that
                // loses words mid-stream is worse than a worker that waits.
                while (!bridge_.Submit(payload) &&
                       !abort_.load(std::memory_order_acquire) &&
                       !shutdown_.load(std::memory_order_acquire)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                ++seq;
            }
            rest.remove_prefix(chunks[n - 1].data() - rest.data() +
                               chunks[n - 1].size());
        }
    }

    void Fail(const std::string& message) {
        {
            std::lock_guard<std::mutex> lock(error_mutex_);
            error_ = message;
        }
        phase_.store(ChatPhase::kError, std::memory_order_release);
    }

    GuiBridge& bridge_;
    GenerationBackend& backend_;
    Queue queue_;
    std::vector<ChatTurn> history_;  // worker-owned; never read by the UI
    std::thread worker_;
    std::atomic<ChatPhase> phase_{ChatPhase::kIdle};
    std::atomic<int> outstanding_{0};  // queued + running turns; UI-facing
    std::atomic<bool> abort_{false};
    std::atomic<bool> shutdown_{false};
    std::atomic<bool> interrupted_{false};
    mutable std::mutex error_mutex_;
    std::string error_;
    bool prepared_ = false;  // worker-thread only
};

}  // namespace lsxhome
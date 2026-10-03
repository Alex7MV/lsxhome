#include <catch2/catch_test_macros.hpp>

#include "lsxhome/chat_session.h"
#include "lsxhome/chat_state.h"
#include "lsxhome/generation_backend.h"
#include "lsxhome/gui_bridge.h"
#include "lsxhome/text_splitter.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// ────────────────────────────────────────────────────────────────────────────
// Counting heap allocator: the streamed-text hot path (chunk -> publish ->
// drain -> append) must never touch it, the same contract the existing bridge
// test proves for token publishing.
// ────────────────────────────────────────────────────────────────────────────
namespace {
std::atomic<long long> g_alloc_count{0};
}  // namespace

void* operator new(std::size_t n) {
    g_alloc_count.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
    g_alloc_count.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void*, std::size_t) noexcept {}
void operator delete[](void*, std::size_t) noexcept {}

namespace {

// This file is deliberately ASCII-only: a narrow literal with non-ASCII bytes
// is interpreted through the compiler's source codepage, which is not
// guaranteed to be UTF-8. Multibyte text is spelled as explicit UTF-8 bytes
// so the assertions test the chunker, not the toolchain's decoding.
const char* const kHello = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82";  // Привет
const char* const kWorld = "\xD0\xBC\xD0\xB8\xD1\x80";                          // мир
const char* const kGlobe = "\xF0\x9F\x8C\x8D";                                  // 🌍

// Strict UTF-8 validator: rejects truncated sequences, stray continuation
// bytes and lead bytes with no continuation. A chunk carrying half a codepoint
// fails here.
bool IsValidUtf8(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char lead = static_cast<unsigned char>(s[i]);
        std::size_t len = 0;
        if (lead < 0x80u) {
            len = 1;
        } else if ((lead & 0xE0u) == 0xC0u) {
            len = 2;
        } else if ((lead & 0xF0u) == 0xE0u) {
            len = 3;
        } else if ((lead & 0xF8u) == 0xF0u) {
            len = 4;
        } else {
            return false;  // continuation byte or invalid lead
        }
        if (i + len > s.size()) return false;
        for (std::size_t k = 1; k < len; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0u) != 0x80u) {
                return false;
            }
        }
        i += len;
    }
    return true;
}

using lsxhome::ChatPhase;
using lsxhome::ChatTurn;
using lsxhome::GenerationBackend;
using lsxhome::GenerationStatus;

/// Scriptable engine stand-in: no CUDA, no model, no vocabulary. Delays,
/// failures and the abort flag are all observable, which is what the session
/// contract is written against.
class FakeBackend final : public GenerationBackend {
public:
    // Scripted behaviour (configured before the session starts).
    std::vector<std::string> deltas;  // emitted in order
    GenerationStatus status = GenerationStatus::kOk;
    bool prepare_ok = true;
    std::string prepare_error;
    std::string generate_error;

    // Observable run record. Atomics and a mutex because the worker thread
    // writes them while the test thread reads.
    std::atomic<int> prepare_calls{0};
    std::atomic<int> generate_calls{0};
    std::atomic<bool> generate_returned{false};
    std::atomic<bool> aborted_seen{false};
    std::mutex history_mutex;
    std::vector<std::vector<ChatTurn>> histories;

    // When set, Generate blocks (without emitting) until it is cleared — the
    // gate that makes queue pressure and Stop deterministic.
    std::atomic<bool> gate{false};

    std::vector<std::vector<ChatTurn>> RecordedHistories() {
        std::lock_guard<std::mutex> lock(history_mutex);
        return histories;
    }

    bool Prepare(std::string& out_error) override {
        prepare_calls.fetch_add(1, std::memory_order_relaxed);
        if (!prepare_ok) {
            out_error = prepare_error;
            return false;
        }
        return true;
    }

    GenerationStatus Generate(const std::vector<ChatTurn>& history,
                             const Sink& sink,
                             const std::atomic<bool>& abort,
                             std::string& out_error) override {
        generate_calls.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(history_mutex);
            histories.push_back(history);
        }
        while (gate.load(std::memory_order_acquire)) {
            if (abort.load(std::memory_order_acquire)) {
                return Aborted();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        for (const std::string& delta : deltas) {
            if (abort.load(std::memory_order_acquire)) {
                return Aborted();
            }
            sink(delta);
        }
        generate_returned.store(true, std::memory_order_relaxed);
        if (status == GenerationStatus::kError) {
            out_error = generate_error;
        }
        return status;
    }

private:
    GenerationStatus Aborted() {
        aborted_seen.store(true, std::memory_order_relaxed);
        generate_returned.store(true, std::memory_order_relaxed);
        return GenerationStatus::kAborted;
    }
};

/// The UI frame's transcript update, mirrored from `DrawChatPanel`: drain
/// first, then close the reply once the session reports the turn as finished.
void PumpFrame(lsxhome::ChatSession& session, lsxhome::ChatState& state) {
    session.DrainInto(state);
    if (state.Busy() && !session.Busy()) {
        state.EndTurn(session.phase() == ChatPhase::kError
                          ? true
                          : session.TakeInterrupted());
    }
}

/// Mirrors `DrawChatForm`'s submit branch: queue first, then open the turn. A
/// refused queue leaves the transcript untouched.
bool SubmitQuestion(lsxhome::ChatSession& session,
                    lsxhome::ChatState& state,
                    const char* question) {
    if (!session.Submit(question)) {
        return false;
    }
    return state.BeginTurn(question);
}

/// Waits until @p predicate holds, pumping the session exactly the way the UI
/// frame does. Fails the test on timeout instead of hanging the suite.
template <typename Predicate>
void PumpUntil(lsxhome::ChatSession& session,
               lsxhome::ChatState& state,
               Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (!predicate()) {
        PumpFrame(session, state);
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    PumpFrame(session, state);
}

}  // namespace

TEST_CASE("lsxhome: UTF-8 chunking never splits a codepoint", "[lsxhome][chat]") {
    const std::string text =
        std::string(kHello) + ", " + kWorld + "! " + kGlobe + " ok";

    std::string_view chunks[32];
    const std::size_t n = lsxhome::SplitUtf8Chunks(text, chunks, 32, 4);
    REQUIRE(n > 1);

    std::string joined;
    for (std::size_t i = 0; i < n; ++i) {
        REQUIRE(chunks[i].size() <= 4);
        REQUIRE(IsValidUtf8(chunks[i]));
        joined.append(chunks[i].data(), chunks[i].size());
    }
    REQUIRE(joined == text);
}

TEST_CASE("lsxhome: UTF-8 chunking honours the output capacity", "[lsxhome][chat]") {
    const std::string text = "abcdefgh";
    std::string_view chunks[2];
    REQUIRE(lsxhome::SplitUtf8Chunks(text, chunks, 2, 4) == 2);
    REQUIRE(chunks[0] == "abcd");
    REQUIRE(chunks[1] == "efgh");

    // Degenerate calls are refused, not crashes.
    std::string_view none[1];
    REQUIRE(lsxhome::SplitUtf8Chunks("a", none, 0, 4) == 0);
    REQUIRE(lsxhome::SplitUtf8Chunks("abc", nullptr, 2, 4) == 0);
    REQUIRE(lsxhome::SplitUtf8Chunks("", chunks, 2, 4) == 0);
}

TEST_CASE("lsxhome: chunking below one codepoint widens instead of corrupting",
          "[lsxhome][chat]") {
    // A 4-byte emoji cannot fit a 2-byte budget. Cutting it would put a broken
    // byte sequence on the wire, so the budget widens to the codepoint size.
    std::string_view chunks[4];
    const std::size_t n = lsxhome::SplitUtf8Chunks(kGlobe, chunks, 4, 2);
    REQUIRE(n == 1);
    REQUIRE(chunks[0].size() == 4);
    REQUIRE(IsValidUtf8(chunks[0]));
}

TEST_CASE("lsxhome: a turn appends the question and opens an assistant reply",
          "[lsxhome][chat]") {
    lsxhome::ChatState state;
    REQUIRE(state.empty());
    REQUIRE_FALSE(state.Busy());

    REQUIRE(state.BeginTurn("why is the sky blue"));
    REQUIRE(state.Busy());
    REQUIRE(state.messages().size() == 2);
    REQUIRE(state.messages()[0].role == "user");
    REQUIRE(state.messages()[0].text == "why is the sky blue");
    REQUIRE(state.messages()[1].role == "assistant");
    REQUIRE(state.messages()[1].text.empty());
    REQUIRE_FALSE(state.messages()[1].interrupted);

    state.EndTurn(false);
    REQUIRE_FALSE(state.Busy());
}

TEST_CASE("lsxhome: a turn is refused while empty or already open",
          "[lsxhome][chat]") {
    lsxhome::ChatState state;
    REQUIRE_FALSE(state.BeginTurn(""));       // nothing to ask
    REQUIRE(state.empty());

    REQUIRE(state.BeginTurn("first"));
    REQUIRE_FALSE(state.BeginTurn("second"));  // a reply is still streaming
    REQUIRE(state.messages().size() == 2);

    state.EndTurn(false);
    REQUIRE(state.BeginTurn("second"));        // the previous turn is closed
    REQUIRE(state.messages().size() == 4);
    REQUIRE(state.messages()[2].text == "second");
}

TEST_CASE("lsxhome: streamed deltas append in order and ignore stale chunks",
          "[lsxhome][chat]") {
    lsxhome::ChatState state;
    REQUIRE(state.BeginTurn("question"));

    REQUIRE(state.AppendDelta(0, kHello));
    REQUIRE(state.AppendDelta(1, ", "));
    REQUIRE(state.AppendDelta(2, kWorld));

    // A replayed or out-of-order chunk must never corrupt the reply text.
    REQUIRE_FALSE(state.AppendDelta(2, "!"));
    REQUIRE_FALSE(state.AppendDelta(0, "?"));
    REQUIRE_FALSE(state.AppendDelta(1, "?"));
    REQUIRE_FALSE(state.AppendDelta(99, "?"));
    REQUIRE(state.messages()[1].text == std::string(kHello) + ", " + kWorld);

    // An empty delta is not a chunk.
    REQUIRE_FALSE(state.AppendDelta(3, ""));

    // The seq cursor resets with the turn.
    state.EndTurn(true);
    REQUIRE(state.messages()[1].interrupted);
    REQUIRE(state.BeginTurn("next"));
    REQUIRE(state.AppendDelta(0, "fresh"));
    REQUIRE(state.messages().back().text == "fresh");
}

TEST_CASE("lsxhome: deltas are refused once the reply is closed", "[lsxhome][chat]") {
    lsxhome::ChatState state;
    REQUIRE(state.BeginTurn("question"));
    REQUIRE(state.AppendDelta(0, "text"));
    state.EndTurn(false);

    REQUIRE_FALSE(state.AppendDelta(1, "tail"));
    REQUIRE(state.messages()[1].text == "text");
    REQUIRE(state.messages().size() == 2);
}

TEST_CASE("lsxhome: the error line is set and cleared independently", "[lsxhome][chat]") {
    lsxhome::ChatState state;
    REQUIRE(state.error().empty());

    state.SetError("model path is required: --model <path>");
    REQUIRE(state.error() == "model path is required: --model <path>");

    state.ClearError();
    REQUIRE(state.error().empty());

    state.BeginTurn("question");
    state.AppendDelta(0, "text");
    state.EndTurn(false);
    state.SetError("failure");
    state.Clear();
    REQUIRE(state.empty());
    REQUIRE(state.error().empty());
    REQUIRE_FALSE(state.Busy());
    REQUIRE(state.BeginTurn("after clear"));
}

TEST_CASE("lsxhome: a submitted question reaches the backend and streams back",
          "[lsxhome][chat]") {
    lsxhome::GuiBridge bridge;
    FakeBackend backend;
    backend.deltas = {kHello, ", ", kWorld};

    lsxhome::ChatState state;
    {
        lsxhome::ChatSession session(bridge, backend);
        REQUIRE(session.phase() == ChatPhase::kIdle);
        REQUIRE(SubmitQuestion(session, state, "hello"));

        PumpUntil(session, state, [&] {
            return !state.Busy() && session.phase() == ChatPhase::kIdle &&
                   state.messages().size() == 2;
        });
        REQUIRE(state.messages()[1].text == std::string(kHello) + ", " + kWorld);
        REQUIRE(backend.prepare_calls.load() == 1);
        REQUIRE(backend.generate_calls.load() == 1);

        const auto histories = backend.RecordedHistories();
        REQUIRE(histories.size() == 1);
        REQUIRE(histories[0].size() == 1);
        REQUIRE(histories[0][0].role == "user");
        REQUIRE(histories[0][0].text == "hello");
    }
    // The destructor joined the worker: the Generate call returned.
    REQUIRE(backend.generate_returned.load());
}

TEST_CASE("lsxhome: the model is prepared once and the history grows",
          "[lsxhome][chat]") {
    lsxhome::GuiBridge bridge;
    FakeBackend backend;
    backend.deltas = {"one"};

    lsxhome::ChatState state;
    lsxhome::ChatSession session(bridge, backend);

    REQUIRE(SubmitQuestion(session, state, "first"));
    PumpUntil(session, state, [&] { return !state.Busy(); });
    REQUIRE(session.phase() == ChatPhase::kIdle);

    REQUIRE(SubmitQuestion(session, state, "second"));
    PumpUntil(session, state, [&] { return !state.Busy(); });

    // Multi-turn: the second request carries the whole exchange.
    const auto histories = backend.RecordedHistories();
    REQUIRE(histories.size() == 2);
    REQUIRE(histories[1].size() == 3);
    REQUIRE(histories[1][0].role == "user");
    REQUIRE(histories[1][0].text == "first");
    REQUIRE(histories[1][1].role == "assistant");
    REQUIRE(histories[1][1].text == "one");
    REQUIRE(histories[1][2].role == "user");
    REQUIRE(histories[1][2].text == "second");

    // The expensive load happens once, not per question.
    REQUIRE(backend.prepare_calls.load() == 1);
    REQUIRE(backend.generate_calls.load() == 2);
}

TEST_CASE("lsxhome: the transcript refuses a second question mid-stream",
          "[lsxhome][chat]") {
    lsxhome::GuiBridge bridge;
    FakeBackend backend;
    backend.gate.store(true);

    lsxhome::ChatState state;
    lsxhome::ChatSession session(bridge, backend);

    REQUIRE(SubmitQuestion(session, state, "first"));
    PumpUntil(session, state,
              [&] { return session.phase() == ChatPhase::kGenerating; });

    // The UI never offers this, and the transcript refuses it too: the reply
    // for the first question is still streaming.
    REQUIRE_FALSE(state.BeginTurn("second"));

    backend.gate.store(false);
}

TEST_CASE("lsxhome: a full request queue refuses instead of blocking",
          "[lsxhome][chat]") {
    lsxhome::GuiBridge bridge;
    FakeBackend backend;
    backend.gate.store(true);

    lsxhome::ChatState state;
    {
        lsxhome::ChatSession session(bridge, backend);
        REQUIRE(SubmitQuestion(session, state, "first"));
        // The worker popped it and is now blocked inside Generate, so nothing
        // else leaves the queue: capacity is deterministic from here.
        PumpUntil(session, state,
                  [&] { return session.phase() == ChatPhase::kGenerating; });

        for (std::size_t i = 0; i < lsxhome::ChatSession::kQueueSlots; ++i) {
            REQUIRE(session.Submit("queued"));
        }
        REQUIRE_FALSE(session.Submit("overflow"));

        // Oversized questions are refused outright: the fixed-size prompt slot
        // must never silently truncate what the user typed.
        const std::string huge(lsxhome::ChatSession::kMaxPromptChars, 'x');
        REQUIRE_FALSE(session.Submit(huge));
        REQUIRE_FALSE(session.Submit(""));

        backend.gate.store(false);
        backend.status = GenerationStatus::kError;  // drain the queue quickly
    }
    REQUIRE(backend.generate_returned.load());
}

TEST_CASE("lsxhome: Stop aborts the run and closes the reply as interrupted",
          "[lsxhome][chat]") {
    lsxhome::GuiBridge bridge;
    FakeBackend backend;
    backend.gate.store(true);

    lsxhome::ChatState state;
    lsxhome::ChatSession session(bridge, backend);

    REQUIRE(SubmitQuestion(session, state, "stop me"));
    PumpUntil(session, state,
              [&] { return session.phase() == ChatPhase::kGenerating; });

    session.RequestStop();
    PumpUntil(session, state, [&] {
        return !state.Busy() && session.phase() == ChatPhase::kIdle;
    });

    REQUIRE(backend.aborted_seen.load());
    // The latch is consumed exactly once, by the frame that closed the reply.
    REQUIRE_FALSE(session.TakeInterrupted());
    REQUIRE(state.messages().size() == 2);
    REQUIRE(state.messages()[1].interrupted);
    REQUIRE(state.messages()[1].text.empty());
    REQUIRE(session.error().empty());
}

TEST_CASE("lsxhome: a failed prepare surfaces as an error phase", "[lsxhome][chat]") {
    lsxhome::GuiBridge bridge;
    FakeBackend backend;
    backend.prepare_ok = false;
    backend.prepare_error = "model path is required: --model <path>";

    lsxhome::ChatState state;
    {
        lsxhome::ChatSession session(bridge, backend);
        REQUIRE(SubmitQuestion(session, state, "question"));
        PumpUntil(session, state, [&] { return session.phase() == ChatPhase::kError; });

        REQUIRE(session.error() == "model path is required: --model <path>");
        REQUIRE(state.error().empty());  // the session's error, not the transcript's
        REQUIRE(backend.generate_calls.load() == 0);
    }
}

TEST_CASE("lsxhome: a failed generation closes the reply as interrupted",
          "[lsxhome][chat]") {
    lsxhome::GuiBridge bridge;
    FakeBackend backend;
    backend.deltas = {"partial answer"};
    backend.status = GenerationStatus::kError;
    backend.generate_error = "KV-cache overflow";

    lsxhome::ChatState state;
    lsxhome::ChatSession session(bridge, backend);

    REQUIRE(SubmitQuestion(session, state, "question"));
    PumpUntil(session, state, [&] { return session.phase() == ChatPhase::kError; });
    PumpFrame(session, state);

    REQUIRE(session.error() == "KV-cache overflow");
    REQUIRE_FALSE(state.Busy());
    REQUIRE(state.messages()[1].interrupted);
    REQUIRE(state.messages()[1].text == "partial answer");
}

TEST_CASE("lsxhome: draining streamed chunks never allocates", "[lsxhome][chat]") {
    lsxhome::GuiBridge bridge;
    FakeBackend backend;  // never invoked: the chunks are published directly
    lsxhome::ChatSession session(bridge, backend);

    lsxhome::ChatState state;
    REQUIRE(state.BeginTurn("question"));

    constexpr std::uint32_t kChunks = 1024;
    for (std::uint32_t seq = 0; seq < kChunks; ++seq) {
        REQUIRE(bridge.Publish(lsxhome::MakeTokenPayload(
            lsxhome::kUnknownTokenId, seq, "x")));
    }

    const long long before = g_alloc_count.load(std::memory_order_relaxed);
    session.DrainInto(state);
    const long long after = g_alloc_count.load(std::memory_order_relaxed);

    REQUIRE(after - before == 0);
    REQUIRE(state.messages()[1].text.size() == kChunks);
}
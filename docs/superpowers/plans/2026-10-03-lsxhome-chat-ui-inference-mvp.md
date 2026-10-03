# Chat UI + Inference MVP Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Type a question in the lsxhome welcome input, have the logestix engine answer it, and watch the answer stream into the main panel with a working Stop button and multi-turn history.

**Architecture:** A UI-thread `ChatState` transcript (header-only, no engine types) is fed by a `ChatSession` that owns a request SPSC ring, a worker thread and the existing `GuiBridge`. The worker calls an injected `GenerationBackend`; the real adapter `LsxGenerationBackend` (`src/lsx_generation_backend.cpp`) is the only translation unit that includes `lsxcommon`. Tests drive a fake backend in a Catch2 target that links neither `lsxcommon` nor CUDA.

**Tech Stack:** C++20, CMake 3.25+ (VS 18 2026, `windows-debug` preset), Dear ImGui 1.92.9b docking, Catch2 v3.16.0, logestix `lsxcommon` (`InferenceEngine::Session`, `LsxModelFactory`, `BuildConversationInputIds`, `Infer` + `token_callback`, `IncrementalDetokenizer`).

**Design spec:** `docs/superpowers/specs/2026-10-03-lsxhome-chat-ui-inference-mvp-design.md`

**Subagent policy:** this repository forbids Task subagents (`AGENTS.md`). Execute inline with `superpowers:executing-plans`.

---

## File Structure

New files:

| File | Responsibility |
|---|---|
| `include/lsxhome/chat_state.h` | UI-thread transcript: messages, open reply, seq cursor, error line. No threads, no ImGui, no `lsxcommon`. |
| `include/lsxhome/generation_backend.h` | Engine boundary (`ChatTurn`, `GenerationStatus`, `GenerationBackend`) + the real adapter's factory declaration is NOT here (see next row). |
| `include/lsxhome/lsx_generation_backend.h` | Factory declaration for the real adapter (`MakeLsxGenerationBackend`). |
| `include/lsxhome/chat_session.h` | Request ring, worker thread, phase/error state, `GuiBridge` publishing, `DrainInto`. |
| `src/lsx_generation_backend.cpp` | The only TU that includes `lsxcommon`: model load, conversation framing, streaming `Infer`, detokenization. |
| `tests/test_lsxhome_chat.cpp` | Catch2 suite + counting allocator + fake backend. |

Modified files:

| File | Change |
|---|---|
| `include/lsxhome/text_splitter.h` | Add `SplitUtf8Chunks`. |
| `include/lsxhome/gui_renderer.h` | Replace `DrawClaudeWelcomeInterface` with `DrawChatPanel(ChatSession&, ChatState&)`; `BuildWorkspaceSkeleton` takes both. |
| `src/gui_renderer.cpp` | Chat transcript, pinned input form, Send/Stop, quick-action prefill; drop the sidebar Drain button. |
| `src/main_win32.cpp` | Delete `ComputeProducer`; construct backend + session + state; auto-submit `--run`; Paris self-check when `--run` has no model. |
| `CMakeLists.txt` | Add `src/lsx_generation_backend.cpp` to the `lsxhome` executable. |
| `tests/CMakeLists.txt` | Add the `test_lsxhome_chat` target and its `fast`-labelled `add_test`. |
| `README.md`, `AGENTS.md` | Keep the documented layout and test list in sync. |

Test target layout: `test_lsxhome_chat` links **only** `Catch2::Catch2WithMain` (no `lsxcommon`, no arrow.dll copy loop — that loop iterates `LSXHOME_TEST_TARGETS`, so the new target must not be added to it).

---

### Task 1: UTF-8-safe chunking + the chat test target

**Files:**
- Create: `tests/test_lsxhome_chat.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `include/lsxhome/text_splitter.h`

- [ ] **Step 1: Register the test target**

In `tests/CMakeLists.txt`, add this block right after the `test_lsxhome_font`
target block (before the `set(LSXHOME_TEST_TARGETS ...)` line). Its name is
deliberately **not** added to that list: the list drives the `arrow.dll` /
`arrow_cuda.dll` POST_BUILD copies, which this engine-free target does not need.

```cmake
# Chat/session logic is engine-free: it links Catch2 alone, so the suite runs
# without CUDA, Arrow or a GPU.
add_executable(test_lsxhome_chat test_lsxhome_chat.cpp)
target_include_directories(test_lsxhome_chat PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(test_lsxhome_chat PRIVATE Catch2::Catch2WithMain)
```

and next to the other `add_test` calls:

```cmake
add_test(NAME test_lsxhome_chat COMMAND test_lsxhome_chat)
set_tests_properties(test_lsxhome_chat PROPERTIES LABELS fast)
```

- [ ] **Step 2: Write the failing test**

Create `tests/test_lsxhome_chat.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "lsxhome/text_splitter.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <string>
#include <string_view>
#include <thread>

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

// Strict UTF-8 validator: rejects truncated sequences, stray continuation
// bytes and overlong forms. A chunk that carries half a codepoint fails here.
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

}  // namespace

TEST_CASE("lsxhome: UTF-8 chunking never splits a codepoint", "[lsxhome][chat]") {
    const std::string text = u8"Привет, мир! \U0001F30D ok";

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
    REQUIRE(lsxhome::SplitUtf8Chunks(text, chunks, 2, 2) == 2);
    REQUIRE(chunks[0] == "ab");
    REQUIRE(chunks[1] == "cd");

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
    const std::size_t n =
        lsxhome::SplitUtf8Chunks(u8"\U0001F30D", chunks, 4, 2);
    REQUIRE(n == 1);
    REQUIRE(chunks[0].size() == 4);
    REQUIRE(IsValidUtf8(chunks[0]));
}
```

- [ ] **Step 3: Run the test to verify it fails**

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug --target test_lsxhome_chat
```

Expected: build failure — `'SplitUtf8Chunks': is not a member of 'lsxhome'`.

- [ ] **Step 4: Implement the minimal chunker**

Append to `include/lsxhome/text_splitter.h`, before the closing `namespace`:

```cpp
/// Splits @p text into chunks of at most @p max_bytes bytes, never cutting a
/// UTF-8 codepoint in half.
///
/// The `TokenPayload` text field is 32 bytes, and a decoded delta is an
/// arbitrary span of model output, so the delta has to be chunked before it
/// can be published. Cutting on byte boundaries alone would emit half a
/// multibyte character into the UI, so the walk advances one codepoint at a
/// time and only closes a chunk when the next codepoint would not fit.
///
/// Writes at most @p cap views into @p out (slices of @p text, no copying) and
/// returns how many were written. A budget smaller than the 4-byte maximum
/// codepoint is widened to 4: emitting a chunk that is too small to hold a whole
/// character would corrupt the text, and no caller can be served by that.
///
/// A @p cap smaller than needed truncates the remainder — the caller must size
/// @p cap for the input length divided by @p max_bytes, plus one.
inline std::size_t SplitUtf8Chunks(std::string_view text,
                                   std::string_view* out,
                                   std::size_t cap,
                                   std::size_t max_bytes) noexcept {
    if (out == nullptr || cap == 0 || text.empty()) {
        return 0;
    }
    if (max_bytes < 4) {
        max_bytes = 4;
    }
    std::size_t count = 0;
    std::size_t start = 0;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[pos]);
        std::size_t len = 1;
        if ((lead & 0xE0u) == 0xC0u) {
            len = 2;
        } else if ((lead & 0xF0u) == 0xE0u) {
            len = 3;
        } else if ((lead & 0xF8u) == 0xF0u) {
            len = 4;
        }
        // A truncated tail (should not happen for UTF-8 input) still terminates.
        if (pos + len > text.size()) {
            len = text.size() - pos;
        }
        if (pos > start && (pos - start) + len > max_bytes) {
            out[count++] = text.substr(start, pos - start);
            if (count == cap) {
                return count;
            }
            start = pos;
        }
        pos += len;
    }
    if (pos > start && count < cap) {
        out[count++] = text.substr(start, pos - start);
    }
    return count;
}
```

- [ ] **Step 5: Run the test to verify it passes**

```powershell
cmake --build --preset windows-debug --target test_lsxhome_chat
ctest --preset windows-debug -R test_lsxhome_chat
```

Expected: 3 tests pass.

- [ ] **Step 6: Commit**

```powershell
git add include/lsxhome/text_splitter.h tests/test_lsxhome_chat.cpp tests/CMakeLists.txt
git commit -m "feat: add UTF-8-safe chunking and the chat test target"
```

---

### Task 2: `ChatState` — the UI-thread transcript

**Files:**
- Create: `include/lsxhome/chat_state.h`
- Modify: `tests/test_lsxhome_chat.cpp`

- [ ] **Step 1: Write the failing test**

Add to `tests/test_lsxhome_chat.cpp` (above the counting-allocator block, next to the other includes):

```cpp
#include "lsxhome/chat_state.h"
```

and append these cases:

```cpp
TEST_CASE("lsxhome: a turn appends the question and opens an assistant reply",
          "[lsxhome][chat]") {
    lsxhome::ChatState state;
    REQUIRE(state.empty());
    REQUIRE_FALSE(state.Busy());

    REQUIRE(state.BeginTurn("Почему небо голубое?"));
    REQUIRE(state.Busy());
    REQUIRE(state.messages().size() == 2);
    REQUIRE(state.messages()[0].role == "user");
    REQUIRE(state.messages()[0].text == "Почему небо голубое?");
    REQUIRE(state.messages()[1].role == "assistant");
    REQUIRE(state.messages()[1].text.empty());
    REQUIRE_FALSE(state.messages()[1].interrupted);

    state.EndTurn(false);
    REQUIRE_FALSE(state.Busy());
}

TEST_CASE("lsxhome: a turn is refused while empty or already open", "[lsxhome][chat]") {
    lsxhome::ChatState state;
    REQUIRE_FALSE(state.BeginTurn(""));       // nothing to ask
    REQUIRE(state.empty());

    REQUIRE(state.BeginTurn("первый"));
    REQUIRE_FALSE(state.BeginTurn("второй"));  // a reply is still streaming
    REQUIRE(state.messages().size() == 2);

    state.EndTurn(false);
    REQUIRE(state.BeginTurn("второй"));        // the previous turn is closed
    REQUIRE(state.messages().size() == 4);
    REQUIRE(state.messages()[2].text == "второй");
}

TEST_CASE("lsxhome: streamed deltas append in order and ignore stale chunks",
          "[lsxhome][chat]") {
    lsxhome::ChatState state;
    REQUIRE(state.BeginTurn("вопрос"));

    REQUIRE(state.AppendDelta(0, "Привет"));
    REQUIRE(state.AppendDelta(1, ", "));
    REQUIRE(state.AppendDelta(2, "мир"));

    // A replayed or out-of-order chunk must never corrupt the reply text.
    REQUIRE_FALSE(state.AppendDelta(2, "!"));
    REQUIRE_FALSE(state.AppendDelta(0, "?"));
    REQUIRE_FALSE(state.AppendDelta(1, "?"));
    REQUIRE_FALSE(state.AppendDelta(99, "?"));
    REQUIRE(state.messages()[1].text == "Привет, мир");

    // An empty delta is not a chunk.
    REQUIRE_FALSE(state.AppendDelta(3, ""));

    // The seq cursor resets with the turn.
    state.EndTurn(true);
    REQUIRE(state.messages()[1].interrupted);
    REQUIRE(state.BeginTurn("ещё"));
    REQUIRE(state.AppendDelta(0, "Свежий"));
    REQUIRE(state.messages().back().text == "Свежий");
}

TEST_CASE("lsxhome: deltas are refused once the reply is closed", "[lsxhome][chat]") {
    lsxhome::ChatState state;
    REQUIRE(state.BeginTurn("вопрос"));
    REQUIRE(state.AppendDelta(0, "текст"));
    state.EndTurn(false);

    REQUIRE_FALSE(state.AppendDelta(1, "хвост"));
    REQUIRE(state.messages()[1].text == "текст");
    REQUIRE(state.messages().size() == 2);
}

TEST_CASE("lsxhome: the error line is set and cleared independently", "[lsxhome][chat]") {
    lsxhome::ChatState state;
    REQUIRE(state.error().empty());

    state.SetError("модель не задана — передайте --model <путь>");
    REQUIRE(state.error() == "модель не задана — передайте --model <путь>");

    state.ClearError();
    REQUIRE(state.error().empty());

    state.BeginTurn("вопрос");
    state.AppendDelta(0, "текст");
    state.EndTurn(false);
    state.SetError("сбой");
    state.Clear();
    REQUIRE(state.empty());
    REQUIRE(state.error().empty());
    REQUIRE_FALSE(state.Busy());
    REQUIRE(state.BeginTurn("после очистки"));
}
```

- [ ] **Step 2: Run the test to verify it fails**

```powershell
cmake --build --preset windows-debug --target test_lsxhome_chat
```

Expected: build failure — `lsxhome/chat_state.h` not found.

- [ ] **Step 3: Implement the minimal transcript**

Create `include/lsxhome/chat_state.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lsxhome {

/// One rendered conversation turn. `role` is "user" or "assistant";
/// `interrupted` marks a reply that was stopped (or failed) before the model
/// finished it.
struct ChatMessage {
    std::string role;
    std::string text;
    bool interrupted = false;
};

/// The UI thread's view of the conversation: what has been asked, what has been
/// answered, and the last error. Display state only — the worker thread never
/// reads it, so there is no lock and no shared ownership.
///
/// A turn is `BeginTurn(question)` (appends the user's message and opens an
/// empty assistant reply) followed by any number of `AppendDelta(seq, text)`
/// calls and a closing `EndTurn(interrupted)`. Chunks carry a per-turn
/// monotonic sequence number so a replayed or reordered chunk can be rejected
/// instead of corrupting the text.
class ChatState {
public:
    /// Capacity reserved for a reply's text when the turn opens. Sized so a
    /// typical streamed answer appends without reallocating.
    static constexpr std::size_t kReplyReserve = 4096;

    bool empty() const noexcept { return messages_.empty(); }
    bool Busy() const noexcept { return open_reply_; }
    const std::vector<ChatMessage>& messages() const noexcept { return messages_; }
    const std::string& error() const noexcept { return error_; }

    /// Opens a turn. Refused for an empty question or while a reply is still
    /// open — the caller keeps the draft in the input box.
    bool BeginTurn(std::string_view question) {
        if (open_reply_ || question.empty()) {
            return false;
        }
        ChatMessage user;
        user.role = "user";
        user.text = std::string(question);
        messages_.push_back(std::move(user));

        ChatMessage answer;
        answer.role = "assistant";
        answer.text.reserve(kReplyReserve);
        messages_.push_back(std::move(answer));

        open_reply_ = true;
        next_seq_ = 0;
        return true;
    }

    /// Appends one streamed chunk. Only the next expected `seq` is accepted;
    /// duplicates and stale chunks are refused so a replay cannot corrupt text.
    bool AppendDelta(std::uint32_t seq, std::string_view text) {
        if (!open_reply_ || text.empty() || seq != next_seq_) {
            return false;
        }
        messages_.back().text.append(text.data(), text.size());
        ++next_seq_;
        return true;
    }

    /// Closes the open reply. `interrupted` marks a stopped or failed turn.
    void EndTurn(bool interrupted) {
        if (!open_reply_) {
            return;
        }
        messages_.back().interrupted = interrupted;
        open_reply_ = false;
        next_seq_ = 0;
    }

    void SetError(std::string message) { error_ = std::move(message); }
    void ClearError() { error_.clear(); }

    void Clear() {
        messages_.clear();
        error_.clear();
        open_reply_ = false;
        next_seq_ = 0;
    }

private:
    std::vector<ChatMessage> messages_;
    std::string error_;
    std::uint32_t next_seq_ = 0;
    bool open_reply_ = false;
};

}  // namespace lsxhome
```

- [ ] **Step 4: Run the test to verify it passes**

```powershell
cmake --build --preset windows-debug --target test_lsxhome_chat
ctest --preset windows-debug -R test_lsxhome_chat
```

Expected: 8 tests pass.

- [ ] **Step 5: Commit**

```powershell
git add include/lsxhome/chat_state.h tests/test_lsxhome_chat.cpp
git commit -m "feat: add the chat transcript state"
```

---

### Task 3: `GenerationBackend` boundary

**Files:**
- Create: `include/lsxhome/generation_backend.h`
- Modify: `tests/test_lsxhome_chat.cpp` (fake backend + helpers, used by Task 4)

- [ ] **Step 1: Write the fake backend and its harness helpers**

Add to `tests/test_lsxhome_chat.cpp`:

```cpp
#include "lsxhome/chat_session.h"
#include "lsxhome/gui_bridge.h"
#include "lsxhome/generation_backend.h"

#include <mutex>
#include <vector>

namespace {

using lsxhome::ChatPhase;
using lsxhome::ChatTurn;
using lsxhome::GenerationBackend;
using lsxhome::GenerationStatus;

/// Scriptable engine stand-in: no CUDA, no model, no vocabulary. Delays,
/// failures and the abort flag are all observable, which is what the session
/// contract is written against.
class FakeBackend final : public GenerationBackend {
public:
    // Scripted behaviour.
    std::vector<std::string> deltas;             // emitted in order
    GenerationStatus status = GenerationStatus::kOk;
    bool prepare_ok = true;
    std::string prepare_error;
    std::string generate_error;

    // Observable run record (mutex-guarded: written by the worker thread).
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
                aborted_seen.store(true, std::memory_order_relaxed);
                generate_returned.store(true, std::memory_order_relaxed);
                return GenerationStatus::kAborted;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        for (const std::string& delta : deltas) {
            if (abort.load(std::memory_order_acquire)) {
                aborted_seen.store(true, std::memory_order_relaxed);
                generate_returned.store(true, std::memory_order_relaxed);
                return GenerationStatus::kAborted;
            }
            sink(delta);
        }
        generate_returned.store(true, std::memory_order_relaxed);
        if (status == GenerationStatus::kError) {
            out_error = generate_error;
        }
        return status;
    }
};

/// The UI frame's transcript update, mirrored from `DrawChatPanel`: drain
/// first, then close the reply once the worker reports a terminal phase.
void PumpFrame(lsxhome::ChatSession& session, lsxhome::ChatState& state) {
    session.DrainInto(state);
    const ChatPhase phase = session.phase();
    if (state.Busy() && (phase == ChatPhase::kIdle || phase == ChatPhase::kError)) {
        state.EndTurn(phase == ChatPhase::kError ? true : session.TakeInterrupted());
    }
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
```

This step intentionally does not compile yet: `chat_session.h` does not exist. Continue to Step 2 and finish Task 3 with Task 4's header before building.

- [ ] **Step 2: Create the boundary header**

Create `include/lsxhome/generation_backend.h`:

```cpp
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace lsxhome {

/// One framed message handed to the engine. Deliberately not
/// `lsxcommon::ChatMessage`: the session and the UI must not know the engine's
/// types, and the adapter does the conversion.
struct ChatTurn {
    std::string role;  // "user" | "assistant"
    std::string text;
};

enum class GenerationStatus {
    kOk,       // the model finished the answer
    kAborted,  // the caller asked to stop; partial text is valid
    kError,    // failed; `out_error` carries the reason
};

/// The engine boundary the chat session runs against.
///
/// Injected, so the session's queueing, streaming, abort and error contract is
/// testable without a GPU, a checkpoint or a vocabulary. The production
/// implementation is `LsxGenerationBackend` (src/lsx_generation_backend.cpp);
/// tests substitute a scripted fake.
///
/// Threading: every method runs on the session's worker thread, never on the UI
/// thread.
class GenerationBackend {
public:
    /// Receives decoded text deltas as the model produces them.
    using Sink = std::function<void(std::string_view)>;

    virtual ~GenerationBackend() = default;

    /// Brings the model up. Expensive (minutes for a real checkpoint) and
    /// idempotent: the session calls it once and keeps the model loaded for the
    /// remaining turns. Returns false with `out_error` set on failure.
    virtual bool Prepare(std::string& out_error) = 0;

    /// Generates an answer for the framed `history`, streaming text deltas into
    /// `sink` and honouring `abort` between steps. `out_error` is only read
    /// when the status is `kError`.
    virtual GenerationStatus Generate(const std::vector<ChatTurn>& history,
                                     const Sink& sink,
                                     const std::atomic<bool>& abort,
                                     std::string& out_error) = 0;
};

}  // namespace lsxhome
```

- [ ] **Step 3: Commit the boundary (unbuilt until Task 4 lands)**

```powershell
git add include/lsxhome/generation_backend.h
git commit -m "feat: add the injected generation backend boundary"
```

Do not build yet — `tests/test_lsxhome_chat.cpp` is intentionally red until Task 4 adds `chat_session.h`.

---

### Task 4: `ChatSession` — queue, worker, streaming, stop, errors

**Files:**
- Create: `include/lsxhome/chat_session.h`
- Modify: `tests/test_lsxhome_chat.cpp`

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_lsxhome_chat.cpp` (inside the file, after the helpers' anonymous namespace closes):

```cpp
TEST_CASE("lsxhome: a submitted question reaches the backend and streams back",
          "[lsxhome][chat]") {
    lsxhome::GuiBridge bridge;
    FakeBackend backend;
    backend.deltas = {"Привет", ", ", "мир"};

    lsxhome::ChatState state;
    {
        lsxhome::ChatSession session(bridge, backend);
        REQUIRE(session.phase() == ChatPhase::kIdle);
        REQUIRE(session.Submit("привет"));

        PumpUntil(session, state, [&] {
            return !state.Busy() && session.phase() == ChatPhase::kIdle &&
                   state.messages().size() == 2;
        });
        REQUIRE(state.messages()[1].text == "Привет, мир");
        REQUIRE(backend.prepare_calls.load() == 1);
        REQUIRE(backend.generate_calls.load() == 1);

        const auto histories = backend.RecordedHistories();
        REQUIRE(histories.size() == 1);
        REQUIRE(histories[0].size() == 1);
        REQUIRE(histories[0][0].role == "user");
        REQUIRE(histories[0][0].text == "привет");
    }
    // The destructor joined the worker: the last Generate call returned.
    REQUIRE(backend.generate_returned.load());
}

TEST_CASE("lsxhome: the model is prepared once and kept across turns",
          "[lsxhome][chat]") {
    lsxhome::GuiBridge bridge;
    FakeBackend backend;
    backend.deltas = "один";
    backend.deltas = {"один"};

    lsxhome::ChatState state;
    lsxhome::ChatSession session(bridge, backend);

    REQUIRE(session.Submit("первый"));
    PumpUntil(session, state, [&] { return !state.Busy(); });
    REQUIRE(session.phase() == ChatPhase::kIdle);

    REQUIRE(session.Submit("второй"));
    PumpUntil(session, state, [&] { return !state.Busy(); });

    // Multi-turn: the second request carries the whole exchange.
    const auto histories = backend.RecordedHistories();
    REQUIRE(histories.size() == 2);
    REQUIRE(histories[1].size() == 3);
    REQUIRE(histories[1][0].role == "user");
    REQUIRE(histories[1][0].text == "первый");
    REQUIRE(histories[1][1].role == "assistant");
    REQUIRE(histories[1][1].text == "один");
    REQUIRE(histories[1][2].role == "user");
    REQUIRE(histories[1][2].text == "второй");

    // Expensive load happens once, not per question.
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

    REQUIRE(session.Submit("первый"));
    PumpUntil(session, state, [&] { return session.phase() == ChatPhase::kGenerating; });

    // The UI never offers this, and the transcript refuses it too: the reply
    // for the first question is still streaming.
    REQUIRE_FALSE(state.BeginTurn("второй"));

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
        REQUIRE(session.Submit("первый"));
        // The worker popped it and is now blocked inside Generate, so nothing
        // else leaves the queue: capacity is fully deterministic from here.
        PumpUntil(session, state,
                  [&] { return session.phase() == ChatPhase::kGenerating; });

        for (std::size_t i = 0; i < lsxhome::ChatSession::kQueueSlots; ++i) {
            REQUIRE(session.Submit("в очереди"));
        }
        REQUIRE_FALSE(session.Submit("лишний"));

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

    REQUIRE(session.Submit("останови меня"));
    PumpUntil(session, state, [&] { return session.phase() == ChatPhase::kGenerating; });

    session.RequestStop();
    PumpUntil(session, state, [&] {
        return !state.Busy() && session.phase() == ChatPhase::kIdle;
    });

    REQUIRE(backend.aborted_seen.load());
    REQUIRE(session.TakeInterrupted() == false);  // already consumed by the pump
    REQUIRE(state.messages().size() == 2);
    REQUIRE(state.messages()[1].interrupted);
    REQUIRE(state.error().empty());
}

TEST_CASE("lsxhome: a failed prepare surfaces as an error phase", "[lsxhome][chat]") {
    lsxhome::GuiBridge bridge;
    FakeBackend backend;
    backend.prepare_ok = false;
    backend.prepare_error = "модель не задана — передайте --model <путь>";

    lsxhome::ChatState state;
    {
        lsxhome::ChatSession session(bridge, backend);
        REQUIRE(session.Submit("вопрос"));
        PumpUntil(session, state, [&] { return session.phase() == ChatPhase::kError; });

        REQUIRE(session.error() ==
                "модель не задана — передайте --model <путь>");
        REQUIRE(state.error().empty());  // the session error, not the transcript's
        REQUIRE(backend.generate_calls.load() == 0);
    }
}

TEST_CASE("lsxhome: a failed generation closes the reply as interrupted",
          "[lsxhome][chat]") {
    lsxhome::GuiBridge bridge;
    FakeBackend backend;
    backend.deltas = {"часть ответа"};
    backend.status = GenerationStatus::kError;
    backend.generate_error = "KV-cache overflow";

    lsxhome::ChatState state;
    lsxhome::ChatSession session(bridge, backend);

    REQUIRE(session.Submit("вопрос"));
    PumpUntil(session, state, [&] { return session.phase() == ChatPhase::kError; });
    PumpFrame(session, state);

    REQUIRE(session.error() == "KV-cache overflow");
    REQUIRE_FALSE(state.Busy());
    REQUIRE(state.messages()[1].interrupted);
    REQUIRE(state.messages()[1].text == "часть ответа");
}
```

- [ ] **Step 2: Run the test to verify it fails**

```powershell
cmake --build --preset windows-debug --target test_lsxhome_chat
```

Expected: build failure — `lsxhome/chat_session.h` not found.

- [ ] **Step 3: Implement the session**

Create `include/lsxhome/chat_session.h`:

```cpp
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

/// Fixed-size queue slot: the ring is pre-allocated and lock-free, so a queued
/// question must be trivially copyable and never heap-allocated.
struct QueuedPrompt {
    char text[kQueuedPromptCapacity] = {};
    std::uint32_t len = 0;
};

/// Owns the request queue, the worker thread and the answer hand-off.
///
/// Threading contract:
///  - `Submit`, `RequestStop`, `DrainInto`, `TakeInterrupted`, `phase`, `error`
///    are the UI thread's API.
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
        : bridge_(bridge), backend_(backend),
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
        // A fresh turn starts with a clean abort latch; the worker owns it from
        // here on and resets it when the turn ends.
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

    /// UI thread: the last failure, or an empty string.
    std::string error() const {
        std::lock_guard<std::mutex> lock(error_mutex_);
        return error_;
    }

    /// UI thread: reads and clears the "the user stopped this" latch.
    bool TakeInterrupted() noexcept {
        return interrupted_.exchange(false, std::memory_order_acq_rel);
    }

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
        interrupted_.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(error_mutex_);
            error_.clear();
        }
        phase_.store(ChatPhase::kLoading, std::memory_order_release);

        std::string error;
        if (!prepared_) {
            if (!backend_.Prepare(error)) {
                Fail(error.empty() ? std::string("не удалось загрузить модель")
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
                Fail(generate_error.empty() ? std::string("инференс не удался")
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
    std::atomic<bool> abort_{false};
    std::atomic<bool> shutdown_{false};
    std::atomic<bool> interrupted_{false};
    mutable std::mutex error_mutex_;
    std::string error_;
    bool prepared_ = false;  // worker-thread only
};

}  // namespace lsxhome
```

The header code block above is complete and self-contained: `QueuedPrompt` is
sized by the namespace-scope `kQueuedPromptCapacity`, and the class's
`kMaxPromptChars` is only a public alias for it (`= kQueuedPromptCapacity`), so
nothing depends on a member declared further down the file. Copy it verbatim.

- [ ] **Step 4: Run the test to verify it passes**

```powershell
cmake --build --preset windows-debug --target test_lsxhome_chat
ctest --preset windows-debug -R test_lsxhome_chat
```

Expected: 15 tests pass.

- [ ] **Step 5: Commit**

```powershell
git add include/lsxhome/chat_session.h tests/test_lsxhome_chat.cpp
git commit -m "feat: add the chat session queue, worker and streaming hand-off"
```

---

### Task 5: Zero-allocation transcript drain

**Files:**
- Modify: `tests/test_lsxhome_chat.cpp`

- [ ] **Step 1: Write the test**

Append:

```cpp
TEST_CASE("lsxhome: draining streamed chunks never allocates", "[lsxhome][chat]") {
    lsxhome::GuiBridge bridge;
    FakeBackend backend;  // never invoked: the chunks are published directly
    lsxhome::ChatSession session(bridge, backend);

    lsxhome::ChatState state;
    REQUIRE(state.BeginTurn("вопрос"));

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
```

- [ ] **Step 2: Run the test**

```powershell
cmake --build --preset windows-debug --target test_lsxhome_chat
ctest --preset windows-debug -R test_lsxhome_chat
```

Expected: 16 tests pass; if `DrainInto` allocates, the delta is non-zero —
`ChatState::BeginTurn` reserves `kReplyReserve`, so 1024 one-byte chunks fit.

- [ ] **Step 3: Commit**

```powershell
git add tests/test_lsxhome_chat.cpp
git commit -m "test: pin the allocation-free streamed-text drain"
```

---

### Task 6: `LsxGenerationBackend` — the real engine adapter

**Files:**
- Create: `include/lsxhome/lsx_generation_backend.h`
- Create: `src/lsx_generation_backend.cpp`
- Modify: `CMakeLists.txt`

No unit tests: this is the engine/device-side boundary, exactly like the D3D12
work. It is verified by building the executable and by the manual acceptance run
in Task 9.

- [ ] **Step 1: Declare the factory**

Create `include/lsxhome/lsx_generation_backend.h`:

```cpp
#pragma once

#include <memory>
#include <string>

#include "generation_backend.h"

namespace lsxhome {

/// Builds the production backend for the checkpoint at @p model_path (the
/// `--model` flag value). An empty path yields a backend whose `Prepare` fails
/// with a user-facing message instead of crashing at the first question.
std::unique_ptr<GenerationBackend> MakeLsxGenerationBackend(std::string model_path);

}  // namespace lsxhome
```

- [ ] **Step 2: Implement the adapter**

Create `src/lsx_generation_backend.cpp`:

```cpp
// lsxhome — the only translation unit that talks to the logestix engine.
//
// Translates the shell's engine-free vocabulary (ChatTurn / GenerationStatus)
// into lsxcommon calls: model load, chat-template framing, streaming Infer with
// per-token detokenization, and cooperative abort. Everything upstream of this
// file stays free of engine types so the session logic is unit-testable.

#include "lsxhome/lsx_generation_backend.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "lsxcommon/chat_conversation.h"
#include "lsxcommon/incremental_detokenizer.h"
#include "lsxcommon/model.h"
#include "lsxcommon/model_engine.h"
#include "lsxcommon/model_factory.h"

namespace lsxhome {
namespace {

/// Tokens generated per answer. The MVP's chat surface, not a benchmark knob.
constexpr int kMaxGenTokens = 512;

class LsxGenerationBackend final : public GenerationBackend {
public:
    explicit LsxGenerationBackend(std::string model_path)
        : model_path_(std::move(model_path)) {}

    bool Prepare(std::string& out_error) override {
        if (model_) {
            return true;
        }
        if (model_path_.empty()) {
            out_error = "модель не задана — передайте --model <путь>";
            return false;
        }
        // The engine session is reference-counted and is created on the worker
        // thread that drives generation, then torn down with the backend.
        session_ = std::make_unique<lsxcommon::InferenceEngine::Session>();

        lsxcommon::ModelInitConfig cfg;
        cfg.model_path = model_path_;
        cfg.max_gen_tokens = kMaxGenTokens;
        model_ = lsxcommon::LsxModelFactory::Create(cfg);
        if (!model_) {
            out_error = "не удалось открыть модель: " + model_path_;
            session_.reset();
            return false;
        }
        if (!model_->Load()) {
            out_error = "не удалось загрузить модель: " + model_path_;
            model_.reset();
            session_.reset();
            return false;
        }
        return true;
    }

    GenerationStatus Generate(const std::vector<ChatTurn>& history,
                             const Sink& sink,
                             const std::atomic<bool>& abort,
                             std::string& out_error) override {
        if (!model_) {
            out_error = "модель не загружена";
            return GenerationStatus::kError;
        }

        // Chat-template framing: the model's own BuildConversationInputIds
        // renders roles and any system prompt; the generic Hermes fallback
        // flattens the transcript when a family has no native template.
        lsxcommon::ChatConversation conversation;
        conversation.messages.reserve(history.size());
        for (const ChatTurn& turn : history) {
            lsxcommon::ChatMessage message;
            message.role = turn.role;
            message.content = turn.text;
            conversation.messages.push_back(std::move(message));
        }

        std::vector<std::int32_t> input_ids;
        if (!model_->BuildConversationInputIds(model_->Tokenizer(),
                                                conversation, input_ids) ||
            input_ids.empty()) {
            out_error = "не удалось подготовить запрос к модели";
            return GenerationStatus::kError;
        }

        // Per-token detokenization with UTF-8 boundary safety: a delta is
        // always valid text, never half a codepoint.
        auto detokenizer = lsxcommon::IncrementalDetokenizer::ArrowDecoder(
            model_->Tokenizer());

        lsxcommon::ModelRequest request{std::move(input_ids), kMaxGenTokens};
        request.abort_flag = &abort;
        request.token_callback = [&detokenizer, &sink](
                                     const lsxcommon::TokenChunk& chunk) {
            if (chunk.is_special) {
                return;
            }
            const std::string delta = detokenizer.Push(
                static_cast<std::int32_t>(chunk.token_id));
            if (!delta.empty()) {
                sink(delta);
            }
        };

        const lsxcommon::gpu::InferenceResult result = model_->Infer(request);

        const std::string tail = detokenizer.Flush();
        if (!tail.empty()) {
            sink(tail);
        }

        if (abort.load(std::memory_order_acquire)) {
            return GenerationStatus::kAborted;
        }
        if (!result.ok) {
            out_error = result.error_msg.empty()
                            ? std::string("инференс не удался")
                            : result.error_msg;
            return GenerationStatus::kError;
        }
        if (result.output_text.empty()) {
            out_error = "модель вернула пустой ответ";
            return GenerationStatus::kError;
        }
        return GenerationStatus::kOk;
    }

private:
    std::string model_path_;
    std::unique_ptr<lsxcommon::InferenceEngine::Session> session_;
    std::unique_ptr<lsxcommon::IModel> model_;
};

}  // namespace

std::unique_ptr<GenerationBackend> MakeLsxGenerationBackend(std::string model_path) {
    return std::make_unique<LsxGenerationBackend>(std::move(model_path));
}

}  // namespace lsxhome
```

- [ ] **Step 3: Add the source to the executable**

In `CMakeLists.txt`, extend the `add_executable(lsxhome WIN32 ...)` source list:

```cmake
add_executable(lsxhome WIN32
    src/main_win32.cpp
    src/d3d12_renderer.cpp
    src/font_loader.cpp
    src/gui_renderer.cpp
    src/lsx_generation_backend.cpp
)
```

- [ ] **Step 4: Build to check it compiles**

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug
```

Expected: `lsx_generation_backend.cpp` compiles. (`main_win32.cpp` is untouched in this task, so the old one-shot producer still links.)

- [ ] **Step 5: Commit**

```powershell
git add include/lsxhome/lsx_generation_backend.h src/lsx_generation_backend.cpp CMakeLists.txt
git commit -m "feat: adapt the logestix engine to the chat session boundary"
```

---

### Task 7: Chat panel UI

**Files:**
- Modify: `include/lsxhome/gui_renderer.h`
- Modify: `src/gui_renderer.cpp`

No headless harness exists for ImGui rendering (see `AGENTS.md`), so this task
is verified by building plus the manual run in Task 9.

- [ ] **Step 1: Change the interface**

Replace the welcome declaration and the skeleton signature in
`include/lsxhome/gui_renderer.h`:

```cpp
#pragma once

#include "lsxhome/chat_session.h"
#include "lsxhome/chat_state.h"
#include "lsxhome/gui_bridge.h"

struct ImFont;

namespace lsxhome {

/// Applies the high-fidelity matte Blackwell/Cowork theme to the active
/// Dear ImGui style. Hardlocked palette and geometry (see gui_renderer.cpp).
void ApplyBlackwellCoworkTheme() noexcept;

/// Registers the large hero font (FontLoader::LoadHeading) used by the
/// welcome screen. Call once after ImGui context creation.
void SetHeadingFont(ImFont* font) noexcept;

/// Renders the chat panel inside the current content region: the welcome
/// screen while the transcript is empty, then the conversation with the input
/// form pinned to the bottom. Drains @p session into @p state and closes a
/// finished reply. Call inside the ##main child.
void DrawChatPanel(ChatSession& session, ChatState& state) noexcept;

/// Renders the full-viewport edge-to-edge dock root, then the seamless
/// sidebar / main-chat panel blocks hosted inside it.
void BuildWorkspaceSkeleton(ChatSession& session, ChatState& state) noexcept;

}  // namespace lsxhome
```

- [ ] **Step 2: Render the transcript**

In `src/gui_renderer.cpp`, add `#include "lsxhome/chat_state.h"` is already
pulled in by `gui_renderer.h`; add `<cstring>` and `<string>` to the include
block, then add these file-scope helpers next to the existing drawing helpers:

```cpp
// ── Chat panel geometry and state ──────────────────────────────────────────
constexpr float kChatFormHeight = 132.0f;
constexpr float kChatSendWidth  = 96.0f;
constexpr float kChatGap        = 12.0f;

constexpr ImU32 kUserTextU32    = IM_COL32(160, 158, 178, 255);
constexpr ImU32 kStoppedU32     = IM_COL32(148, 148, 158, 255);
constexpr ImU32 kErrorU32       = IM_COL32(226, 108, 92, 255);
constexpr ImU32 kLoadingU32     = IM_COL32(148, 148, 158, 255);

// The question draft survives a refused Submit (busy queue, oversized text), so
// it lives in file scope rather than on the stack.
char g_question[lsxhome::ChatSession::kMaxPromptChars] = "";

void PrefillQuestion(const char* text) noexcept {
    std::snprintf(g_question, sizeof(g_question), "%s", text);
}
```

Add `PrefillQuestion` to `gui_renderer.h` as
`void PrefillQuestion(const char* text) noexcept;` (the quick-action cards need
it), and make `DrawClaudeWelcomeInterface()` `static` in the `.cpp` — it is now
an implementation detail of the chat panel.

Then replace the body of `DrawClaudeWelcomeInterface` so that (a) the
quick-action cards call `PrefillQuestion(labels[i])`, and (b) the input card's
bottom row gains nothing — the form moves to the chat panel. Concretely:

- keep `DrawClaudeWelcomeInterface()` as-is except for the quick-action loop:

```cpp
    for (int i = 0; i < 3; ++i) {
        ImGui::InvisibleButton(quick_action_ids[i], card_size);
        if (ImGui::IsItemClicked()) {
            PrefillQuestion(labels[i]);
        }
        DrawQuickActionCard(dl, ImVec2(cx, ay), card_size, labels[i]);
        cx += card_w + gap;
    }
```

with the ids declared next to `labels`:

```cpp
    static const char* quick_action_ids[] = {"##qa0", "##qa1", "##qa2"};
```

(`InvisibleButton` must come first so the click test precedes the manual
`DrawQuickActionCard` draw; the button is placed at the same cursor position as
the card by saving and restoring the cursor.)

- add the transcript and the form as new functions:

```cpp
void DrawTranscript(const ChatState& state) noexcept {
    for (const ChatMessage& message : state.messages()) {
        const bool is_user = message.role == "user";
        ImGui::PushStyleColor(ImGuiCol_Text,
                              is_user ? kUserTextU32 : kTextU32);
        ImGui::TextUnformatted(is_user ? "Вы" : "LogestiX");
        ImGui::PopStyleColor();
        ImGui::TextWrapped("%s", message.text.c_str());
        if (message.interrupted) {
            ImGui::TextColored(kStoppedU32, "— остановлено");
        }
        ImGui::Separator();
    }
    // Stick to the tail while tokens stream in.
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) {
        ImGui::SetScrollHereY(1.0f);
    }
}

void DrawChatForm(ChatSession& session, ChatState& state) noexcept {
    const ChatPhase phase = session.phase();
    const bool generating = (phase == ChatPhase::kLoading ||
                             phase == ChatPhase::kGenerating);
    const float field_w =
        ImGui::GetContentRegionAvail().x - kChatSendWidth - kChatGap;

    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, kWelcomeRound);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, MakeVec4(0.10f, 0.10f, 0.10f, 1.0f));
    const bool submitted = ImGui::InputTextMultiline(
        "##question", g_question, IM_ARRAYSIZE(g_question),
        ImVec2(field_w, 64.0f),
        ImGuiInputTextFlags_EnterReturnsTrue |
            ImGuiInputTextFlags_NoHorizontalScroll);
    ImGui::PopStyleColor(1);
    ImGui::PopStyleVar(1);

    ImGui::SameLine();
    const bool pressed =
        generating ? ImGui::Button("Стоп", ImVec2(kChatSendWidth, 0.0f))
                   : ImGui::Button("Отправить", ImVec2(kChatSendWidth, 0.0f));

    if (phase == ChatPhase::kLoading) {
        ImGui::TextColored(kLoadingU32, "Загрузка модели…");
    }
    if (!session.error().empty()) {
        ImGui::TextColored(kErrorU32, "%s", session.error().c_str());
    }
    if (!state.error().empty()) {
        ImGui::TextColored(kErrorU32, "%s", state.error().c_str());
    }

    if (generating) {
        if (pressed && phase == ChatPhase::kGenerating) {
            session.RequestStop();
        }
        return;  // one turn at a time
    }
    if (!pressed && !submitted) {
        return;
    }
    if (g_question[0] == '\0') {
        return;
    }
    // Queue first: a refused Submit (full queue) must leave the transcript
    // untouched and keep the draft for the next attempt.
    if (!session.Submit(g_question)) {
        return;
    }
    state.BeginTurn(g_question);
    g_question[0] = '\0';
}
```

`Submit` runs before `BeginTurn` on purpose: `Submit` is the only call that can
be refused (full queue / oversized text), and it must not leave a phantom turn
in the transcript. The reply stays "not open" for the few microseconds between
the two calls, which is exactly what the worker's first `PublishDelta` already
tolerates.

- [ ] **Step 3: Wire the panel into the skeleton**

Add the panel entry point and update the skeleton's signature:

```cpp
void DrawChatPanel(ChatSession& session, ChatState& state) noexcept {
    if (state.empty()) {
        DrawClaudeWelcomeInterface();
        return;
    }
    const float form_y = kChatFormHeight;
    if (ImGui::BeginChild("##transcript", ImVec2(0.0f, -form_y), true,
                          ImGuiChildFlags_Borders)) {
        DrawTranscript(state);
    }
    ImGui::EndChild();

    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowPos().x + 14.0f,
                                     ImGui::GetWindowPos().y +
                                         ImGui::GetWindowSize().y - form_y + 8.0f));
    DrawChatForm(session, state);
}

void BuildWorkspaceSkeleton(ChatSession& session, ChatState& state) noexcept {
    ... // unchanged dock host
    ImGui::BeginChild("##sidebar", ImVec2(kSidebarWidth, 0.0f), false);
    {
        ImGui::TextUnformatted("LogestiX Home");
        ImGui::Separator();
        ImGui::TextUnformatted("Сессия");
        ImGui::Separator();
        // The chat panel is now the bridge's only consumer, so the old
        // "Drain pool" button is gone: it would steal streamed chunks from the
        // transcript. Telemetry stays.
        ImGui::Text("Фаза: %s", PhaseLabel(session.phase()));
        ImGui::Text("Опубликовано: %zu\nОтброшено: %zu\nСлотов: %zu",
                    session.Published(), session.Dropped(),
                    GuiBridge::kPoolSlots);
        if (!session.error().empty()) {
            ImGui::TextColored(kErrorU32, "%s", session.error().c_str());
        }
        if (ImGui::Button("Очистить диалог")) {
            state.Clear();
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("##main", ImVec2(0.0f, 0.0f), false);
    {
        DrawChatPanel(session, state);
    }
    ImGui::EndChild();

    ImGui::End();
}
```

with

```cpp
const char* PhaseLabel(ChatPhase phase) noexcept {
    switch (phase) {
        case ChatPhase::kIdle:       return "свободна";
        case ChatPhase::kLoading:    return "загрузка модели";
        case ChatPhase::kGenerating: return "генерация";
        case ChatPhase::kError:      return "ошибка";
    }
    return "свободна";
}
```

This needs `session.Published()` / `session.Dropped()` accessors — add them to
`ChatSession` next to `phase()`:

```cpp
    /// Telemetry forwarded from the shared bridge (the sidebar shows it).
    std::size_t Published() const noexcept { return bridge_.Published(); }
    std::size_t Dropped() const noexcept { return bridge_.Dropped(); }
```

- [ ] **Step 4: Build**

```powershell
cmake --build --preset windows-debug
```

Expected: failure only in `src/main_win32.cpp` (it still calls the old
`BuildWorkspaceSkeleton(bridge)`) — that is fixed in Task 8. Everything else
must compile.

- [ ] **Step 5: Commit**

```powershell
git add include/lsxhome/gui_renderer.h src/gui_renderer.cpp include/lsxhome/chat_session.h
git commit -m "feat: render the chat transcript, pinned input form and stop control"
```

---

### Task 8: Wire the session into the shell

**Files:**
- Modify: `src/main_win32.cpp`

- [ ] **Step 1: Replace the one-shot producer with a session**

Delete `ComputeProducer` (lines 82-150) and the now-unused
`kMaxWordsPerChunk` constant, and drop the includes that only it needed
(`lsxhome/text_splitter.h`, `lsxcommon/model.h`,
`lsxcommon/model_engine.h`, `lsxcommon/model_factory.h`,
`lsxcommon/prompt_framing.h`, `lsxcommon/log.h`,
`absl/flags/flag.h` stays — it is needed for `absl::GetFlag`). Add:

```cpp
#include "lsxhome/chat_session.h"
#include "lsxhome/chat_state.h"
#include "lsxhome/gui_bridge.h"
#include "lsxhome/gui_renderer.h"
#include "lsxhome/lsx_generation_backend.h"
```

Keep `<absl/flags/parse.h>`, `<string>`, `<vector>`.

Then replace the startup block that spawned the thread:

```cpp
    lsxhome::GuiBridge bridge;
    auto backend = lsxhome::MakeLsxGenerationBackend(model_path);
    lsxhome::ChatSession session(bridge, *backend);
    lsxhome::ChatState chat;

    // --run keeps its CLI contract: the resolved prompt is the first chat turn.
    // With no --model there is nothing to run, so the deterministic GLM-5.2
    // self-check token (11751 -> "Paris") is published as the answer instead of
    // starting a load that cannot succeed.
    const bool run_requested = !absl::GetFlag(FLAGS_run).empty();
    if (run_requested) {
        if (model_path.empty()) {
            const lsxhome::TokenPayload self_check =
                lsxhome::MakeFallbackTokenPayload(run_prompt, false);
            if (chat.BeginTurn(run_prompt)) {
                chat.AppendDelta(self_check.stream_seq, self_check.text);
                chat.EndTurn(false);
            }
        } else {
            if (session.Submit(run_prompt)) {
                chat.BeginTurn(run_prompt);
            }
        }
    }
```

and the frame body:

```cpp
        // Edge-to-edge docking root + chat panel; the panel drains the bridge
        // into the transcript every frame.
        lsxhome::BuildWorkspaceSkeleton(session, chat);
```

Delete the now-unused `GuiBridge&` parameter plumbing in
`BuildWorkspaceSkeleton`'s body (the `bridge.DrainAll` debug button) — covered
in Task 7.

- [ ] **Step 2: Build**

```powershell
cmake --build --preset windows-debug
```

Expected: clean build.

- [ ] **Step 3: Run the whole fast suite**

```powershell
ctest --preset windows-debug
```

Expected: `test_lsxhome_gui`, `test_lsxhome_font`, `test_lsxhome_chat`,
`patch_drift_guard` all pass.

- [ ] **Step 4: Commit**

```powershell
git add src/main_win32.cpp
git commit -m "refactor: drive the shell from the chat session instead of the one-shot producer"
```

---

### Task 9: Manual acceptance + documentation sync

**Files:**
- Modify: `README.md`
- Modify: `AGENTS.md`

- [ ] **Step 1: Manual acceptance run**

```powershell
& build/windows-debug/bin/lsxhome.exe --model <путь-к-конвертированной-модели>
```

Verify, in order:

1. The welcome screen appears with an empty transcript.
2. Typing a question and pressing Enter (or «Отправить») opens a user turn and
   an empty assistant turn; the phase reads «загрузка модели» while the
   checkpoint loads, then «генерация».
3. The answer streams into the transcript as the model decodes.
4. «Стоп» interrupts: the reply is marked «— остановлено».
5. A second question is answered with the first exchange in context.
6. Launching without `--model` and pressing «Отправить» shows the
   «модель не задана — передайте --model <путь>» error instead of crashing.
7. `lsxhome.exe --run "текст"` still submits that prompt automatically, and
   `lsxhome.exe --run` with no `--model` still shows the «Paris» self-check.

- [ ] **Step 2: Update `README.md`**

- Layout block: add `chat_state, chat_session, generation_backend,
  lsx_generation_backend` to the `include/lsxhome/` list and
  `lsx_generation_backend` to the `src/` list.
- Tests line: `tests/ test_lsxhome_chat, test_lsxhome_font, test_lsxhome_gui
  (label fast)`.

- [ ] **Step 3: Update `AGENTS.md`**

- Structure section: add the four new headers and `src/lsx_generation_backend.cpp`
  to the `src/` / `include/lsxhome/` lists.
- Test section: describe `test_lsxhome_chat` — `SplitUtf8Chunks` boundary
  safety, transcript turn/seq rules, session queue/backpressure/abort/error
  contract against a fake backend, and the allocation-free drain.
- Conventions: note that the chat path is engine-free by construction
  (`chat_state.h` / `chat_session.h` must not include `lsxcommon` or ImGui), so
  the suite runs without CUDA.

- [ ] **Step 4: Final full-suite verification**

```powershell
cmake --build --preset windows-debug
ctest --preset windows-debug
```

Expected: every `fast`-labelled test passes.

- [ ] **Step 5: Commit**

```powershell
git add README.md AGENTS.md
git commit -m "docs: document the chat MVP layout, tests and engine-free boundary"
```

---

## Self-Review

**Spec coverage**

| Spec requirement | Task |
|---|---|
| `SplitUtf8Chunks`, no lost text, no split codepoint | 1 |
| `ChatState` turn rules, seq cursor, error line | 2 |
| Injected `GenerationBackend` boundary | 3 |
| Queue, `Prepare` once, phases, streaming, multi-turn history, busy/queue-full refusal, Stop, error paths, destructor join | 4 |
| Zero-allocation drain | 5 |
| `LsxGenerationBackend` (session, factory, `BuildConversationInputIds`, streaming `Infer`, detokenizer, abort) | 6 |
| Chat panel: welcome → transcript, pinned form, Send/Stop, Enter submits, quick-action prefill, sidebar telemetry, Drain button removed | 7 |
| `--run` auto-submit + Paris self-check, shell wiring | 8 |
| Docs sync, acceptance | 9 |

**Consistency:** `ChatPhase`, `ChatTurn`, `GenerationStatus`,
`GenerationBackend::Sink`, `ChatSession::kQueueSlots`, `kMaxPromptChars`,
`TakeInterrupted`, `DrainInto`, `PrefillQuestion` and `kQueuedPromptCapacity`
are each introduced once and used under the same name everywhere.
`QueuedPrompt` is sized by the namespace-scope `kQueuedPromptCapacity` and
`ChatSession::kMaxPromptChars` aliases it, so the struct never depends on a
class member declared later in the file.

**Known deviations to keep in mind while implementing**
- `PumpFrame` (tests) mirrors the frame logic that lives in `DrawChatPanel`;
  keep them in sync if the close rule changes.
- Task 3 leaves the tree intentionally unbuildable until Task 4; do not run a
  build between those two steps.
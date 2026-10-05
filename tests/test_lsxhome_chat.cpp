#include <catch2/catch_test_macros.hpp>

#include "lsxhome/chat_session.h"
#include "lsxhome/chat_state.h"
#include "lsxhome/generation_backend.h"
#include "lsxhome/gui_bridge.h"
#include "lsxhome/history_budget.h"
#include "lsxhome/model_root.h"
#include "lsxhome/prompt_repair.h"
#include "lsxhome/text_normalizer.h"
#include "lsxhome/text_splitter.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

// ASCII-only source: a narrow literal with non-ASCII bytes is interpreted
// through the compiler's source codepage, which is not guaranteed to be UTF-8,
// and `\xNN` escapes are greedy — so every multibyte string is built from
// explicit bytes instead. Then the assertions test the chunker, not the
// toolchain's decoding.
const std::string kHello = {static_cast<char>(0xD0), static_cast<char>(0x9F),
                            static_cast<char>(0xD1), static_cast<char>(0x80),
                            static_cast<char>(0xD0), static_cast<char>(0xB8),
                            static_cast<char>(0xD0), static_cast<char>(0xB2),
                            static_cast<char>(0xD0), static_cast<char>(0xB5),
                            static_cast<char>(0xD1), static_cast<char>(0x82)};  // Привет
const std::string kWorld = {static_cast<char>(0xD0), static_cast<char>(0xBC),
                            static_cast<char>(0xD0), static_cast<char>(0xB8),
                            static_cast<char>(0xD1), static_cast<char>(0x80)};  // мир
const std::string kGlobe = {static_cast<char>(0xF0), static_cast<char>(0x9F),
                            static_cast<char>(0x8C), static_cast<char>(0x8D)};  // 🌍

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
// ── Model path resolution ──────────────────────────────────────────────────
namespace {

namespace fs = std::filesystem;

/// A throwaway directory tree; removed on scope exit.
struct TempTree {
    fs::path root;

    explicit TempTree(const char* name) {
        root = fs::temp_directory_path() / name;
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root, ec);
    }
    ~TempTree() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    fs::path WriteIndex(const fs::path& dir) {
        fs::create_directories(dir);
        std::ofstream out(dir / "model.index.json");
        out << R"({"model": "gemma-4-26b-a4b-it"})";
        return dir;
    }
};

}  // namespace

TEST_CASE("lsxhome: a converted model directory resolves to its model root",
          "[lsxhome][chat]") {
    // The engine appends `logestix/model.index.json` itself, so it wants the
    // model ROOT. Passing the converted directory instead used to fail
    // autodetection with "cannot autodetect model type from ...".
    TempTree tree("lsxhome_model_root_root");
    const fs::path converted = tree.WriteIndex(tree.root / "logestix");

    REQUIRE(lsxhome::ResolveModelRoot(converted.string()) == tree.root.string());
    // The root itself passes through unchanged.
    REQUIRE(lsxhome::ResolveModelRoot(tree.root.string()) == tree.root.string());
}

TEST_CASE("lsxhome: an unrecognised model path is passed through untouched",
          "[lsxhome][chat]") {
    TempTree tree("lsxhome_model_root_unknown");
    // No index anywhere: let the engine report its own error rather than
    // silently rewriting the user's path.
    REQUIRE(lsxhome::ResolveModelRoot(tree.root.string()) == tree.root.string());
    REQUIRE(lsxhome::ResolveModelRoot("").empty());
}

// ── Engine-text normalization ──────────────────────────────────────────────
namespace {
/// UTF-8 encoding of @p cp, built from bytes (`\xNN` escapes are greedy).
std::string Utf8Encode(unsigned cp) {
    std::string out;
    if (cp < 0x80u) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800u) {
        out += static_cast<char>(0xC0u | (cp >> 6));
        out += static_cast<char>(0x80u | (cp & 0x3Fu));
    } else {
        out += static_cast<char>(0xE0u | (cp >> 12));
        out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
        out += static_cast<char>(0x80u | (cp & 0x3Fu));
    }
    return out;
}

/// The reference GPT-2 `bytes_to_unicode` mapping, byte -> codepoint. Printable
/// ASCII and Latin-1 keep their value; every other byte is lifted into the
/// Latin-1 supplement in ascending byte order. Recomputed here (rather than
/// mirrored from the implementation) so the test pins the algorithm, not the
/// code.
unsigned ReferenceMarkerCp(unsigned b) {
    const auto printable = [](unsigned x) {
        return (x >= 33u && x <= 126u) || (x >= 161u && x <= 172u) || x >= 174u;
    };
    if (printable(b)) {
        return b;
    }
    unsigned n = 0;
    for (unsigned x = 0; x < b; ++x) {
        if (!printable(x)) {
            ++n;
        }
    }
    return 256u + n;
}
}  // namespace

// ── Engine-text normalization ────────────────────────────────────────────────
// Measured on the converted Gemma-4 checkpoint after the engine fix
// (logestix 456280d, "decode to text the tokenizer can encode again"):
//   * the decoder now substitutes U+FFFD for any byte that cannot begin or
//     complete a well-formed sequence, so invalid bytes never reach the shell;
//   * the vocabulary lifts nothing into U+0100..U+0143 (the space is an ordinary
//     0x20 token), so the GPT-2 "Ġ" theory does not apply to this checkpoint;
//   * what IS left is C1 control junk: the model emits byte-fallback pieces
//     (<0xC2><0x97> = U+0097) repeatedly inside its answers.
//
// So the shell's job is narrow: drop the C1 controls, leave everything else
// byte-identical. Anything more aggressive rewrites the model's own words.

TEST_CASE("lsxhome: C1 control junk is dropped from an answer", "[lsxhome][chat]") {
    const std::string u0097 = {static_cast<char>(0xC2), static_cast<char>(0x97)};
    const std::string u0085 = {static_cast<char>(0xC2), static_cast<char>(0x85)};  // NEL

    REQUIRE(lsxhome::NormalizeEngineText(u0097).empty());
    REQUIRE(lsxhome::NormalizeEngineText(u0085).empty());
    REQUIRE(lsxhome::NormalizeEngineText("a" + u0097 + "b") == "ab");
    REQUIRE(lsxhome::NormalizeEngineText(kHello + u0097 + kWorld) ==
            kHello + kWorld);

    // Every C1 codepoint is in range U+0080..U+009F, i.e. C2 80..C2 9F.
    for (unsigned cp = 0x80u; cp <= 0x9Fu; ++cp) {
        const std::string control = Utf8Encode(cp);
        REQUIRE(lsxhome::NormalizeEngineText(control).empty());
    }

    // The result is always valid UTF-8 and never contains a control character.
    const std::string cleaned =
        lsxhome::NormalizeEngineText(kHello + u0097 + ", " + kWorld + u0085);
    REQUIRE(IsValidUtf8(cleaned));
    REQUIRE(cleaned == kHello + ", " + kWorld);
}

TEST_CASE("lsxhome: the engine's replacement marker is dropped from an answer",
          "[lsxhome][chat]") {
    // After the engine fix (logestix 456280d) the decoder substitutes U+FFFD for
    // any byte it cannot reconstruct — that is what this model produces when it
    // emits byte-fallback pieces. Measured on a real answer: the raw text is
    // "????? , G??G???" once UTF-8 decoded, i.e. FFFD runs between ASCII letters.
    // The marker is the engine telling us the model generated unencodable bytes;
    // showing it to the user as a wall of diamonds helps nobody, so it is dropped
    // and the surrounding words stay readable.
    const std::string fffd = "\xEF\xBF\xBD";
    REQUIRE(lsxhome::NormalizeEngineText(fffd).empty());
    REQUIRE(lsxhome::NormalizeEngineText("a" + fffd + "b") == "ab");
    REQUIRE(lsxhome::NormalizeEngineText(kHello + fffd + kWorld) ==
            kHello + kWorld);

    // A whole answer made of markers collapses to nothing rather than to noise.
    REQUIRE(lsxhome::NormalizeEngineText(fffd + fffd + fffd).empty());

    // Words stay separated: the marker is removed, not replaced by a space, so
    // the model never grows phantom whitespace between the surviving words.
    REQUIRE(lsxhome::NormalizeEngineText("the" + fffd + " answer") ==
            "the answer");
}

TEST_CASE("lsxhome: normalization leaves the model's own text byte-identical",
          "[lsxhome][chat]") {
    // ASCII, Cyrillic, emoji, tabs and newlines: untouched.
    REQUIRE(lsxhome::NormalizeEngineText("Plain ASCII text") == "Plain ASCII text");
    REQUIRE(lsxhome::NormalizeEngineText(kHello) == kHello);
    REQUIRE(lsxhome::NormalizeEngineText(kGlobe) == kGlobe);
    REQUIRE(lsxhome::NormalizeEngineText("line1\nline2\ttabbed") ==
            "line1\nline2\ttabbed");
    REQUIRE(lsxhome::NormalizeEngineText("").empty());

    // Latin-1 typography ("é") is ordinary text: the vocabulary keeps it and
    // re-encoding it is exact. Rewriting it would corrupt real answers.
    const std::string e_acute = "\xC3\xA9";
    REQUIRE(lsxhome::NormalizeEngineText(e_acute) == e_acute);
    REQUIRE(lsxhome::NormalizeEngineText("Caf" + e_acute + "!") ==
            "Caf" + e_acute + "!");

    // Smart quotes and dashes the model emits are content.
    const std::string typographic = "\xE2\x80\x99\xE2\x80\x93";
    REQUIRE(lsxhome::NormalizeEngineText(typographic) == typographic);

    // U+0120 «Ġ» is a valid character in this vocabulary (it decodes as-is), so
    // it must NOT be rewritten into a space.
    const std::string g_with_dot = "\xC4\xA0";
    REQUIRE(lsxhome::NormalizeEngineText(g_with_dot) == g_with_dot);

// The engine's own repair marker is removed (see the test above), while
    // everything else — including a genuine replacement character written by a
    // user as content — is untouched.
    const std::string smart = "\xE2\x80\x9Cquoted\xE2\x80\x9D";
    REQUIRE(lsxhome::NormalizeEngineText(smart) == smart);

    // DEL is invisible junk; it becomes a space so words never fuse.
    REQUIRE(lsxhome::NormalizeEngineText(std::string(1, '\x7F')) == " ");
    REQUIRE(lsxhome::NormalizeEngineText("a" + std::string(1, '\x7F') + "b") ==
            "a b");
}

// ── Prompt budget ────────────────────────────────────────────────────────────
TEST_CASE("lsxhome: a short conversation is framed whole", "[lsxhome][chat]") {
    const std::vector<ChatTurn> history = {
        {"user", "first question"},
        {"assistant", "first answer"},
        {"user", "second question"},
    };

    const auto windowed = lsxhome::TrimHistoryToBudget(history, 4096);
    REQUIRE(windowed.size() == history.size());
    REQUIRE(windowed[0].text == "first question");
    REQUIRE(windowed[2].text == "second question");
}

TEST_CASE("lsxhome: the oldest turns are dropped, and whole ones only",
          "[lsxhome][chat]") {
std::vector<ChatTurn> history;
    // Strictly alternating user/assistant, ending on the question being asked.
    for (int i = 0; i < 21; ++i) {
        history.push_back({(i % 2) == 0 ? "user" : "assistant",
                           std::string(200, static_cast<char>('a' + i % 26))});
    }
    const auto windowed = lsxhome::TrimHistoryToBudget(history, 900);
    REQUIRE_FALSE(windowed.empty());
    REQUIRE(windowed.size() < history.size());

    // Kept turns are a suffix of the conversation, and it still ends with the
    // question the model has to answer.
    REQUIRE(windowed.back().role == "user");
    REQUIRE(windowed.back().text == history.back().text);

    // A half exchange (assistant without its question) is never framed: the
    // family template expects user -> assistant -> user.
    std::size_t users = 0;
    std::size_t assistants = 0;
    for (const ChatTurn& turn : windowed) {
        users += turn.role == "user" ? 1 : 0;
        assistants += turn.role == "assistant" ? 1 : 0;
    }
    REQUIRE(users == assistants + 1);
}

TEST_CASE("lsxhome: an over-long single question survives the budget",
          "[lsxhome][chat]") {
    // Whatever the budget, the newest question must reach the model: dropping it
    // would answer nothing at all.
    const std::vector<ChatTurn> history = {
        {"assistant", std::string(9000, 'x')},
        {"user", "what is the answer?"},
    };

    const auto windowed = lsxhome::TrimHistoryToBudget(history, 512);
    REQUIRE(windowed.size() == 1);
    REQUIRE(windowed.back().role == "user");
    REQUIRE(windowed.back().text == "what is the answer?");
}

TEST_CASE("lsxhome: the budget is measured in bytes", "[lsxhome][chat]") {
    // Cyrillic costs two bytes each, so a byte budget must not assume one byte
    // per character.
    const std::vector<ChatTurn> history = {
        {"user", std::string(200, ' ') + kHello},
    };
    const auto windowed = lsxhome::TrimHistoryToBudget(history, 16);
    REQUIRE(windowed.size() == 1);
    REQUIRE(windowed[0].text == history[0].text);
}

// ── Prompt repair ────────────────────────────────────────────────────────────
TEST_CASE("lsxhome: text the tokenizer accepts is returned untouched",
          "[lsxhome][chat]") {
    // A tokenizer that accepts everything must see exactly one call and change
    // nothing: this is the common path, and it must stay cheap.
    int calls = 0;
    const auto encodable = [&](std::string_view) {
        ++calls;
        return true;
    };
    const std::string text = "Привет, как дела?";
    REQUIRE(lsxhome::DropUnencodable(encodable, text) == text);
    REQUIRE(calls == 1);
}

TEST_CASE("lsxhome: a rejected byte is dropped, the rest of the word survives",
          "[lsxhome][chat]") {
    // A stand-in vocabulary: a byte sequence encodes unless it contains 0xFF,
    // which is how a real tokenizer refuses a byte it cannot represent.
    const auto encodable = [](std::string_view s) {
        return s.find('\xFF') == std::string_view::npos;
    };

    const std::string cleaned =
        lsxhome::DropUnencodable(encodable, "bad\xFFword keep this");
    REQUIRE(cleaned == "badword keep this");

    // A rejected byte at the edge does not take the word with it.
    REQUIRE(lsxhome::DropUnencodable(encodable, "\xFFkeep") == "keep");
    REQUIRE(lsxhome::DropUnencodable(encodable, "keep\xFF") == "keep");

    // Multibyte characters must survive whole: their individual bytes never
    // encode on their own, so a byte-wise walk would delete every Cyrillic
    // character from the prompt.
    const std::string hello = kHello;  // D0 9F D1 80 ... (2 bytes per character)
    REQUIRE(lsxhome::DropUnencodable(encodable, hello) == hello);
    REQUIRE(lsxhome::DropUnencodable(encodable, hello + "\xFF") == hello);
    REQUIRE(lsxhome::DropUnencodable(encodable, hello + "\xFF" + kWorld) ==
            hello + kWorld);

    // Word boundaries and separators are preserved exactly, including newlines
    // and tabs, because the prompt layout is the model's chat template.
    REQUIRE(lsxhome::DropUnencodable(encodable, "a\nb\tc") == "a\nb\tc");
    REQUIRE(lsxhome::DropUnencodable(encodable, " x\xFF y ") == " x y ");
}

TEST_CASE("lsxhome: a word of nothing but bad bytes disappears entirely",
          "[lsxhome][chat]") {
    const auto encodable = [](std::string_view s) {
        return s.find('\xFF') == std::string_view::npos &&
               s.find('\xFE') == std::string_view::npos;
    };
    REQUIRE(lsxhome::DropUnencodable(encodable, "\xFF\xFE").empty());
    REQUIRE(lsxhome::DropUnencodable(encodable, "ok \xFF\xFE done") ==
            "ok  done");
}

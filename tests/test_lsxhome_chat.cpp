#include <catch2/catch_test_macros.hpp>

#include "lsxhome/chat_session.h"
#include "lsxhome/chat_state.h"
#include "lsxhome/generation_backend.h"
#include "lsxhome/gui_bridge.h"
#include "lsxhome/model_root.h"
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

/// Inverse of `ReferenceMarkerCp`: which byte a lifted codepoint stands for.
unsigned ReferenceInverseByte(unsigned cp) {
    for (unsigned b = 0; b < 256u; ++b) {
        if (ReferenceMarkerCp(b) == cp) {
            return b;
        }
    }
    return 0u;
}
}  // namespace

TEST_CASE("lsxhome: byte-level markers decode back to their bytes",
          "[lsxhome][chat]") {
    // Control and ASCII bytes travel as lifted markers.
    REQUIRE(lsxhome::NormalizeEngineText(
                Utf8Encode(ReferenceMarkerCp(0x20u))) == " ");   // space
    REQUIRE(lsxhome::NormalizeEngineText(
                Utf8Encode(ReferenceMarkerCp(0x09u))) == "\t");  // tab
    REQUIRE(lsxhome::NormalizeEngineText(
                Utf8Encode(ReferenceMarkerCp(0x0Au))) == "\n");  // newline

    // Printable bytes are NOT lifted: their marker is the character itself, and
    // it must pass through untouched (GPT-2 keeps 0x21..0x7E and the printable
    // Latin-1 range as-is).
    REQUIRE(lsxhome::NormalizeEngineText("Hello, world!") == "Hello, world!");
    REQUIRE(lsxhome::NormalizeEngineText(
                Utf8Encode(ReferenceMarkerCp(0xABu))) ==
            Utf8Encode(ReferenceMarkerCp(0xABu)));

    // A lifted control byte that is not printable whitespace becomes a space, so
    // the transcript never carries an invisible control character.
    REQUIRE(lsxhome::NormalizeEngineText(
                Utf8Encode(ReferenceMarkerCp(0x01u))) == " ");
}

TEST_CASE("lsxhome: C1 control characters never reach the transcript", "[lsxhome][chat]") {
    // The model emits U+0097 (a C1 control) between words. It is valid UTF-8,
    // so a structural validator keeps it — but it is invisible junk, and the
    // model itself flags the text as "wrong encoding" when it reads its own
    // transcript back. Dropped here.
    const std::string c1 = {static_cast<char>(0xC2), static_cast<char>(0x97)};
    REQUIRE(lsxhome::NormalizeEngineText(kHello + c1 + kWorld) == kHello + kWorld);
    // DEL is invisible junk too; it becomes a space rather than vanishing, so
    // it never splits two words together.
    REQUIRE(lsxhome::NormalizeEngineText(std::string(1, '\x7F')) == " ");

    // A C1 control next to ordinary text is removed without touching the text.
    REQUIRE(lsxhome::NormalizeEngineText("ok" + c1 + " done") == "ok done");
    REQUIRE(IsValidUtf8(lsxhome::NormalizeEngineText("ok" + c1 + " done")));

    // Real typography must survive: U+2019 (') is not a control.
    const std::string apostrophe = "\xE2\x80\x99";
    REQUIRE(lsxhome::NormalizeEngineText(kWorld + apostrophe) ==
            kWorld + apostrophe);
}

TEST_CASE("lsxhome: latin-1 characters are left to the vocabulary", "[lsxhome][chat]") {
    // A printable Latin-1 byte keeps its character in a byte-level vocabulary,
    // so "é" is ordinary text that round-trips through the tokenizer unchanged.
    // Deciding otherwise would be guessing: U+00E9 as text and byte 0xE9 as a
    // vocabulary piece are the same bytes, and only the converted model's
    // tokenizer can tell them apart.
    const std::string e_acute = "\xC3\xA9";
    REQUIRE(IsValidUtf8(e_acute));
    REQUIRE(lsxhome::NormalizeEngineText(e_acute) == e_acute);
    REQUIRE(lsxhome::NormalizeEngineText("Caf" + e_acute + "!") ==
            "Caf" + e_acute + "!");

    // A marker pair is NOT text: those bytes start no valid sequence on their
    // own, so they are restored (or dropped) instead of being shown.
    REQUIRE(lsxhome::NormalizeEngineText(
                Utf8Encode(ReferenceMarkerCp(0x20u)) + e_acute) == " " + e_acute);
}

TEST_CASE("lsxhome: stray bytes that cannot form UTF-8 are dropped", "[lsxhome][chat]") {
    // The engine's decoder hands back raw vocabulary bytes, so an answer can
    // contain a lone high byte that starts no UTF-8 sequence. Framing that back
    // into the next prompt fails outright (the tokenizer rejects malformed
    // UTF-8), so normalization must not pass it on.
    const std::string lone = {static_cast<char>(0xAB)};
    REQUIRE(lsxhome::NormalizeEngineText(lone).empty());
    REQUIRE(lsxhome::NormalizeEngineText(std::string(1, '\xC4')).empty());

    // A broken sequence around real text is repaired, not propagated.
    const std::string mixed = std::string(kHello) + lone +
                              Utf8Encode(ReferenceMarkerCp(0x20u)) + kWorld;
    const std::string fixed = lsxhome::NormalizeEngineText(mixed);
    REQUIRE(IsValidUtf8(fixed));
    REQUIRE(fixed == std::string(kHello) + " " + kWorld);

    // A truncated multibyte tail (the model's last token cut off) is dropped.
    const std::string truncated = std::string(kHello) + "\xD0";
    const std::string repaired = lsxhome::NormalizeEngineText(truncated);
    REQUIRE(IsValidUtf8(repaired));
    REQUIRE(repaired == kHello);
}

TEST_CASE("lsxhome: a marker pair rebuilds one UTF-8 character", "[lsxhome][chat]") {
    // A lifted pair must reassemble into the character it encodes; a stray
    // continuation byte next to real text is removed without touching the text.
    const std::string with_quote = Utf8Encode(ReferenceMarkerCp(0x20u)) +
                                   Utf8Encode(0x00C2u) +  // "«"
                                   kHello +
                                   Utf8Encode(ReferenceMarkerCp(0x20u));
    const std::string decoded = lsxhome::NormalizeEngineText(with_quote);
    REQUIRE(IsValidUtf8(decoded));
    REQUIRE(decoded == " " + std::string(Utf8Encode(0x00C2u)) + kHello + " ");

    // The whole answer must stay valid UTF-8 after normalization.
    const std::string text = Utf8Encode(0x00C2u) + kHello +
                             Utf8Encode(ReferenceMarkerCp(0x20u));
    REQUIRE(IsValidUtf8(lsxhome::NormalizeEngineText(text)));
}

TEST_CASE("lsxhome: marker ranges decode to readable text",
          "[lsxhome][chat]") {
    // The engine's decoder returns raw vocabulary bytes, so a generated answer
    // arrives with no ordinary space and no ordinary newline — just U+0120 and
    // U+010A. Framing that answer back into the next prompt then fails twice
    // over: the vocabulary sees one 500+ byte "word" (kMaxWordBytes is 512), and
    // the model reads the glyphs as mojibake ("wrong encoding").
    REQUIRE(lsxhome::NormalizeEngineText(
                Utf8Encode(0x0120)) == " ");  // U+0120 -> space
    REQUIRE(lsxhome::NormalizeEngineText(
                Utf8Encode(0x010A)) == "\n");  // U+010A -> newline
    REQUIRE(lsxhome::NormalizeEngineText(
                Utf8Encode(0x0109)) == "\t");  // U+0109 -> tab
    // The whole lifted range must decode to a single character/byte, never to
    // the raw glyph that would otherwise reach the prompt.
    for (unsigned cp = 0x0100u; cp <= 0x0143u; ++cp) {
        const std::string marker = Utf8Encode(cp);
        const std::string decoded = lsxhome::NormalizeEngineText(marker);
        INFO("codepoint=0x" << std::hex << cp << " marker_size=" << marker.size()
                            << " decoded_size=" << decoded.size());
        // High bytes that cannot start a UTF-8 sequence on their own are dropped
        // (they are continuation bytes whose lead byte lives elsewhere).
        const unsigned byte = ReferenceInverseByte(cp);
        if (byte > 0x7Fu) {
            REQUIRE(decoded.empty());
        } else {
            REQUIRE(decoded.size() == 1u);
        }
        REQUIRE(decoded != marker);  // never the glyph itself
    }

    // A full sentence of markers turns back into readable text with real word
    // boundaries.
    const std::string sentence =
        Utf8Encode(0x0120) + std::string(kHello) + Utf8Encode(0x0120) +
        std::string(kWorld) + Utf8Encode(0x010A) + Utf8Encode(0x010A) +
        std::string(kWorld);
    const std::string normalized = lsxhome::NormalizeEngineText(sentence);
    REQUIRE(normalized ==
            std::string(" ") + kHello + " " + kWorld + "\n\n" + kWorld);

    std::size_t longest = 0;
    std::size_t run = 0;
    for (const char ch : normalized) {
        run = (ch == ' ') ? 0 : run + 1;
        longest = std::max(longest, run);
    }
    REQUIRE(longest < 64);
}

TEST_CASE("lsxhome: non-breaking spaces become plain spaces", "[lsxhome][chat]") {
    // The engine's vocabulary has no U+00A0, so a model that emits it makes
    // arrow_tokenizer_encode fail and the whole next prompt is rejected.
    const std::string nbsp = {static_cast<char>(0xC2),
                              static_cast<char>(0xA0)};  // U+00A0
    const std::string narrow = {static_cast<char>(0xE2),
                                static_cast<char>(0x80),
                                static_cast<char>(0xAF)};  // U+202F
    const std::string figure = {static_cast<char>(0xE2),
                                static_cast<char>(0x80),
                                static_cast<char>(0x87)};  // U+2007
    const std::string thin = {static_cast<char>(0xE2),
                              static_cast<char>(0x80),
                              static_cast<char>(0x89)};  // U+2009
    const std::string em = {static_cast<char>(0xE3),
                            static_cast<char>(0x80),
                            static_cast<char>(0x80)};  // U+3000

    REQUIRE(lsxhome::NormalizeEngineText(kHello + nbsp + kWorld) ==
            std::string(kHello) + " " + kWorld);
    REQUIRE(lsxhome::NormalizeEngineText(kHello + narrow + kWorld) ==
            std::string(kHello) + " " + kWorld);
    REQUIRE(lsxhome::NormalizeEngineText(kHello + figure + kWorld) ==
            std::string(kHello) + " " + kWorld);
    REQUIRE(lsxhome::NormalizeEngineText(kHello + thin + kWorld) ==
            std::string(kHello) + " " + kWorld);
    REQUIRE(lsxhome::NormalizeEngineText(kHello + em + kWorld) ==
            std::string(kHello) + " " + kWorld);

    // Only the characters are rewritten, never collapsed: two exotic spaces
    // stay two spaces, so the model's own spacing survives untouched.
    REQUIRE(lsxhome::NormalizeEngineText(kHello + nbsp + narrow + kWorld) ==
            std::string(kHello) + "  " + kWorld);
}

TEST_CASE("lsxhome: normalization leaves ordinary text byte-identical",
          "[lsxhome][chat]") {
    const std::string plain = "Plain ASCII text";
    REQUIRE(lsxhome::NormalizeEngineText(plain) == plain);

    const std::string cyrillic = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82";  // Привет
    REQUIRE(lsxhome::NormalizeEngineText(cyrillic) == cyrillic);

    // Tabs, newlines and punctuation are content, not spacing quirks.
    const std::string layout = "line1\nline2\ttabbed \"quoted\" **bold**";
    REQUIRE(lsxhome::NormalizeEngineText(layout) == layout);

    REQUIRE(lsxhome::NormalizeEngineText("").empty());
    // A 4-byte emoji survives: only spaces are touched.
    REQUIRE(lsxhome::NormalizeEngineText(kGlobe) == kGlobe);
}
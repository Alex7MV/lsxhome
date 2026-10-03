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
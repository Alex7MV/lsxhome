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
/// empty assistant reply), then any number of `AppendDelta(seq, text)` calls,
/// then the closing `EndTurn(interrupted)`. Chunks carry a per-turn monotonic
/// sequence number, so a replayed or reordered chunk is rejected instead of
/// corrupting the text.
class ChatState {
public:
    /// Capacity reserved for a reply's text when the turn opens, so a typical
    /// streamed answer appends without reallocating.
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
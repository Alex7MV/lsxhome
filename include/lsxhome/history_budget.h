#pragma once

#include <cstddef>
#include <vector>

#include "generation_backend.h"

namespace lsxhome {

/// Trims the framed history to `budget_bytes` of text, keeping the newest turns.
///
/// Why: the prompt grows with every exchange, and answer time grows with it. On
/// the real checkpoint a third question already doubled the wait and the fifth
/// took minutes, because each turn re-encodes the whole transcript (measured:
/// `input_ids` 41 → 417 → 468 → 1733 for the same conversation). A chat surface
/// needs a ceiling, not an ever-growing context.
///
/// Rules:
///  - the newest turn (the question being answered) is always kept, however long;
///  - older turns are dropped whole, from the oldest, until the budget fits;
///  - what remains starts at a user turn, so the family chat template never sees
///    an assistant turn whose question was cut off;
///  - the budget counts UTF-8 bytes, because that is what the tokenizer sees.
///
/// Returns a copy; the input is never modified (the worker keeps the full
/// history for the transcript's sake).
inline std::vector<ChatTurn> TrimHistoryToBudget(
    const std::vector<ChatTurn>& history, std::size_t budget_bytes) {
    if (history.empty()) {
        return {};
    }

    const auto bytes_of = [](const std::vector<ChatTurn>& turns) {
        std::size_t total = 0;
        for (const ChatTurn& turn : turns) {
            total += turn.text.size() + turn.role.size();
        }
        return total;
    };

    if (bytes_of(history) <= budget_bytes) {
        return history;
    }

    // Walk backwards until the next older turn would overflow the budget.
    std::size_t start = history.size();
    std::size_t used = 0;
    while (start > 0) {
        const ChatTurn& turn = history[start - 1];
        const std::size_t cost = turn.text.size() + turn.role.size();
        if (used + cost > budget_bytes) {
            break;
        }
        used += cost;
        --start;
    }

    // The newest question must always reach the model.
    if (start == history.size()) {
        start = history.size() - 1;
    }

    // Skip a leading assistant turn: the template expects user -> assistant.
    while (start + 1 < history.size() && history[start].role != "user") {
        ++start;
    }

    return std::vector<ChatTurn>(history.begin() + static_cast<std::ptrdiff_t>(start),
                                 history.end());
}

}  // namespace lsxhome
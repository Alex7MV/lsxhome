#pragma once

#include <cstddef>
#include <string_view>

namespace lsxhome {

/// Splits @p text into space-delimited words, skipping empty runs.
///
/// Writes at most @p cap `std::string_view`s into @p out — each one a slice of
/// @p text, so no allocation and no copying happens. Returns the number of
/// words actually written.
///
/// Runs of spaces are separators, not words: `"alpha  beta"` yields two words,
/// never an empty one between them. Whitespace-only or empty input yields
/// nothing, so the caller never publishes a blank token to the UI stream.
inline std::size_t SplitOnSpaces(std::string_view text,
                                 std::string_view* out,
                                 std::size_t cap) noexcept {
    if (out == nullptr || cap == 0) {
        return 0;
    }
    std::size_t count = 0;
    std::size_t pos = 0;
    while (pos < text.size() && count < cap) {
        while (pos < text.size() && text[pos] == ' ') {
            ++pos;
        }
        const std::size_t start = pos;
        while (pos < text.size() && text[pos] != ' ') {
            ++pos;
        }
        if (pos > start) {
            out[count++] = text.substr(start, pos - start);
        }
    }
    return count;
}

}  // namespace lsxhome

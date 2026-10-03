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

/// Splits @p text into chunks of at most @p max_bytes bytes, never cutting a
/// UTF-8 codepoint in half.
///
/// The `TokenPayload` text field is 32 bytes while a decoded delta is an
/// arbitrary span of model output, so a delta has to be chunked before it can
/// be published. Cutting on byte boundaries alone would emit half a multibyte
/// character into the UI, so the walk advances one codepoint at a time and
/// closes a chunk only when the next codepoint would not fit.
///
/// Writes at most @p cap views into @p out (slices of @p text, so no allocation
/// and no copying) and returns how many were written. A budget below the 4-byte
/// maximum codepoint is widened to 4: emitting a chunk too small to hold a whole
/// character would corrupt the text, and no input can be served by it.
///
/// A @p cap smaller than the text needs truncates the remainder — size @p cap
/// for `text.size() / max_bytes + 1`.
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
        // A truncated tail still has to terminate the walk.
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

}  // namespace lsxhome

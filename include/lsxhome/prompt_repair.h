#pragma once

#include <string>
#include <string_view>
#include <utility>

namespace lsxhome {

/// Drops the parts of @p text that `encodes` rejects.
///
/// A chat prompt is rendered from a transcript the model produced, and the
/// model can emit bytes the vocabulary refuses (byte-fallback pieces, control
/// characters, exotic spaces). One such byte makes the tokenizer reject the
/// whole prompt, and the user loses every answer from that point on. Instead of
/// failing the turn, ask the tokenizer what it accepts and keep only that.
///
/// `encodes(text)` must return true when the tokenizer can encode `text`.
/// Probing is by word first — one call per word — and byte-wise inside a word
/// only when that word is rejected, so the common case (everything encodes)
/// costs a single call and normal text is returned unchanged.
template <typename Probe>
std::string DropUnencodable(Probe&& encodes, std::string_view text) {
    if (encodes(text)) {
        return std::string(text);
    }

    constexpr std::string_view kSeparators = " \n\r\t";
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        std::size_t word_end = text.find_first_of(kSeparators, i);
        if (word_end == std::string_view::npos) {
            word_end = text.size();
        }
        const std::string_view word = text.substr(i, word_end - i);
        if (encodes(word)) {
            out.append(word);
        } else {
            // The word is refused as a whole. Walk it **by codepoint**, asking
            // the vocabulary about one character at a time and dropping the
            // characters it refuses. Walking per byte would be wrong: the bytes
            // of a multibyte character do not encode on their own, so it would
            // delete every Cyrillic character in the prompt.
            std::size_t k = 0;
            while (k < word.size()) {
                std::size_t len = 1;
                const unsigned char lead = static_cast<unsigned char>(word[k]);
                if ((lead & 0xE0u) == 0xC0u) {
                    len = 2;
                } else if ((lead & 0xF0u) == 0xE0u) {
                    len = 3;
                } else if ((lead & 0xF8u) == 0xF0u) {
                    len = 4;
                }
                if (k + len > word.size()) {
                    len = word.size() - k;  // truncated tail
                }
                const std::string_view character = word.substr(k, len);
                if (encodes(character)) {
                    out.append(character);
                }
                k += len;
            }
        }
        i = word_end;
        if (i < text.size()) {
            out += text[i];  // the separator itself
            ++i;
        }
    }
    return out;
}

}  // namespace lsxhome
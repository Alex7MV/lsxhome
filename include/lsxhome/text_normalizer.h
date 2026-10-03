#pragma once

#include <string>
#include <string_view>

namespace lsxhome {

/// Rewrites text that the engine's raw-byte decoder leaves as byte-level BPE
/// escapes, plus the Unicode spaces a converted vocabulary has no token for.
///
/// A byte-level BPE vocabulary stores every non-printable byte as a
/// Latin-1 supplement glyph: the space becomes U+0120 «Ġ», a newline U+010A
/// «Ċ», a tab U+0109 «ĉ» (GPT-2's bytes_to_unicode mapping, U+0100..U+0143).
/// `arrow_tokenizer_decode` returns those raw vocabulary bytes, so a generated
/// answer arrives at the shell containing **no ordinary space and no ordinary
/// newline** — only these glyphs. Two measured consequences on the real Gemma
/// checkpoint:
///
///  - framing the answer back into the next turn fails: the vocabulary sees one
///    500+ byte "word" (`kMaxWordBytes` is 512), so `arrow_tokenizer_encode`
///    returns an error and the whole prompt is rejected — the first question
///    worked, every follow-up question did not;
///  - the model itself reads the glyphs as mojibake and answers with
///    "the question appears to be in the wrong encoding".
///
/// U+00A0 and the other no-break spaces are handled too: they are real Unicode
/// spaces that this vocabulary has no token for.
///
/// Everything else — tabs, newlines, punctuation, letters, emoji — passes
/// through byte-identical.
inline std::string NormalizeEngineText(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        const unsigned char second =
            i + 1 < text.size() ? static_cast<unsigned char>(text[i + 1]) : 0u;
        const unsigned char third =
            i + 2 < text.size() ? static_cast<unsigned char>(text[i + 2]) : 0u;

        // GPT-2 bytes_to_unicode, reversed. U+0100..U+013F is UTF-8 C4 80..C4 BF and
        // U+0140..U+0143 is C5 80..C5 83 (all two-byte sequences).
        int decoded_byte = -1;
        std::size_t skip = 0;
        if (lead == 0xC4u && second >= 0x80u && second <= 0xBFu) {
            const unsigned cp = 0x100u + (second - 0x80u);  // 0x100..0x13F
            // cp 0x100..0x120 stands for bytes 0x00..0x20, cp 0x121..0x143 for
            // bytes 0x7F..0xA3.
            decoded_byte = cp <= 0x120u ? static_cast<int>(cp - 0x100u)
                                       : static_cast<int>(cp - 0x121u) + 0x7F;
            skip = 2;
        } else if (lead == 0xC5u && second >= 0x80u && second <= 0x83u) {
            decoded_byte = 0x9E + static_cast<int>(second - 0x80u);  // 0x9E..0xA1
            skip = 2;
        }
        if (skip != 0) {
            if (decoded_byte >= 0) {
                const char byte = static_cast<char>(decoded_byte);
                // NUL and the remaining C0 controls mean nothing in a
                // transcript; newline and tab are content and survive.
                if (byte == '\0' || (byte < ' ' && byte != '\n' && byte != '\t')) {
                    out += ' ';
                } else {
                    out += byte;
                }
                i += skip;
                continue;
            }
            // Not a byte-level marker after all: emit it unchanged.
            out.append(text.substr(i, skip));
            i += skip;
            continue;
        }

        // Unicode spaces the vocabulary has no token for: U+00A0, U+1680,
        // U+2000..U+200A, U+202F, U+205F, U+3000.
        const bool is_unicode_space =
            (lead == 0xC2u && second == 0xA0u) ||
            (lead == 0xE1u && second == 0x9Au && third == 0x80u) ||
            (lead == 0xE2u && second == 0x80u && third >= 0x80u &&
             third <= 0x8Au) ||
            (lead == 0xE2u && second == 0x80u && third == 0xAFu) ||
            (lead == 0xE2u && second == 0x81u && third == 0x9Fu) ||
            (lead == 0xE3u && second == 0x80u && third == 0x80u);
        if (is_unicode_space) {
            out += ' ';
            i += (lead == 0xC2u) ? 2 : 3;
            continue;
        }

        out += text[i];
        ++i;
    }
    return out;
}

}  // namespace lsxhome
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

        // GPT-2 bytes_to_unicode, reversed. U+0100..U+013F is UTF-8 C4 80..C4 BF
        // and U+0140..U+0143 is C5 80..C5 83 (all two-byte sequences).
        // The lifted bytes are: cp 0x100..0x120 -> 0x00..0x20,
        // cp 0x121..0x142 -> 0x7F..0xA0, and cp 0x143 -> 0xAD on its own.
        int decoded_byte = -1;
        std::size_t skip = 0;
        if (lead == 0xC4u && second >= 0x80u && second <= 0xBFu) {
            const unsigned cp = 0x100u + (second - 0x80u);  // 0x100..0x13F
            decoded_byte = cp <= 0x120u
                               ? static_cast<int>(cp - 0x100u)
                               : static_cast<int>(cp - 0x121u) + 0x7F;
            skip = 2;
        } else if (lead == 0xC5u && second >= 0x80u && second <= 0x83u) {
            // 0x141..0x142 continue the 0x7F..0xA0 run; 0x143 stands alone,
            // because bytes 0xA1..0xAC are printable Latin-1 and were never
            // lifted (0xAD, the soft hyphen's second UTF-8 byte, is).
            const unsigned cp = 0x140u + (second - 0x80u);
            decoded_byte = cp == 0x143u ? 0xAD
                                        : 0x9E + static_cast<int>(cp - 0x140u);
            skip = 2;
        }
        if (skip != 0) {
            if (decoded_byte >= 0) {
                const char byte = static_cast<char>(decoded_byte);
                const unsigned ubyte = static_cast<unsigned char>(byte);
                // NUL and the remaining C0 controls mean nothing in a
                // transcript; newline and tab are content and survive. The
                // comparison must be unsigned: a restored 0x80 as a signed char
                // is negative and would look like a control.
                if (ubyte == 0u || (ubyte < 0x20u && byte != '\n' && byte != '\t')) {
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

    // The decoder hands back raw vocabulary bytes, so a generated answer can
    // also contain a lone high byte (a byte-fallback piece) that starts no
    // UTF-8 sequence, or a sequence the last token cut in half. Framing that
    // back into the next prompt fails (the tokenizer rejects malformed UTF-8),
    // so drop whatever cannot form valid text.
    std::string valid;
    valid.reserve(out.size());
    std::size_t j = 0;
    while (j < out.size()) {
        const unsigned char lead = static_cast<unsigned char>(out[j]);
        std::size_t len = 1;
        if (lead < 0x80u) {
            len = 1;
        } else if ((lead & 0xE0u) == 0xC0u) {
            len = 2;
        } else if ((lead & 0xF0u) == 0xE0u) {
            len = 3;
        } else if ((lead & 0xF8u) == 0xF0u) {
            len = 4;
        } else {
            ++j;  // continuation byte with no lead: drop it
            continue;
        }
        if (j + len > out.size()) {
            break;  // truncated tail
        }
        bool ok = true;
        for (std::size_t k = 1; k < len; ++k) {
            if ((static_cast<unsigned char>(out[j + k]) & 0xC0u) != 0x80u) {
                ok = false;
                break;
            }
        }
        if (!ok) {
            ++j;
            continue;
        }
        valid.append(out, j, len);
        j += len;
    }
    return valid;
}

}  // namespace lsxhome
#pragma once

#include <string>
#include <string_view>

namespace lsxhome {

/// Rewrites characters a model vocabulary cannot encode back into plain ASCII
/// spaces.
///
/// Two distinct causes, both measured on the real Gemma checkpoint:
///
///  - **U+0120 «Ġ» (bytes C4 A0)** — the byte-level BPE spelling of a space.
///    The engine's decoder returns raw vocabulary bytes, so a generated answer
///    arrives with *no ordinary space anywhere*: the whole answer becomes one
///    500+ byte "word", the vocabulary refuses it (`kMaxWordBytes` is 512), and
///    because that answer is framed back into the next turn's prompt, every
///    follow-up question failed with "failed to frame the prompt for the model".
///    The first question worked; the second never did. Rewriting the marker
///    also fixes what the user sees — the "strange character" was this glyph.
///  - **U+00A0 and friends (NBSP, narrow NBSP, thin space, ideographic space)**
///    — real Unicode spaces the converted vocabulary has no token for.
///
/// Anything else — tabs, newlines, punctuation, letters, emoji — is content and
/// passes through byte-identical.
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
        // The byte-level BPE space marker.
        const bool is_byte_level_space = (lead == 0xC4u && second == 0xA0u);
        // Zs class and the two no-break specials, as their UTF-8 encodings.
        const bool is_space_like =
            (lead == 0xC2u && second == 0xA0u) ||                    // U+00A0
            (lead == 0xE2u && second == 0x80u && third >= 0x80u &&
             third <= 0x8Au) ||                                     // U+2000..200A
            (lead == 0xE2u && second == 0x80u && third == 0xAFu) ||  // U+202F
            (lead == 0xE2u && second == 0x81u && third == 0x9Fu) ||  // U+205F
            (lead == 0xE1u && second == 0x9Au && third == 0x80u) ||   // U+1680
            (lead == 0xE3u && second == 0x80u && third == 0x80u);    // U+3000
        if (is_byte_level_space || is_space_like) {
            out += ' ';
            // Codepoint length: U+00A0 and the byte-level marker are two bytes,
            // every Unicode Zs space is three. Getting this wrong would eat the
            // first byte of the character that follows.
            i += (lead == 0xC2u || is_byte_level_space) ? 2u : 3u;
            continue;
        }
        out += text[i];
        ++i;
    }
    return out;
}

}  // namespace lsxhome
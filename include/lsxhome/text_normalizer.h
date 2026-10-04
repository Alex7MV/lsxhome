#pragma once

#include <string>
#include <string_view>

namespace lsxhome {

/// Removes the invisible junk the engine leaves in generated text, and nothing
/// else.
///
/// Measured on the converted Gemma-4 checkpoint (gemma-4-26B-A4B-it) after the
/// engine fix in logestix 456280d ("decode to text the tokenizer can encode
/// again"):
///
///  * **The decoder already repairs invalid bytes.** Both decode branches
///    substitute U+FFFD for a byte that cannot begin or complete a well-formed
///    sequence, with a streaming state machine so byte-fallback pieces still
///    assemble a codepoint split across tokens. Verified with the audit probe
///    (`docs/repro/engine-encoding-audit`): decoding a lone 0x97, 0xC4, 0xE9 or
///    0x80 yields U+FFFD, while `decode(0xD0 0x9F)` still yields «П». So invalid
///    bytes never reach the shell and must not be handled here again.
///  * **The vocabulary lifts nothing into U+0100..U+0143** — the space is an
///    ordinary 0x20 token and there is not a single «C4 xx» entry. The GPT-2
///    "Ġ is the space" theory does not apply to this checkpoint, and rewriting
///    U+0100..U+0143 would corrupt real characters.
///  * **What remains is C1 junk.** The model emits byte-fallback pieces
///    (`<0xC2><0x97>` = U+0097) repeatedly inside its answers. They are valid
///    UTF-8, so nothing filters them, and they are why the model itself reports
///    that the conversation "looks like the wrong encoding".
///
/// Everything else — ASCII, Cyrillic, emoji, Latin-1 typography, smart quotes,
/// tabs, newlines and the engine's own U+FFFD — passes through byte-identical.
/// Deliberately absent, each tried and measured against: re-encoding Latin-1 to
/// bytes (ambiguous: U+00E9 as text and byte 0xE9 are the same bytes), a greedy
/// byte-repair pass (it glued unrelated bytes into new characters), and
/// converting by the Windows ANSI code page (text is already UTF-8).
inline std::string NormalizeEngineText(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);

        // U+0080..U+009F arrives as C2 80..C2 9F: drop it.
        if (lead == 0xC2u && i + 1 < text.size()) {
            const unsigned char second = static_cast<unsigned char>(text[i + 1]);
            if (second >= 0x80u && second <= 0x9Fu) {
                i += 2;
                continue;
            }
            out += text[i];
            out += text[i + 1];
            i += 2;
            continue;
        }

        // DEL is invisible too; a space keeps the words apart.
        if (lead == 0x7Fu) {
            out += ' ';
            ++i;
            continue;
        }

        // Any other byte, including a lead byte of a multibyte character, is
        // copied verbatim and its length preserved by the scan below.
        out += text[i];
        ++i;
    }
    return out;
}

}  // namespace lsxhome
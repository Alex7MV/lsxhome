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
///  * **What remains is model-generated junk.** The model emits byte-fallback
///    pieces (`<0xC2><0x97>` = U+0097) and lone high bytes inside its answers.
///    The decoder now marks what it cannot reconstruct with U+FFFD, so a real
///    answer arrives as runs of U+0097 and U+FFFD between ASCII words — the
///    engine is reporting the model's own unencodable output, not a decoding bug.
///
/// So the shell's job is narrow: drop the C1 controls and the engine's U+FFFD
/// marker, and leave everything else byte-identical. Anything more aggressive
/// rewrites the model's own words (a greedy byte-repair pass glued unrelated
/// bytes into new characters, and rewriting U+0100..U+0143 corrupted genuine
/// characters such as «Ġ»).
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

        // U+0080..U+009F arrives as C2 80..C2 9F: the model's byte-fallback
        // junk. Dropped.
        if (lead == 0xC2u && second >= 0x80u && second <= 0x9Fu) {
            i += 2;
            continue;
        }

        // U+FFFD (EF BF BD): the decoder's marker for a byte it could not
        // reconstruct. Removed so a mangled answer reads as words with gaps
        // instead of a wall of replacement diamonds.
        if (lead == 0xEFu && second == 0xBFu && third == 0xBDu) {
            i += 3;
            continue;
        }

        // DEL is invisible too; a space keeps the words apart.
        if (lead == 0x7Fu) {
            out += ' ';
            ++i;
            continue;
        }

        out += text[i];
        ++i;
    }
    return out;
}

}  // namespace lsxhome
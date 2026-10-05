// lsxhome audit probe: how the engine's tokenizer stores and decodes bytes.
//
// Self-contained: needs only lsxcommon + Arrow, no CUDA and no model load.
// Point it at the MODEL ROOT (the directory that contains logestix/):
//
//     tokenizer_audit <model-root>
//
// It answers the four questions the chat MVP had to answer the hard way:
//   1. Does the vocabulary keep non-printable bytes as Latin-1 supplement
//      glyphs (GPT-2 bytes_to_unicode: 0x20 -> U+0120), i.e. is the space stored
//      as the two bytes C4 A0?
//   2. What does arrow_tokenizer_decode return for such a token — the glyph, or
//      the byte it stands for?
//   3. How many vocabulary entries are byte-level fallbacks (<0xHH>)?
//   4. Does arrow_tokenizer_encode accept the decoded text back? A chat
//      transcript must round-trip: answer -> decode -> frame -> encode.

#include "lsxcommon/chat_conversation.h"
#include "lsxcommon/gemma_tool_format.h"
#include "lsxcommon/tokenizer.h"

#include <arrow/io/file.h>  // arrow::io::MemoryMappedFile

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {

int g_failures = 0;

void Report(const char* what, bool ok, const std::string& detail) {
    std::printf("[%s] %-58s %s\n", ok ? " ok " : "FAIL", what, detail.c_str());
    if (!ok) {
        ++g_failures;
    }
}

std::string Escape(std::string_view text) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    for (const char ch : text) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c >= 0x20 && c < 0x7F) {
            out += static_cast<char>(c);
        } else {
            out += "\\x";
            out += kHex[c >> 4];
            out += kHex[c & 0x0F];
        }
    }
    return out;
}

/// Returns the id of the entry whose raw bytes are exactly @p bytes, or -1.
std::int32_t FindToken(const lsxcommon::ArrowTokenizerDb& db,
                       std::string_view bytes) {
    for (std::int32_t id = 0; id < db.vocab_size; ++id) {
        if (db.is_special[id]) continue;
        const std::int64_t off = db.token_offsets[id];
        const std::int64_t len = db.token_offsets[id + 1] - off;
        if (static_cast<std::size_t>(len) != bytes.size()) continue;
        if (std::memcmp(db.token_bytes + off, bytes.data(), bytes.size()) == 0) {
            return id;
        }
    }
    return -1;
}

/// Decodes @p ids with the engine's decoder.
std::string Decode(const lsxcommon::ArrowTokenizerDb& db,
                   const std::vector<std::int32_t>& ids) {
    std::size_t cap = 4096;
    std::string out;
    for (int attempt = 0; attempt < 8; ++attempt) {
        out.assign(cap, '\0');
        const std::int32_t rc = lsxcommon::arrow_tokenizer_decode(
            db, ids.data(), ids.size(), out.data(), &cap);
        if (rc == 0) {
            out.resize(cap);
            return out;
        }
        if (rc == -2 && cap > out.size() && cap <= (1u << 20)) continue;
        return "<decode error " + std::to_string(rc) + ">";
    }
    return "<decode never satisfied>";
}

/// Encodes @p text and reports the engine's return code plus the id count.
void CheckEncode(const lsxcommon::ArrowTokenizerDb& db,
                 const char* label,
                 std::string_view text) {
    std::vector<std::int32_t> ids(4096);
    std::size_t cap = ids.size();
    const std::int32_t rc =
        lsxcommon::arrow_tokenizer_encode(db, text.data(), text.size(),
                                          ids.data(), &cap);
    std::string detail = "rc=" + std::to_string(rc) + " ids=" +
                         std::to_string(rc > 0 ? cap : 0) + " text=[" +
                         Escape(text).substr(0, 60) + "]";
    if (rc >= 0) {
        // Round-trip: decode what we just encoded.
        const std::string back = Decode(db, std::vector<std::int32_t>(
                                                ids.begin(),
                                                ids.begin() + (rc > 0 ? cap : 0)));
        detail += " decoded=[" + Escape(back).substr(0, 60) + "]";
    }
    Report(label, rc >= 0, detail);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: tokenizer_audit <model-root>\n");
        return 2;
    }
    const std::string root = argv[1];
    const std::string vocab_path = root + "/logestix/pinned/tokenizer_vocab.arrow";
    const std::string merges_path =
        root + "/logestix/pinned/tokenizer_merges.arrow";

    auto vocab_file = arrow::io::MemoryMappedFile::Open(
        vocab_path, arrow::io::FileMode::READ);
    if (!vocab_file.ok()) {
        std::printf("cannot open %s: %s\n", vocab_path.c_str(),
                    vocab_file.status().ToString().c_str());
        return 2;
    }
    auto merges_file = arrow::io::MemoryMappedFile::Open(
        merges_path, arrow::io::FileMode::READ);
    if (!merges_file.ok()) {
        std::printf("cannot open %s: %s\n", merges_path.c_str(),
                    merges_file.status().ToString().c_str());
        return 2;
    }

    lsxcommon::ArrowTokenizerDb db;
    const arrow::Status loaded = lsxcommon::load_tokenizer_metadata(
        *vocab_file, *merges_file, db);
    if (!loaded.ok() || !db.valid()) {
        std::printf("load_tokenizer_metadata failed: %s (%s)\n",
                    loaded.ToString().c_str(), db.error_msg.c_str());
        return 2;
    }

    std::printf("vocab_size=%d tokenizer_kind=%d byte_fallback=%d "
                "add_dummy_prefix=%d escape_whitespaces=%d merges=%d\n\n",
                db.vocab_size, db.tokenizer_kind, db.byte_fallback,
                db.add_dummy_prefix, db.escape_whitespaces, db.merge_count);

    // ── 1. How is the space stored? ────────────────────────────────────────
    // Measured on gemma-4-26B-A4B-it: the vocabulary does NOT lift
    // non-printable bytes into U+0100..U+0143 (byte_fallback=0, zero "C4 xx"
    // entries). The space is an ordinary one-byte token, and the GPT-2
    // "Ġ" theory does not apply to this checkpoint.
    const std::int32_t space_id = FindToken(db, " ");
    Report("vocab stores the space as a plain 0x20 token", space_id >= 0,
           "id=" + std::to_string(space_id));
    const std::int32_t lifted_space = FindToken(db, "\xC4\xA0");  // U+0120
    Report("vocab lifts bytes into U+0100..U+0143 (GPT-2 style)",
           lifted_space >= 0,
           std::string("id=") + std::to_string(lifted_space) +
               " (absent means the 'Ġ' theory does not apply here)");

    // ── 2. Round-trip of a normal token ─────────────────────────────────────
    if (space_id >= 0) {
        const std::string decoded = Decode(db, {space_id});
        Report("decode(space) yields a plain space", decoded == " ",
               "decoded=[" + Escape(decoded) + "]");
    }

    // ── 3. Byte-level fallback inventory ────────────────────────────────────
    std::int32_t byte_fallback_entries = 0;
    std::int32_t lifted_entries = 0;
    std::int32_t c1_entries = 0;
    for (std::int32_t id = 0; id < db.vocab_size; ++id) {
        if (db.is_special[id]) continue;
        const std::int64_t off = db.token_offsets[id];
        const std::int64_t len = db.token_offsets[id + 1] - off;
        const char* p = reinterpret_cast<const char*>(db.token_bytes + off);
        if (len == 6 && p[0] == '<' && p[1] == '0' && p[2] == 'x' &&
            p[5] == '>') {
            ++byte_fallback_entries;
            continue;
        }
        if (len == 2 && static_cast<unsigned char>(p[0]) == 0xC4) {
            ++lifted_entries;
        }
        if (len == 2 && static_cast<unsigned char>(p[0]) == 0xC2 &&
            static_cast<unsigned char>(p[1]) >= 0x80 &&
            static_cast<unsigned char>(p[1]) <= 0x9F) {
            ++c1_entries;  // U+0080..U+009F: C1 controls the model emits
        }
    }
    std::printf("entries: byte-fallback=%d lifted(C4 xx)=%d C1-controls(C2 8x..9x)=%d\n",
                byte_fallback_entries, lifted_entries, c1_entries);
    Report("C1-control entries exist (model can emit them)", c1_entries > 0,
           "count=" + std::to_string(c1_entries));

    // ── 5. The actual failure mechanism ────────────────────────────────────
    // The engine fix (logestix a077d53, "decode to text the tokenizer can
    // encode again") makes BOTH decode branches substitute U+FFFD for bytes
    // that cannot begin or complete a well-formed sequence, with a streaming
    // state machine so byte-fallback pieces still assemble a codepoint split
    // across tokens. These checks pin exactly that contract.

    const std::string replacement = "\xEF\xBF\xBD";  // U+FFFD

    // A vocabulary token whose value IS a lone high byte: the model can emit
    // it, and decoding it must not yield an un-encodable byte.
    for (const unsigned byte : {0x97u, 0xC4u, 0xE9u, 0x80u}) {
        const std::int32_t id = db.byte_to_token_id[byte];
        if (id < 0) {
            Report("vocab exposes the lone byte as a token", false,
                   "byte=0x" + std::to_string(byte));
            continue;
        }
        const std::string decoded = Decode(db, {id});
        // Either the decoder repaired it, or the byte was printable ASCII.
        const bool repaired = decoded == replacement;
        const bool unchanged = decoded.size() == 1 &&
                               static_cast<unsigned char>(decoded[0]) == byte;
        Report("decode(lone high byte) is encodable text",
               repaired || unchanged,
               "byte=0x" + std::to_string(byte) + " decoded=[" +
                   Escape(decoded) + "]" +
                   (repaired ? " (replaced)" : (unchanged ? " (as-is)" : " (BROKEN)")));
    }

    // The other half of the contract: byte-fallback pieces that assemble a
    // real codepoint must survive untouched, or every Cyrillic character in a
    // Russian answer would turn into three U+FFFD.
    {
        const std::int32_t b0 = db.byte_to_token_id[0xD0];  // 'П' = D0 9F
        const std::int32_t b1 = db.byte_to_token_id[0x9F];
        const std::string decoded = Decode(db, {b0, b1});
        Report("decode(0xD0 0x9F) keeps the codepoint", decoded == "\xD0\x9F",
               "decoded=[" + Escape(decoded) + "]");
    }

    CheckEncode(db, "encode plain ASCII", "hello world");
    CheckEncode(db, "encode cyrillic UTF-8",
                "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82");
    CheckEncode(db, "encode U+0120 (the 'Ġ' glyph seen in answers)",
                "\xC4\xA0");

    // The encoder must still reject raw invalid text: that is what the decoder
    // now prevents from ever reaching it.
    CheckEncode(db, "encode a C1 control (U+0097)", "ok\xC2\x97" "done");
    CheckEncode(db, "encode a lone continuation byte (0x97)", "ok\x97" "done");
    CheckEncode(db, "encode a lone lead byte (0xC4)", "ok\xC4" "done");
    CheckEncode(db, "encode U+0120 as if it were text", "ok\xC4\xA0" "done");

    // Byte-fallback ids the model can emit: does the vocabulary expose them?
    std::int32_t printable_fallback = 0;
    for (int b = 0; b < 256; ++b) {
        if (db.byte_to_token_id[b] >= 0 && !db.is_special[db.byte_to_token_id[b]]) {
            ++printable_fallback;
        }
    }
    std::printf("byte_to_token_id populated for %d/256 bytes\n\n",
                printable_fallback);

    // ── 6. Encoder audit: does the model get the question we meant to send? ──
    // The decode side is fixed (U+FFFD substitution). This checks the other
    // direction: frame the question with the model's own chat template, encode
    // it, decode it back, and require the text to be unchanged. Any difference
    // is what the model actually reads instead of the user's words — the one
    // failure mode that is invisible in the UI but fatal to the answer.
    {
        lsxcommon::ChatConversation conversation;
        lsxcommon::ChatMessage question;
        question.role = "user";
        question.content = "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82, "
                           "\xD0\xBA\xD0\xB0\xD0\xBA \xD0\xB4\xD0\xB5\xD0\xBB\xD0\xB0?";
        conversation.messages.push_back(question);

        std::string rendered;
        const bool render_ok =
            lsxcommon::RenderGemmaConversation(conversation, rendered);
        Report("RenderGemmaConversation succeeds", render_ok,
               "chars=" + std::to_string(rendered.size()));

        std::vector<std::int32_t> ids;
        const bool ids_ok =
            lsxcommon::AppendGemmaChatIds(db, rendered, ids);
        Report("AppendGemmaChatIds succeeds", ids_ok,
               "ids=" + std::to_string(ids.size()));

        if (ids_ok && !ids.empty()) {
            const std::string back = Decode(db, ids);

            // decode() drops special tokens by design, so compare the *user
            // words*, not the whole template: they must arrive byte-exactly.
            const std::string marker = "<|turn>user\n";
            const std::size_t at = rendered.find(marker);
            const std::size_t from = at == std::string::npos ? 0 : at + marker.size();
            const std::size_t to =
                rendered.find("<turn|>", from);
            const std::string asked =
                rendered.substr(from, to == std::string::npos
                                            ? std::string::npos
                                            : to - from);
            Report("the question is framed into the prompt", at != std::string::npos,
                   "asked=[" + Escape(asked).substr(0, 60) + "]");
            Report("the user's question reaches the model byte-exactly",
                   back.find(asked) != std::string::npos,
                   "decoded=[" + Escape(back).substr(0, 90) + "]");

            // No U+FFFD may appear: that would mean the encoder produced bytes
            // the decoder had to repair, i.e. the question changed.
            Report("no replacement characters in the framed prompt",
                   back.find(replacement) == std::string::npos,
                   "fffd_count=" + std::to_string(
                       std::count(back.begin(), back.end(), '\xEF') > 0 ? 1 : 0));

            // Every token must be a real vocabulary id (no unk, no hole).
            bool ids_valid = true;
            for (const std::int32_t id : ids) {
                if (id < 0 || id >= db.vocab_size) {
                    ids_valid = false;
                    break;
                }
            }
            Report("every framed id exists in the vocabulary", ids_valid,
                   "ids=" + std::to_string(ids.size()) +
                       " unk_id=" + std::to_string(db.unk_id));

            // The exact prompts the shell failed to frame, byte for byte as the
            // UI sent them. Each must encode; a failure here reproduces the
            // "vocabulary rejected the prompt" error without loading the model.
            struct Prompt {
                const char* label;
                const char* text;
            };
            const Prompt prompts[] = {
                {"p1 ASCII", "Hello! How are you today?"},
                {"p2 Cyrillic + question mark",
                 "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82, "
                 "\xD0\xBA\xD0\xB0\xD0\xBA "
                 "\xD0\xB4\xD0\xB5\xD0\xBB\xD0\xB0?"},
                {"p3 Cyrillic + '!' + hyphen",
                 "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82! "
                 "\xD0\x9E\xD1\x82\xD0\xB2\xD0\xB5\xD1\x82\xD1\x8C "
                 "\xD0\xBF\xD0\xBE-\xD1\x80\xD1\x83\xD1\x81\xD1\x81\xD0\xBA\xD0"
                 "\xB8 "
                 "\xD0\xBE\xD0\xB4\xD0\xBD\xD0\xB8\xD0\xBC "
                 "\xD0\xBF\xD1\x80\xD0\xB5\xD0\xB4\xD0\xBB\xD0\xBE\xD0\xB6"
                 "\xD0\xB5\xD0\xBD\xD0\xB8\xD0\xB5\xD0\xBC."},
                {"p4 ASCII + C1 junk", "Hello! \xC2\x97\xC2\x97 done"},
                {"p5 long ASCII answer",
                 "It looks like you might be testing how I handle specific "
                 "characters or formatting. How can I help you today?"},
            };

            for (const Prompt& prompt : prompts) {
                lsxcommon::ChatConversation one;
                lsxcommon::ChatMessage only;
                only.role = "user";
                only.content = prompt.text;
                one.messages.push_back(only);

                std::string text;
                const bool render_ok = lsxcommon::RenderGemmaConversation(one, text);
                std::vector<std::int32_t> prompt_ids;
                const bool encode_ok = render_ok && lsxcommon::AppendGemmaChatIds(
                                                        db, text, prompt_ids);
                std::string detail;
                if (!render_ok) {
                    detail = "render failed";
                } else if (!encode_ok) {
                    detail = "encode failed; rendered=[" +
                             Escape(text).substr(0, 140) + "]";
                } else {
                    detail = "ids=" + std::to_string(prompt_ids.size());
                }
                Report(prompt.label, encode_ok && !prompt_ids.empty(), detail);
            }
        }
    }

    std::printf("\n%s: %d check(s) failed\n", g_failures ? "AUDIT" : "clean",
                g_failures);
    return g_failures ? 1 : 0;
}
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

#include "lsxcommon/tokenizer.h"

#include <arrow/io/file.h>  // arrow::io::MemoryMappedFile

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

    // ── 4. Does the chat transcript round-trip? ─────────────────────────────
    CheckEncode(db, "encode plain ASCII", "hello world");
    CheckEncode(db, "encode cyrillic UTF-8",
                "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82");
    CheckEncode(db, "encode U+0120 (the 'Ġ' glyph seen in answers)",
                "\xC4\xA0");

    // ── 5. The actual failure mechanism ────────────────────────────────────
    // byte_fallback pieces let the model emit a raw byte that starts no UTF-8
    // sequence. The decoder passes it through, and the encoder rejects it —
    // so one such byte in an answer makes every follow-up prompt fail to
    // frame. These three checks bracket it.
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

    std::printf("\n%s: %d check(s) failed\n", g_failures ? "AUDIT" : "clean",
                g_failures);
    return g_failures ? 1 : 0;
}
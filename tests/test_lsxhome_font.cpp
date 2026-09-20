#include <catch2/catch_test_macros.hpp>

#include "lsxhome_mono_payload.h"

#include <cstddef>
#include <cstdint>

// The JetBrains Mono payload is baked into the executable's static memory by
// the lsx_embed_binary pass (cmake/embed_binary.cmake). First TDD step: the
// test drives the embed target into existence — it must fail (missing fsymbol)
// before CMakeLists.txt wires the mono embed target.

TEST_CASE("lsxhome: embedded JetBrains Mono payload announces a non-zero size",
          "[lsxhome][font][mono]") {
    STATIC_REQUIRE(lsxhome_mono_payload_size > 0);
    REQUIRE(lsxhome_mono_payload_size > 0);
}

TEST_CASE("lsxhome: embedded JetBrains Mono payload is a TrueType container",
          "[lsxhome][font][mono]") {
    // sfntVersion field of a collection-safe TrueType: 0x00010000 (bytes
    // 0x00 0x01 0x00 0x00). A corrupted extraction would break this signature.
    REQUIRE(lsxhome_mono_payload_size >= 4);
    REQUIRE(lsxhome_mono_payload[0] == 0x00);
    REQUIRE(lsxhome_mono_payload[1] == 0x01);
    REQUIRE(lsxhome_mono_payload[2] == 0x00);
    REQUIRE(lsxhome_mono_payload[3] == 0x00);
}

TEST_CASE("lsxhome: embedded JetBrains Mono is larger than the empty marker",
          "[lsxhome][font][mono]") {
    REQUIRE(lsxhome_mono_payload_size > 1024);
}
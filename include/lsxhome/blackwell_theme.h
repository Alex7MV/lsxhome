#pragma once

#include <cstdint>

namespace lsxhome {
namespace theme {

/// RGBA packed color in 0xRRGGBBAA form.
using Color = std::uint32_t;

inline constexpr Color MakeRgba(std::uint8_t r, std::uint8_t g,
                                std::uint8_t b, std::uint8_t a = 0xFF) noexcept {
    return (static_cast<std::uint32_t>(r) << 24) |
           (static_cast<std::uint32_t>(g) << 16) |
           (static_cast<std::uint32_t>(b) << 8) | a;
}

/// Dark Blackwell-style palette. Precision hex values derived from the
/// Cowork desktop geometry; tuned for high-contrast text on near-black panels.
struct BlackwellPalette {
    Color window_bg        = MakeRgba(0x0B, 0x0B, 0x10);  // #0B0B10
    Color window_bg_alt    = MakeRgba(0x10, 0x10, 0x18);  // #101018
    Color field_bg         = MakeRgba(0x15, 0x15, 0x20);  // #151520
    Color field_hover      = MakeRgba(0x1C, 0x1C, 0x2C);  // #1C1C2C
    Color border           = MakeRgba(0x26, 0x26, 0x38);  // #262638
    Color sidebar          = MakeRgba(0x0D, 0x0D, 0x14);  // #0D0D14
    Color accent           = MakeRgba(0x4F, 0x8C, 0xFF);  // #4F8CFF
    Color accent_hover     = MakeRgba(0x6F, 0xA4, 0xFF);  // #6FA4FF
    Color text             = MakeRgba(0xE8, 0xE8, 0xF2);  // #E8E8F2
    Color text_secondary   = MakeRgba(0x9A, 0x9A, 0xAC);  // #9A9AAC
    Color text_disabled    = MakeRgba(0x62, 0x62, 0x74);  // #626274
    Color token_green      = MakeRgba(0x39, 0xD3, 0x53);  // #39D353 (Paris OK)
    Color token_red        = MakeRgba(0xE8, 0x50, 0x50);  // #E85050 (drop)
    Color scrollbar_bg     = MakeRgba(0x12, 0x12, 0x1C);  // #12121C
    Color scrollbar_grab   = MakeRgba(0x33, 0x33, 0x48);  // #333348
};

inline constexpr BlackwellPalette kPalette;

/// Cowork-style geometry: paddings, rounding, fixed panel sizes, spacing.
struct Geometry {
    float sidebar_width   = 300.f;
    float sidebar_min_w   = 240.f;
    float header_height   = 56.f;
    float footer_height   = 132.f;
    float padding         = 16.f;
    float item_spacing    = 6.f;
    float frame_padding   = 8.f;
    float rounding        = 8.f;
    float indent          = 24.f;
    float font_size       = 15.f;
    float scrollbar_size  = 8.f;
    float sidebar_font_size = 13.f;
};

inline constexpr Geometry kGeometry;

}  // namespace theme
}  // namespace lsxhome
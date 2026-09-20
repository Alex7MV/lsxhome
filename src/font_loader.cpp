// Font loader — ingests the embedded TrueType payload from static memory and
// feeds it to the imgui_freetype LCD subpixel rasterizer.

#include "lsxhome/font_loader.h"

#include "lsxhome/blackwell_theme.h"
#include "lsxhome_font_payload.h"
#include "lsxhome_mono_payload.h"
#include <imgui.h>
#include <imgui_freetype.h>

#include <cstdint>

namespace lsxhome {

namespace {

/// Adds an embedded TrueType payload via the FreeType loader with LCD subpixel
/// AA and returns the producing ImFont. Shared by both family loads.
ImFont* AddEmbedded(ImFontAtlas& atlas,
                    unsigned int loader_flags,
                    const unsigned char* data,
                    std::size_t size,
                    float size_px) noexcept {
    if (size == 0 || data == nullptr) {
        return atlas.AddFontDefault();
    }

    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 1;
    cfg.GlyphExtraAdvanceX = 0.0f;
    cfg.FontLoaderFlags = loader_flags;
    cfg.FontDataOwnedByAtlas = false;

    ImFont* font = atlas.AddFontFromMemoryTTF(
        const_cast<unsigned char*>(data), static_cast<int>(size), size_px, &cfg);
    return font;
}

}  // namespace

ImFont* FontLoader::LoadDefault(ImFontAtlas& atlas) noexcept {
    constexpr unsigned int kFlags = ImGuiFreeTypeLoaderFlags_LightHinting |
                                    ImGuiFreeTypeLoaderFlags_ForceAutoHint;
    return AddEmbedded(atlas, kFlags, static_cast<const unsigned char*>(lsxhome_font_payload),
                       lsxhome_font_payload_size, lsxhome::theme::kGeometry.font_size);
}

ImFont* FontLoader::LoadMonospace(ImFontAtlas& atlas) noexcept {
    constexpr unsigned int kFlags = ImGuiFreeTypeLoaderFlags_LightHinting |
                                    ImGuiFreeTypeLoaderFlags_ForceAutoHint;
    return AddEmbedded(atlas, kFlags,
                       static_cast<const unsigned char*>(lsxhome_mono_payload),
                       lsxhome_mono_payload_size, lsxhome::theme::kGeometry.font_size - 1.0f);
}

ImFont* FontLoader::LoadHeading(ImFontAtlas& atlas) noexcept {
    constexpr unsigned int kFlags = ImGuiFreeTypeLoaderFlags_LightHinting |
                                    ImGuiFreeTypeLoaderFlags_ForceAutoHint;
    return AddEmbedded(atlas, kFlags,
                       static_cast<const unsigned char*>(lsxhome_font_payload),
                       lsxhome_font_payload_size, 34.0f);
}

}  // namespace lsxhome
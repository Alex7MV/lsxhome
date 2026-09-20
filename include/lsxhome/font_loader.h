#pragma once

struct ImFont;
struct ImFontAtlas;

namespace lsxhome {

/// Font loader for the lsxhome shell: pulls the TrueType vector out of the
/// pre-allocated static execution memory (the CMake-embedded `lsxhome_font`
/// archive) and hands it to imgui_freetype for LCD subpixel rasterization.
///
/// The runtime calls this exactly once, right after ImGui context creation and
/// before the first font atlas build. All state is deliberately freed after
/// the atlas owns its copies (ImGui ref-copies the font binary), so the
/// embedded payload never lingers in the heap.
struct FontLoader {
    /// Loads the embedded Inter-Regular.ttf into @p atlas with FreeType LCD
    /// subpixel anti-aliasing enabled (RasterizerFlags LightHinting).
    /// Returns the built base font (already selected as the default).
    static ImFont* LoadDefault(ImFontAtlas& atlas) noexcept;

    /// Loads the embedded JetBrains Mono into the same atlas (monospace font
    /// for telemetry/chart labels). Returns the built mono font.
    static ImFont* LoadMonospace(ImFontAtlas& atlas) noexcept;

    /// Loads the embedded Inter at a larger point size for the welcome screen's
    /// hero greeting. Same FreeType LCD rasterizer settings as the base font.
    static ImFont* LoadHeading(ImFontAtlas& atlas) noexcept;
};

}  // namespace lsxhome
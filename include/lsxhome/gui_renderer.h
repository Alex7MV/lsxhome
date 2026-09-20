#pragma once

#include "lsxhome/gui_bridge.h"

struct ImFont;

namespace lsxhome {

/// Applies the high-fidelity matte Blackwell/Cowork theme to the active
/// Dear ImGui style. Hardlocked palette and geometry (see gui_renderer.cpp).
void ApplyBlackwellCoworkTheme() noexcept;

/// Registers the large hero font (FontLoader::LoadHeading) used by the
/// welcome interface. Call once after ImGui context creation.
void SetHeadingFont(ImFont* font) noexcept;

/// Renders the Claude-style welcome screen (greeting, input card, quick
/// actions) filling the current content region. Call inside the ##main child.
void DrawClaudeWelcomeInterface() noexcept;

/// Renders the full-viewport edge-to-edge dock root, then the seamless
/// sidebar / main-welcome panel blocks hosted inside it.
void BuildWorkspaceSkeleton(GuiBridge& bridge) noexcept;

}  // namespace lsxhome

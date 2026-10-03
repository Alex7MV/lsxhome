#pragma once

#include "lsxhome/chat_session.h"
#include "lsxhome/chat_state.h"
#include "lsxhome/gui_bridge.h"

struct ImFont;

namespace lsxhome {

/// Applies the high-fidelity matte Blackwell/Cowork theme to the active
/// Dear ImGui style. Hardlocked palette and geometry (see gui_renderer.cpp).
void ApplyBlackwellCoworkTheme() noexcept;

/// Registers the large hero font (FontLoader::LoadHeading) used by the
/// welcome screen. Call once after ImGui context creation.
void SetHeadingFont(ImFont* font) noexcept;

/// Fills the question draft with @p text (the quick-action cards call it), so a
/// suggested prompt can be edited before it is submitted.
void PrefillQuestion(const char* text) noexcept;

/// Renders the chat panel inside the current content region: the welcome screen
/// while the transcript is empty, then the conversation with the input form
/// pinned to the bottom. Drains @p session into @p state and closes a finished
/// reply. Call inside the ##main child.
void DrawChatPanel(ChatSession& session, ChatState& state) noexcept;

/// Renders the full-viewport edge-to-edge dock root, then the seamless
/// sidebar / main-chat panel blocks hosted inside it.
void BuildWorkspaceSkeleton(ChatSession& session, ChatState& state) noexcept;

}  // namespace lsxhome
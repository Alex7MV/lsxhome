// lsxhome — Blackwell matte/Cowork theme styling profile (Phase 1) and the
// Claude-style welcome interface.
//
// Owns the hardlocked ImGuiStyle, the edge-to-edge docking-grid skeleton
// render pass, and the centered welcome screen that fills the main panel.
// Zero-allocation: the theme touches style state only, and the workspace
// skeleton reuses the existing viewport — no per-frame allocations.

#include "lsxhome/gui_renderer.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace lsxhome {
namespace {

constexpr ImVec4 MakeVec4(float r, float g, float b, float a = 1.0f) noexcept {
    return ImVec4(r, g, b, a);
}

constexpr float kSidebarWidth = 300.0f;

// ── Welcome-interface geometry (per design: rounding 6f, centered column) ──
constexpr float kWelcomeColumn  = 700.0f;
constexpr float kWelcomeRound   = 6.0f;
constexpr float kHeaderH        = 56.0f;
constexpr float kCardH          = 220.0f;
constexpr float kActionsH       = 88.0f;
constexpr float kVGap           = 20.0f;

// ── Dedicated accents (coral asterisk, purple chip) — kept local ──
constexpr ImU32 kCoralU32        = IM_COL32(255, 106, 74, 255);
constexpr ImU32 kPurpleChipU32   = IM_COL32(108,  84, 200, 235);
constexpr ImU32 kChipTextU32     = IM_COL32(233, 231, 250, 255);
constexpr ImU32 kMutedU32        = IM_COL32(148, 148, 158, 255);
constexpr ImU32 kTextU32         = IM_COL32(225, 225, 235, 255);
constexpr ImU32 kCardBgU32       = IM_COL32( 26,  26,  26, 255);  // ~0.10 alpha grey
constexpr ImU32 kCardBgHoverU32  = IM_COL32( 42,  42,  42, 255);

// ── Chat panel geometry and accents (kept local) ────────────────────────────
constexpr float kChatFormH   = 132.0f;  // height reserved for the pinned form
constexpr float kChatSendW   = 96.0f;
constexpr float kChatGap     = 12.0f;
constexpr float kChatFieldH  = 64.0f;

constexpr ImU32 kUserTextU32  = IM_COL32(160, 158, 178, 255);
constexpr ImU32 kStoppedU32   = IM_COL32(148, 148, 158, 255);
constexpr ImU32 kErrorU32     = IM_COL32(226, 108,  92, 255);
constexpr ImU32 kLoadingU32   = IM_COL32(148, 148, 158, 255);

// ImGui widgets take an ImVec4; the packed accents above are for draw lists.
ImVec4 WidgetColor(ImU32 packed) noexcept {
    return ImGui::ColorConvertU32ToFloat4(packed);
}

ImFont* g_heading_font = nullptr;

// The question draft lives in file scope so it survives a refused Submit (a
// full queue keeps the text for the next attempt).
char g_question[lsxhome::ChatSession::kMaxPromptChars] = "";

void DrawAsterisk(ImDrawList* dl, ImVec2 c, float r, float t, ImU32 col) noexcept {
    // Three full lines through the centre at 60deg -> a six-ray asterisk with
    // round caps (filled circles at each end).
    for (int i = 0; i < 3; ++i) {
        const float a = static_cast<float>(i) * 3.14159265f / 3.0f + 3.14159265f / 6.0f;
        const ImVec2 d(std::cos(a) * r, std::sin(a) * r);
        dl->AddLine(ImVec2(c.x - d.x, c.y - d.y), ImVec2(c.x + d.x, c.y + d.y), col, t);
        dl->AddCircleFilled(ImVec2(c.x - d.x, c.y - d.y), t * 0.5f, col);
        dl->AddCircleFilled(ImVec2(c.x + d.x, c.y + d.y), t * 0.5f, col);
    }
}

void DrawIconX(ImDrawList* dl, ImVec2 c, float r, float t, ImU32 col) noexcept {
    dl->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), col, t);
    dl->AddLine(ImVec2(c.x - r, c.y + r), ImVec2(c.x + r, c.y - r), col, t);
}

void DrawIconRefresh(ImDrawList* dl, ImVec2 c, float r, float t, ImU32 col) noexcept {
    dl->AddCircle(c, r, col, 24, t);
    const float a = 3.14159265f * 0.25f;
    const ImVec2 tip(c.x + r * std::cos(a), c.y - r * std::sin(a));
    dl->AddTriangleFilled(tip,
                          ImVec2(tip.x - 5.0f, tip.y + 4.0f),
                          ImVec2(tip.x + 2.0f, tip.y + 1.0f), col);
}

void DrawPlanChip(ImDrawList* dl, const ImVec2& tl, float fs, const char* label) noexcept {
    const ImVec2 ts = ImGui::CalcTextSize(label);
    const ImVec2 size(ts.x + 24.0f, fs + 8.0f);
    dl->AddRectFilled(tl, ImVec2(tl.x + size.x, tl.y + size.y),
                      kPurpleChipU32, kWelcomeRound);
    dl->AddText(ImVec2(tl.x + 12.0f, tl.y + 4.0f), kChipTextU32, label);
}

void DrawPickerCombo(const char* id, const char* preview) noexcept {
    if (ImGui::BeginCombo(id, preview, ImGuiComboFlags_NoPreview)) {
        ImGui::Selectable(preview);
        ImGui::EndCombo();
    }
}

void DrawQuickActionCard(ImDrawList* dl, const ImVec2& tl, const ImVec2& sz,
                         const char* label) noexcept {
    const bool hovered =
        ImGui::IsMouseHoveringRect(tl, ImVec2(tl.x + sz.x, tl.y + sz.y));
    dl->AddRectFilled(tl, ImVec2(tl.x + sz.x, tl.y + sz.y),
                      hovered ? kCardBgHoverU32 : kCardBgU32, kWelcomeRound);
    const float pad = 10.0f;
    dl->AddText(ImVec2(tl.x + pad + 2.0f, tl.y + pad), kTextU32, label);
}

void DrawMCPTools(ImDrawList* dl, const ImVec2& pos, ImVec2 size, const char* label) noexcept {
    const float fs = ImGui::GetFontSize();
    const float fs2 = fs * 0.42f;
    ImGui::SetCursorScreenPos(pos);
    ImGui::InvisibleButton("##mcp_tools", size);
    const bool hovered = ImGui::IsItemHovered();
    // Fork/puzzle: two dots + count + label.
    dl->AddCircleFilled(ImVec2(pos.x + fs2, pos.y + fs * 0.55f), fs2, kCoralU32);
    dl->AddCircleFilled(ImVec2(pos.x + fs2 * 2.0f + 3.0f, pos.y + fs * 0.55f), fs2,
                        kMutedU32);
    float x = pos.x + fs2 * 3.0f + 6.0f;
    dl->AddText(ImVec2(x, pos.y + 1.0f), kTextU32, "28");
    x += ImGui::CalcTextSize("28").x;
    dl->AddText(ImVec2(x, pos.y + 1.0f), kMutedU32, label);
    if (hovered) {
        ImGui::SetTooltip("Active MCP tools available to this session");
    }
}

const char* PhaseLabel(lsxhome::ChatPhase phase) noexcept {
    switch (phase) {
        case lsxhome::ChatPhase::kIdle:       return "idle";
        case lsxhome::ChatPhase::kLoading:    return "loading the model";
        case lsxhome::ChatPhase::kGenerating: return "generating";
        case lsxhome::ChatPhase::kError:      return "error";
    }
    return "idle";
}

// The shared question widget: the welcome card and the pinned chat form render
// the same input, so both can submit the same draft.
bool DrawQuestionInput(float width, float height, bool transparent_bg) noexcept {
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, kWelcomeRound);
    ImGui::PushStyleColor(
        ImGuiCol_FrameBg,
        transparent_bg ? ImVec4(0.0f, 0.0f, 0.0f, 0.0f)
                       : MakeVec4(0.10f, 0.10f, 0.10f, 1.00f));
    // Enter submits; Shift+Enter still inserts a newline (ImGui checks the shift
    // state before EnterReturnsTrue).
    const bool entered = ImGui::InputTextMultiline(
        "##question", g_question, IM_ARRAYSIZE(g_question),
        ImVec2(width, height),
        ImGuiInputTextFlags_EnterReturnsTrue |
            ImGuiInputTextFlags_NoHorizontalScroll);
    ImGui::PopStyleColor(1);
    ImGui::PopStyleVar(1);
    return entered;
}

/// Send / Stop. Returns true only for a real send press: while a turn is in
/// flight the same button stops it, and a stop is never a submit.
bool DrawSendStopButton(lsxhome::ChatSession& session) noexcept {
    // Busy (not phase) is the gate: a queued question has not reached the worker
    // yet, so its phase is still kIdle.
    if (session.Busy()) {
        if (ImGui::Button("Stop", ImVec2(kChatSendW, 0.0f))) {
            session.RequestStop();
        }
        return false;
    }
    return ImGui::Button("Send", ImVec2(kChatSendW, 0.0f));
}

void DrawStatusLines(const lsxhome::ChatSession& session,
                     const lsxhome::ChatState& state) noexcept {
    if (session.phase() == lsxhome::ChatPhase::kLoading) {
        ImGui::TextColored(WidgetColor(kLoadingU32), "Loading the model...");
    }
    if (!session.error().empty()) {
        ImGui::TextColored(WidgetColor(kErrorU32), "%s", session.error().c_str());
    }
    if (!state.error().empty()) {
        ImGui::TextColored(WidgetColor(kErrorU32), "%s", state.error().c_str());
    }
}

void SubmitDraft(lsxhome::ChatSession& session, lsxhome::ChatState& state,
                 bool requested) noexcept {
    if (!requested || session.Busy() || g_question[0] == '\0') {
        return;
    }
    // Queue first: a refused Submit (full queue / oversized question) must leave
    // the transcript untouched and keep the draft for the next attempt.
    if (!session.Submit(g_question)) {
        return;
    }
    state.BeginTurn(g_question);
    g_question[0] = '\0';
}

void DrawTranscript(const lsxhome::ChatState& state) noexcept {
    for (const lsxhome::ChatMessage& message : state.messages()) {
        const bool is_user = message.role == "user";
        ImGui::PushStyleColor(ImGuiCol_Text, is_user ? kUserTextU32 : kTextU32);
        ImGui::TextUnformatted(is_user ? "You" : "LogestiX");
        ImGui::PopStyleColor();
        ImGui::TextWrapped("%s", message.text.c_str());
        if (message.interrupted) {
            ImGui::TextColored(WidgetColor(kStoppedU32), "- stopped");
        }
        ImGui::Separator();
    }
    // Follow the tail, but never yank the view away from a user who scrolled up.
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f) {
        ImGui::SetScrollHereY(1.0f);
    }
}

void DrawChatForm(lsxhome::ChatSession& session, lsxhome::ChatState& state) noexcept {
    const float field_w =
        ImGui::GetContentRegionAvail().x - kChatSendW - kChatGap;
    const bool typed_enter = DrawQuestionInput(field_w, kChatFieldH, false);
    ImGui::SameLine();
    const bool pressed_send = DrawSendStopButton(session);
    DrawStatusLines(session, state);
    SubmitDraft(session, state, typed_enter || pressed_send);
}

}  // namespace

void PrefillQuestion(const char* text) noexcept {
    std::snprintf(g_question, sizeof(g_question), "%s", text ? text : "");
}

void SetHeadingFont(ImFont* font) noexcept {
    g_heading_font = font;
}

static void DrawClaudeWelcomeInterface(lsxhome::ChatSession& session,
                                       lsxhome::ChatState& state) noexcept {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 avail = ImGui::GetContentRegionAvail();

    // Horizontal + vertical centering of the fixed-width column.
    const float stack_h = kHeaderH + kVGap + kCardH + kVGap + kActionsH;
    const float start_x = ImMax(0.0f, (avail.x - kWelcomeColumn) * 0.5f);
    const float start_y = ImMax(0.0f, (avail.y - stack_h) * 0.5f);
    ImGui::SetCursorPos(ImVec2(start_x, start_y));
    const ImVec2 base = ImGui::GetCursorScreenPos();
    const float fs = ImGui::GetFontSize();

    // ── 1) Header: coral asterisk + 34px greeting + purple plan chip ──
    {
        const float glyph_r = 13.0f;
        const float glyph_cy = base.y + kHeaderH * 0.5f;
        DrawAsterisk(dl, ImVec2(base.x + 18.0f, glyph_cy), glyph_r, 3.5f, kCoralU32);
        ImGui::SetCursorScreenPos(ImVec2(base.x + 42.0f, base.y + 2.0f));
        if (g_heading_font) {
            ImGui::PushFont(g_heading_font);
        }
        ImGui::TextUnformatted("Good evening, User");
        if (g_heading_font) {
            ImGui::PopFont();
        }
        const ImVec2 ts = ImGui::CalcTextSize("Professional Plan");
        const ImVec2 chip_size(ts.x + 24.0f, fs + 8.0f);
        const ImVec2 chip_tl(base.x + kWelcomeColumn - chip_size.x, base.y + 6.0f);
        DrawPlanChip(dl, chip_tl, fs, "Professional Plan");
    }

    // ── 2) Central input card ──
    ImGui::SetCursorPos(ImVec2(start_x, start_y + kHeaderH + kVGap));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, MakeVec4(0.10f, 0.10f, 0.10f, 1.00f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, kWelcomeRound);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 14.0f));
    if (ImGui::BeginChild("##welcome_input_card", ImVec2(kWelcomeColumn, kCardH),
                          true)) {
        ImDrawList* child_dl = ImGui::GetWindowDrawList();
        const float bottom_row_h = 30.0f;
        const ImVec2 input_pos = ImGui::GetCursorScreenPos();
        const bool typed_enter =
            DrawQuestionInput(-1.0f, -bottom_row_h, /*transparent_bg=*/true);
        if (g_question[0] == '\0') {
            child_dl->AddText(input_pos, kMutedU32, "How can Claude help you today?");
        }

        // Bottom control row: model/style pickers left, MCP tools + Send right.
        const float row_y =
            ImGui::GetWindowPos().y + ImGui::GetWindowSize().y - bottom_row_h - 8.0f;
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowPos().x + 14.0f, row_y));
        DrawPickerCombo("##model", "Claude 3.5 Sonnet");
        ImGui::SameLine();
        DrawPickerCombo("##style", "Choose style");
        const char* mcp = "MCP tools available";
        const float fs3 = fs * 0.42f * 3.0f + 6.0f;
        const float mcp_w = fs3 + ImGui::CalcTextSize("28").x +
                            ImGui::CalcTextSize(mcp).x;
        const float send_w =
            ImGui::CalcTextSize(session.Busy() ? "Stop" : "Send").x + 20.0f;
        ImGui::SameLine(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x -
                        mcp_w - send_w - kChatGap - 8.0f);
        DrawMCPTools(child_dl,
                     ImVec2(ImGui::GetCursorScreenPos().x, row_y),
                     ImVec2(mcp_w, bottom_row_h), mcp);
        ImGui::SameLine();
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowPos().x +
                                             ImGui::GetWindowSize().x - send_w - 8.0f,
                                         row_y));
        const bool pressed_send = DrawSendStopButton(session);
        DrawStatusLines(session, state);
        SubmitDraft(session, state, typed_enter || pressed_send);
        ImGui::EndChild();
    }
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(1);

    // ── 3) Quick-action cards + vertical icon panel ──
    const float ay = start_y + kHeaderH + kVGap + kCardH + kVGap;
    const float panel_w = 60.0f;
    const float gap = 10.0f;
    const float card_w = (kWelcomeColumn - panel_w - gap * 3.0f) / 3.0f;
    const ImVec2 card_size(card_w, kActionsH);
    float cx = base.x;
    const char* labels[] = {"Provide stakeholder perspective",
                            "Extract insights from report",
                            "Polish your prose"};
    static const char* quick_action_ids[] = {"##quick_action_0",
                                             "##quick_action_1",
                                             "##quick_action_2"};
    for (int i = 0; i < 3; ++i) {
        // The button is invisible: the card below is drawn by hand, so only the
        // hit-rectangle and the click test come from ImGui. A click prefills the
        // question draft instead of firing a canned prompt.
        ImGui::SetCursorScreenPos(ImVec2(cx, ay));
        ImGui::InvisibleButton(quick_action_ids[i], card_size);
        if (ImGui::IsItemClicked()) {
            PrefillQuestion(labels[i]);
        }
        DrawQuickActionCard(dl, ImVec2(cx, ay), card_size, labels[i]);
        cx += card_w + gap;
    }
    // Vertical panel with clear (X) and refresh (cycle) icon buttons.
    const ImVec2 panel_tl(base.x + kWelcomeColumn - panel_w, ay);
    dl->AddRectFilled(panel_tl, ImVec2(panel_tl.x + panel_w, panel_tl.y + kActionsH),
                      kCardBgU32, kWelcomeRound);
    const float pcy = panel_tl.x + panel_w * 0.5f;
    DrawIconX(dl, ImVec2(pcy, panel_tl.y + 24.0f), 7.0f, 2.0f, kTextU32);
    DrawIconRefresh(dl, ImVec2(pcy, panel_tl.y + 60.0f), 9.0f, 2.0f, kTextU32);
}

void ApplyBlackwellCoworkTheme() noexcept {
    ImGuiStyle& style = ImGui::GetStyle();

    // Hardlocked matte Blackwell palette. Strict dark graphite panels, thin
    // dark-gray borders, deep matte-charcoal chrome and soft ivory text — no
    // blue tones remain anywhere.
    style.Colors[ImGuiCol_WindowBg] = MakeVec4(0.07f, 0.07f, 0.08f, 1.00f);
    style.Colors[ImGuiCol_ChildBg] = MakeVec4(0.11f, 0.11f, 0.12f, 1.00f);
    style.Colors[ImGuiCol_Border] = MakeVec4(0.18f, 0.18f, 0.20f, 1.00f);
    style.Colors[ImGuiCol_Button] = MakeVec4(0.14f, 0.14f, 0.16f, 1.00f);
    style.Colors[ImGuiCol_ButtonHovered] = MakeVec4(0.20f, 0.20f, 0.22f, 1.00f);
    style.Colors[ImGuiCol_ButtonActive] = MakeVec4(0.24f, 0.24f, 0.26f, 1.00f);
    style.Colors[ImGuiCol_Header] = MakeVec4(0.14f, 0.14f, 0.16f, 1.00f);
    style.Colors[ImGuiCol_HeaderHovered] = MakeVec4(0.20f, 0.20f, 0.22f, 1.00f);
    style.Colors[ImGuiCol_HeaderActive] = MakeVec4(0.24f, 0.24f, 0.26f, 1.00f);
    style.Colors[ImGuiCol_TitleBg] = MakeVec4(0.14f, 0.14f, 0.16f, 1.00f);
    style.Colors[ImGuiCol_TitleBgActive] = MakeVec4(0.14f, 0.14f, 0.16f, 1.00f);
    style.Colors[ImGuiCol_TitleBgCollapsed] = MakeVec4(0.14f, 0.14f, 0.16f, 1.00f);
    style.Colors[ImGuiCol_Text] = MakeVec4(0.88f, 0.88f, 0.90f, 1.00f);
    style.Colors[ImGuiCol_TextDisabled] = MakeVec4(0.58f, 0.58f, 0.62f, 1.00f);

    // Cowork geometry: square main viewport, softly rounded panels and frames,
    // zero window chrome so the dock host fuses edge-to-edge with the screen.
    style.WindowRounding = 0.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize = 0.0f;
    style.FrameBorderSize = 0.0f;
    style.ItemSpacing = ImVec2(8.0f, 8.0f);

    style.FramePadding = ImVec2(6.0f, 6.0f);
    style.WindowPadding = ImVec2(6.0f, 6.0f);
    style.PopupRounding = 6.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding = 6.0f;
    style.TabRounding = 6.0f;
}

void BuildWorkspaceSkeleton(ChatSession& session, ChatState& state) noexcept {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();

    // Edge-to-edge root dock host. Covers the full viewport with zero chrome
    // and zero rounding, so no title bar, collapsing triangle, border, or
    // floating window can ever escape the graphite backdrop.
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const ImGuiWindowFlags host_flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    ImGui::Begin("##LSXHomeDockHost", nullptr, host_flags);
    ImGui::PopStyleVar(3);

    // Lock the panels together across the full viewport. Passthru keeps the
    // dock transparent so the host's seamless children stay fully visible.
    ImGui::DockSpaceOverViewport(0, viewport,
                                 ImGuiDockNodeFlags_PassthruCentralNode);

    // Sidebar: matte charcoal panel, seamlessly inset into the root block.
    ImGui::BeginChild("##sidebar", ImVec2(kSidebarWidth, 0.0f), false);
    {
        ImGui::TextUnformatted("LogestiX Home");
        ImGui::Separator();
        ImGui::TextUnformatted("Session");

        ImGui::Separator();
        // The chat panel is now the bridge's only consumer, so the old
        // "Drain pool" button is gone: it would steal streamed chunks from the
        // transcript. Telemetry stays.
        ImGui::Text("Phase: %s", PhaseLabel(session.phase()));
        ImGui::Text("Published: %zu\nDropped: %zu\nPoolSlots: %zu",
                    session.Published(), session.Dropped(),
                    GuiBridge::kPoolSlots);
        if (ImGui::Button("Clear conversation")) {
            state.Clear();
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // Main content: welcome screen or conversation, seamless sibling block.
    ImGui::BeginChild("##main", ImVec2(0.0f, 0.0f), false);
    {
        DrawChatPanel(session, state);
    }
    ImGui::EndChild();

    ImGui::End();
}

void DrawChatPanel(ChatSession& session, ChatState& state) noexcept {
    // Drain first: the worker publishes the last chunk before it reports the
    // turn as finished, so a phase check that ran earlier would close the reply
    // with its tail unread.
    session.DrainInto(state);
    if (state.Busy() && !session.Busy()) {
        state.EndTurn(session.phase() == ChatPhase::kError
                          ? true
                          : session.TakeInterrupted());
    }

    if (state.empty()) {
        DrawClaudeWelcomeInterface(session, state);
        return;
    }

    if (ImGui::BeginChild("##transcript", ImVec2(0.0f, -kChatFormH), true)) {
        DrawTranscript(state);
    }
    ImGui::EndChild();

    ImGui::SetCursorScreenPos(
        ImVec2(ImGui::GetWindowPos().x + 14.0f,
               ImGui::GetWindowPos().y + ImGui::GetWindowSize().y -
                   kChatFormH + 8.0f));
    DrawChatForm(session, state);
}

}  // namespace lsxhome
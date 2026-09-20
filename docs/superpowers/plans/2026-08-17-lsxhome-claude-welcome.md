# lsxhome Claude-Style Welcome Interface — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a Claude-style welcome screen (`DrawClaudeWelcomeInterface`) as the content of the `##main` panel in the lsxhome shell, with a 34px heading font, a centered input card, and quick-action cards.

**Architecture:** Two self-contained additive changes to existing lsxhome files: (1) a new large Inter heading font loaded via `FontLoader::LoadHeading`, and (2) a new render function + module font accessor in `gui_renderer` wired into `BuildWorkspaceSkeleton` in place of the ImPlot panel. No new files; the existing `##main` child is repurposed.

**Tech Stack:** C++20, Dear ImGui 1.92.9-docking, ImDrawList, imgui_freetype LCD rasterizer, CMake (VS 18 2026 generator, Release), Catch2 (existing tests, no new tests needed).

**Spec:** `docs/superpowers/specs/2026-08-17-lsxhome-claude-welcome-design.md`

---

## Environment

- Build dir: `D:\Projects\GitHub\logestix\build` (already configured, VS 18 2026, Release).
- CMake (not on PATH): `$cmake = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"`
- CTest: `$ctest = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"`
- Build a target: `& $cmake --build build --config Release --target <target>`
- Run lsxhome tests: `& $ctest --test-dir build -C Release -R "lsxhome" --output-on-failure`

Verified: `build/CMakeCache.txt` reports `CMAKE_GENERATOR=Visual Studio 18 2026`, `CMAKE_BUILD_TYPE=Release`.

---

### Task 1: Add a 34px Inter heading font via FontLoader

**Files:**
- Modify: `include/lsxhome/font_loader.h`
- Modify: `src/font_loader.cpp`
- Modify: `src/main_win32.cpp`

- [ ] **Step 1: Declare `LoadHeading` in the header**

Edit `include/lsxhome/font_loader.h` — add a third method after `LoadMonospace`:

```cpp
    /// Loads the embedded Inter at a larger point size for the welcome screen's
    /// hero greeting. Same FreeType LCD rasterizer settings as the base font.
    static ImFont* LoadHeading(ImFontAtlas& atlas) noexcept;
```

- [ ] **Step 2: Implement `LoadHeading` in the source**

Edit `src/font_loader.cpp` — append inside `namespace lsxhome`:

```cpp
ImFont* FontLoader::LoadHeading(ImFontAtlas& atlas) noexcept {
    constexpr unsigned int kFlags = ImGuiFreeTypeLoaderFlags_LightHinting |
                                    ImGuiFreeTypeLoaderFlags_ForceAutoHint;
    return AddEmbedded(atlas, kFlags,
                       static_cast<const unsigned char*>(lsxhome_font_payload),
                       lsxhome_font_payload_size, 34.0f);
}
```

- [ ] **Step 3: Wire the heading font into the frame**

Edit `src/main_win32.cpp` — right after the `FontLoader::LoadDefault(*io.Fonts);` call at line 144:

```cpp
    // Hero heading face for the welcome screen's 34px greeting.
    lsxhome::FontLoader::LoadHeading(*io.Fonts);
```

- [ ] **Step 4: Build to verify**

Run: `& $cmake --build build --config Release --target lsxhome`
Expected: link succeeds (no font target changes — both face sizes reuse the same embedded `lsxhome_font_payload`).

- [ ] **Step 5: Commit**

```bash
git add include/lsxhome/font_loader.h src/font_loader.cpp src/main_win32.cpp
git commit -m "feat: load 34px Inter heading font for lsxhome welcome screen"
```

---

### Task 2: Implement the Claude-style welcome interface

**Files:**
- Modify: `include/lsxhome/gui_renderer.h`
- Modify: `src/gui_renderer.cpp`
- Modify: `src/main_win32.cpp`

- [ ] **Step 1: Declare the new API in the header**

Replace the entire contents of `include/lsxhome/gui_renderer.h` with:

```cpp
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
```

- [ ] **Step 2: Add helper widgets + implementation in the source**

Edit `src/gui_renderer.cpp`:

First, extend the includes (keep the existing ones):

```cpp
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cfloat>
```

Then extend the anonymous namespace (replace the block ending at line 26):

```cpp
constexpr ImVec4 MakeVec4(float r, float g, float b, float a = 1.0f) noexcept {
    return ImVec4(r, g, b, a);
}

constexpr float kSidebarWidth = 300.0f;
constexpr int kPlotSamples = 256;

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

ImFont* g_heading_font = nullptr;

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

void DrawPickerCombo(ImGuiID id, const char* preview) noexcept {
    char label[64];
    ImFormatString(label, sizeof(label), "##picker_%lld", static_cast<long long>(id));
    if (ImGui::BeginCombo(label, preview, ImGuiComboFlags_NoPreview)) {
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

}  // namespace
```

- [ ] **Step 2b: Wire the heading font into the renderer via main_win32**

The `LoadHeading` call in Task 1 currently discards its return value, so the
welcome interface would never receive the 34px face. In
`src/main_win32.cpp`, replace the caption/assignment set in Task 1
so the returned face is handed to the renderer:

```cpp
    // Hero heading face for the welcome screen's 34px greeting.
    lsxhome::SetHeadingFont(lsxhome::FontLoader::LoadHeading(*io.Fonts));
```

(`SetHeadingFont` now exists in `lsxhome/gui_renderer.h`; no new include is
needed — `main_win32.cpp` already includes `lsxhome/gui_renderer.h`.)

- [ ] **Step 3: Implement `SetHeadingFont` + `DrawClaudeWelcomeInterface`**

Edit `src/gui_renderer.cpp` — insert these two functions right after the closing `}  // namespace` (which now ends around the block above) and before `ApplyBlackwellCoworkTheme()`:

```cpp
void SetHeadingFont(ImFont* font) noexcept {
    g_heading_font = font;
}

void DrawClaudeWelcomeInterface() noexcept {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImGuiStyle& g = ImGui::GetStyle();
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
        const float bottom_row_h = 30.0f;
        const ImVec2 input_pos = ImGui::GetCursorScreenPos();
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, kWelcomeRound);
        static char input_buf[512] = "";
        ImGui::InputTextMultiline(
            "##welcome_input", input_buf, IM_ARRAYSIZE(input_buf),
            ImVec2(-1.0f, -bottom_row_h), ImGuiInputTextFlags_NoHorizontalScroll);
        ImGui::PopStyleVar(1);
        ImGui::PopStyleColor(1);
        if (input_buf[0] == '\0') {
            dl->AddText(input_pos, kMutedU32, "How can Claude help you today?");
        }

        // Bottom control row: model/style pickers left, MCP tools right.
        const float row_y =
            ImGui::GetWindowPos().y + ImGui::GetWindowSize().y - bottom_row_h - 8.0f;
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowPos().x + 14.0f, row_y));
        DrawPickerCombo(ImGui::GetID("model"), "Claude 3.5 Sonnet");
        ImGui::SameLine();
        DrawPickerCombo(ImGui::GetID("style"), "Choose style");
        const char* mcp = "MCP tools available";
        const float fs3 = fs * 0.42f * 3.0f + 6.0f;
        const float mcp_w = fs3 + ImGui::CalcTextSize("28").x +
                            ImGui::CalcTextSize(mcp).x;
        ImGui::SameLine(ImGui::GetWindowPos().x + ImGui::GetWindowSize().x -
                        mcp_w - 8.0f);
        DrawMCPTools(dl,
                     ImVec2(ImGui::GetCursorScreenPos().x,
                            row_y),
                     ImVec2(mcp_w, bottom_row_h), mcp);
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
    for (int i = 0; i < 3; ++i) {
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
```

- [ ] **Step 4: Repurpose the ##main panel**

In `src/gui_renderer.cpp`, inside `BuildWorkspaceSkeleton`, replace the body of the `##main` child block (currently the ImPlot Token Throughput panel at lines 117-130) with a call to the welcome interface:

```cpp
    ImGui::SameLine();

    // Main content: Claude-style welcome screen, seamless sibling block.
    ImGui::BeginChild("##main", ImVec2(0.0f, 0.0f), false);
    {
        DrawClaudeWelcomeInterface();
    }
    ImGui::EndChild();
```

Also `#include <implot.h>` at the top of `gui_renderer.cpp` is no longer used by this file — remove it to keep the code clean. (`ApplyBlackwellCoworkTheme` doesn't reference ImPlot.)

- [ ] **Step 5: Build to verify**

Run: `& $cmake --build build --config Release --target lsxhome`
Expected: compiles and links cleanly.

- [ ] **Step 6: Run existing tests to confirm no regression**

Run: `& $ctest --test-dir build -C Release -R "lsxhome" --output-on-failure`
Expected: `test_lsxhome_gui` and `test_lsxhome_font` PASS.

- [ ] **Step 7: Commit**

```bash
git add include/lsxhome/gui_renderer.h src/gui_renderer.cpp src/main_win32.cpp
git commit -m "feat: render Claude-style welcome interface in lsxhome ##main panel"
```

---

## Optional cleanups / verification notes

- The plot code was only token-throttle telemetry; it is intentionally removed by this design. If the plot must be retained, it should be moved to a new dockable node — but per the approved spec it is replaced.
- `LogestiX Home`, `Sessions`, and the `Drain pool` block in the sidebar are untouched.
- Manual QA: run the `lsxhome` exe and confirm the welcome screen renders centered with the coral asterisk, purple chip, input card, and quick-action row. No automated ImGui snapshot tests exist in this project.

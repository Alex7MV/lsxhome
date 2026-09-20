# lsxhome — Claude-Style Welcome Interface (Design)

- **Date:** 2026-08-17
- **Branch:** `feat/lsxhome-design`
- **Status:** Approved

## Goal

Recreate the Claude (web/desktop) welcome screen as a Dear ImGui (v1.92.9-docking)
layout inside the lsxhome workspace, matching the attached reference: a large
«Good evening, User» greeting with a coral asterisk glyph, a centered input card
with model/style pickers and an MCP-tools indicator, and a row of quick-action
button cards.

## Scope & Placement

- New self-contained function `void DrawClaudeWelcomeInterface()` implemented in
  `src/gui_renderer.cpp`, declared in
  `include/lsxhome/gui_renderer.h`.
- It becomes the **entire content** of the existing `##main` child inside
  `BuildWorkspaceSkeleton()`, **replacing** the ImPlot Token Throughput panel.
  The 300px sidebar is left unchanged.
- Called once per frame before `renderer.EndFrame(true)` (wired into
  `BuildWorkspaceSkeleton`, no call-site change in `main_win32.cpp`).

## Font Changes

- Only 15px Inter (default) + 14px JetBrains Mono are currently loaded. The
  greeting needs a large face.
- Add `FontLoader::LoadHeading(ImFontAtlas&) -> ImFont*` in
  `src/font_loader.cpp` / `font_loader.h`, loading the embedded
  Inter TTF at **34px** with the same imgui_freetype LCD flags
  (`LightHinting | ForceAutoHint`, `OversampleH=2`, `OversampleV=1`).
- `main_win32.cpp` calls `LoadHeading(*io.Fonts)` once after `LoadDefault`.
- Expose a small module accessor in `gui_renderer` (e.g.
  `SetHeadingFont(const ImFont*)` + private `g_heading_font`) so the welcome
  interface can `ImGui::PushFont`/`PopFont` without threading the pointer
  through signatures.

## Layout

All horizontal geometry is centered: compute `avail = GetContentRegionAvail().x`,
fixed column width (≈700px), and `ImGui::SetCursorPosX((avail - col_width) * 0.5f)`.
Spacing derived from `GetFontSize()` where practical — minimal magic numbers.

### 1. Header row
- Coral-orange asterisk drawn with `ImDrawList`: several thick, crossing line
  segments with round caps (`AddLine` with `thickness` and `ImDrawFlags_RoundCorners`
  on a filled circle cap, or explicit round-cap circles), ~`#FF6B4A`.
- `ImGui::PushFont(g_heading_font)` then **«Good evening, User»** in `Text`
  `0.88` color, `PopFont`.
- Small **«Professional Plan»** chip after the greeting: rounded plash
  (`rounding 6f`) purple background, small text.

### 2. Central input card
- `ImGui::BeginChild` with `ChildBg 0.10f` and rounding `6f` (per spec, overriding
  theme's `8f`), child border.
- Borderless `ImGui::InputTextMultiline` (own buffer), hint
  **«How can Claude help you today?»** in muted color, expanding to fill the card.
  **Interactive (typing/cursor) but no submit logic.**
- Bottom control row inside the child:
  - Left: two combo/selectable pickers — model **«Claude 3.5 Sonnet»** and style
    **«Choose style»**.
  - Right: **«28 MCP tools available»** — fork/puzzle icon via ImDrawList + `28`,
    hover → `ImGui::SetTooltip(...)`.

### 3. Quick-actions row
- Three equal-width button cards: «Provide stakeholder perspective»,
  «Extract insights from report», «Polish your prose».
- Each is a custom widget: text top-left aligned, internal padding, hover lifts
  background alpha.
- Right vertical panel: **✕** (clear) and **↻** (refresh) icon buttons.

## Color & Geometry

- Use `kPalette` (BlackwellPalette) dark matte values for backgrounds (`window_bg`,
  `field_bg`, `border`, `text`, `text_secondary`).
- Card rounding = `6.0f` per the request (overrides `kGeometry.rounding = 8f`).
- Dedicated accent colors, not hardcoded into the shared palette, for the coral
  asterisk and purple chip (kept local to the welcome renderer).

## Testing

- No change to existing `test_lsxhome_gui.cpp` (SPSC bridge invariants).
- The GUI rendering function is not unit-tested directly (no headless ImGui
  context harness in the project today). The font-loader addition is covered by
  the existing `test_lsxhome_font` compile/link path.

## Out of Scope (YAGNI)

- Submit/Enter logic, wiring to generation pipeline.
- Persistence of model/style combo selections.
- Any change to the sidebar, dock host, or theme defaults.

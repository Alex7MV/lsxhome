# lsxhome — AGENTS.md

## Project

Windows-native x64 desktop shell for the [logestix](https://github.com/Alex7MV/logestix)
synthesis core. C++20. CMake 3.25+ (`CMakePresets.json`), Visual Studio 18 2026
generator on Windows.

D3D12 + Dear ImGui (docking branch) immediate-mode stack. The engine's compute
thread publishes decoded tokens into a lock-free SPSC `GuiBridge`; the UI thread
drains them each frame. Strict separation: the UI thread never touches the model,
the compute thread never touches the window.

Windows-only: on any other host the whole build is a no-op (early `return()` in
`CMakeLists.txt`), so the tree configures cleanly on CI and Linux development
hosts. `linux-check` exists only to verify that no-op path.

## Dependencies

| Dependency | Version | Source | License | Type |
|---|---|---|---|---|
| logestix (`lsxcommon`) | pinned SHA `7c03e12…` | FetchContent / local override | AGPL-3.0 | Static + shared (`arrow.dll`) |
| Dear ImGui | `v1.92.9b-docking` | FetchContent | MIT | Static (`lsxhome_imgui`) |
| ImPlot | `v1.0` | FetchContent | MIT | Static (`lsxhome_implot`) |
| ImNodes | `v0.5` | FetchContent | MIT | Static (`lsxhome_imnodes`) |
| FreeType | `VER-2-14-3` | FetchContent | FTL (of FTL/GPLv2) | Static |
| Catch2 | `v3.16.0` | FetchContent (test only) | BSL-1.0 | Static |
| Inter | v4.001 | `assets/Inter-Regular.ttf` | SIL OFL 1.1 | Embedded TTF |
| JetBrains Mono | v2.304 | `assets/JetBrainsMono-Regular.ttf` | SIL OFL 1.1 | Embedded TTF |

Engine bring-up flags are forced OFF in `CMakeLists.txt`
(`LOGESTIX_BUILD_APPS`/`TOOLS`/`TESTS`), so only `lsxcommon` is produced. To
develop against a local engine checkout instead of the pinned remote, pass
`-DLSXHOME_LOGESTIX_SOURCE_DIR=<path>` at configure time.

## Build

```bash
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug          # label `fast`

cmake --preset windows-release
cmake --build --preset windows-release
```

`cmake --preset linux-check` configures the no-op path on Linux/macOS.

## Test

`BUILD_TESTING=ON` in the Windows presets; Catch2 v3.16.0 via FetchContent.
Tests are Windows-only (they link `lsxcommon`), label `fast`:

- `test_lsxhome_gui` — `TokenPayload` layout and producer clock stamp, SPSC
  bridge round-trip / backpressure / zero-allocation invariants, refused-drain
  safety, fallback-token labelling, token-text capacity, space-splitting of
  generated text, SRV descriptor slot recycling, back-buffer install/invalidate
  bookkeeping.
- `test_lsxhome_font` — the embedded JetBrains Mono payload is non-empty and a
  valid TrueType container.
- `patch_drift_guard` — the after-fetch patch helper rewrites a live key,
  refuses a drifted key, and never re-wraps or skips a self-nested key.

There is no headless ImGui harness; GUI rendering is not unit-tested. Device-side
D3D12 work (swap-chain resize, descriptor-handle arithmetic) stays untested —
only its slot bookkeeping is factored into `srv_descriptor_pool.h` so the
alloc/free contract is verifiable without a GPU.

## Structure

- `src/` — `main_win32.cpp` (WinMain + compute producer thread), `d3d12_renderer.cpp`,
  `font_loader.cpp`, `gui_renderer.cpp` (Blackwell/Cowork theme + welcome interface).
- `include/lsxhome/` — `d3d12_renderer.h`, `font_loader.h`, `gui_bridge.h`,
  `spsc_token_ring.h`, `srv_descriptor_pool.h`, `swapchain_targets.h`,
  `text_splitter.h`, `blackwell_theme.h`, `gui_renderer.h`,
  `imnodes_offsetof_shim.h`.
- `cmake/` — `embed_binary.cmake` + `embed_font.ps1` (TTF → byte-array TU),
  `patch_vendored_sources.cmake` (after-fetch rewrite helper),
  `copy_libzmq_source.cmake` (Release MPL-2.0 source bundle).
- `assets/` — embedded fonts and their license texts.
- `tests/` — Catch2 suite (label `fast`).
- `docs/superpowers/` — design specs and implementation plans.

## CMake Key Points

- CUDA is enabled in the top-level `project()` before the engine's
  `add_subdirectory` — under CMake 4.x a nested directory being first to enable
  CUDA breaks. `find_package(CUDAToolkit REQUIRED)`.
- `CMAKE_POLICY_VERSION_MINIMUM 3.5` is forced for Arrow's bundled xsimd.
- `CMAKE_RUNTIME_OUTPUT_DIRECTORY` = `${CMAKE_BINARY_DIR}/bin`.
- The GUI stack is built as separate static archives; `lsxhome_imgui` exposes
  `IMGUI_DEFINE_MATH_OPERATORS` PUBLIC. `IMGUI_DISABLE_OBSOLETE_FUNCTIONS` is
  deliberately NOT used (struct-layout desync across TUs).
- The engine needs exactly **one** patch (CUDA 13.4 Debug guard, below). At its
  pinned SHA everything else already works as a CMake subproject on Windows: its
  POSIX-only calls are guarded upstream (`setenv` in `kimi_moe_inference.cu` since
  `bbf6771`), the `${CMAKE_SOURCE_DIR}/third_party` paths became
  `PROJECT_SOURCE_DIR`, `M_PI` became `std::numbers::pi`, and `expert_store.h`
  guards `munmap` itself. `7c03e12` additionally exports the NVCC options the
  public headers require via an INTERFACE target, so consumers compiling their
  own `.cu` no longer have to rediscover them.
- Every after-fetch patch (engine, ImNodes, FreeType) goes through
  `lsxhome_replace_in_file()` in `cmake/patch_vendored_sources.cmake`, which
  rewrites each key exactly once and **hard-fails the configure** when a key is
  neither present nor already applied — a stale key means the pin moved, never a
  silent no-op. Contract: `tests/cmake/patch_drift_guard_test.cmake` (ctest
  `patch_drift_guard`). Current patches: ImNodes v0.5 `ImDrawCmd::TextureId` →
  `TexRef`, FreeType `FT_CONFIG_OPTION_SUBPIXEL_RENDERING` unmuted for
  imgui_freetype LCD glyphs.
- After-fetch patches must stay minimal, and a patch for an already-fixed issue
  is worse than a harmless leftover: its key usually survives inside an upstream
  `#else` branch, so the rewrite re-wraps the dependency's own fix on every
  configure. The Kimi `setenv` patch did exactly that (five nested `#ifdef`
  layers) before it was dropped. Re-verify necessity against
  `git show <pin>:<file>` on the *pristine* checkout before adding a patch.
- CUDA 13.4 caveat for any future `.cu` in this shell: `--expt-relaxed-constexpr`
  plus `_DEBUG` (implied by `/MDd`) makes NVVM abort device codegen with
  `parse Invalid instruction with no BB (Producer: 'LLVM23.0.0')` in
  `perf_calibration.cu` and `gigachat_moe_inference_pipeline.cu`. The engine
  passes the flag to its own TUs with no Debug guard, so the two upstream TUs
  fail; lsxhome compiles only `.cpp` and is unaffected. Reproduced on
  CUDA 13.4.92 / LLVM 23.
- FreeType is static with zlib/bzip2/brotli/harfbuzz/png discovery disabled.
- `lsx_embed_binary()` runs `embed_font.ps1` on Windows (linear-time byte
  emitter); the plain-CMake path is a slow fallback.
- POST_BUILD copies `THIRD_PARTY_NOTICES`, `THIRD_PARTY_NOTICES.logestix`,
  `arrow.dll`/`arrow_cuda.dll`, and (Release only) the libzmq source bundle.

## License Compliance

- The project is AGPL-3.0 (`LICENSE`).
- `THIRD_PARTY_NOTICES` collects the notices for components introduced here; the
  engine's own notices ship as `THIRD_PARTY_NOTICES.logestix`.
- libzmq is statically linked into `lsxcommon` (MPL-2.0). Release builds bundle
  its complete Source Code Form under `<bin>/third_party/libzmq/` via
  `cmake/copy_libzmq_source.cmake` (no-op for Debug), satisfying MPL-2.0 §3.2(a).
- Never commit third-party source that is not already covered by a notice; NVIDIA
  documentation is not vendored.

## Conventions

- English commit messages with prefixes `build:`/`feat:`/`fix:`/`refactor:`/
  `test:`/`docs:`.
- Commit only on explicit request.
- No per-file license headers in first-party sources; descriptive comments are
  the norm, and CMake scripts carry explanatory comments.
- Keep `THIRD_PARTY_NOTICES` and `README.md` in sync when adding or bumping a
  dependency.

## Subagent policy

- **No parallel/subagents**: using Task subagents (explore/general/parallel agents) is forbidden — they make many mistakes. All codebase research, audits and edits are done by the main session directly (Read/Grep/Edit/etc.).

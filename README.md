# lsxhome

Native Windows production environment and control interface for the
[logestix](https://github.com/Alex7MV/logestix) synthesis core.

`lsxhome` is a Windows-native x64 desktop shell built on a hardware-accelerated
D3D12 + Dear ImGui immediate-mode stack. The engine's compute thread publishes
decoded tokens to a lock-free SPSC bridge; the UI thread drains them every
frame.

Windows-only: the build is a no-op on other hosts, so the repository configures
cleanly anywhere (`linux-check` exists only to verify that no-op path).

## Layout

```
CMakeLists.txt        top-level build (Windows-only)
CMakePresets.json     windows-debug / windows-release / linux-check
cmake/                embed_binary.cmake + embed_font.ps1 (asset embedding),
                      patch_vendored_sources.cmake (after-fetch rewrites),
                      copy_libzmq_source.cmake (Release MPL-2.0 bundle)
src/                  main_win32 (WinMain + ChatSession wiring), d3d12_renderer,
                      font_loader, gui_renderer, lsx_generation_backend
                      (the only TU that translates between the shell and
                      lsxcommon vocabularies)
include/lsxhome/      chat_session, chat_state, generation_backend,
                      lsx_generation_backend, srv_descriptor_pool,
                      swapchain_targets, text_splitter, text_normalizer,
                      prompt_repair, history_budget, model_root,
                      blackwell_theme, gui_renderer, imnodes_offsetof_shim,
                      d3d12_renderer, font_loader, gui_bridge, spsc_token_ring
assets/               embedded TrueType fonts (Inter, JetBrains Mono) + licenses
tests/                Catch2 suite, label `fast`: test_lsxhome_gui,
                      test_lsxhome_font, test_lsxhome_chat, patch_drift_guard
docs/superpowers/     design specs and implementation plans
docs/repro/           the engine encoding-audit bundle (tokenizer_audit probe,
                      prompts, NVIDIA reproducer) and the engine fix prompts
```

## Chat

The main panel is a chat surface: type a question (Enter or **Send**), and the
logestix engine answers it token by token. **Stop** interrupts a running
generation, and every exchange is framed back to the model, so follow-up
questions keep their context. The model picker in the UI is still a visual stub;
the model path comes from `--model`, which accepts either the model root or the
converted `logestix/` directory inside it (both resolve to the same root).
`--run [PROMPT]` submits its prompt as the first chat turn (with no `--model` it
shows the deterministic GLM-5.2 self-check token instead). Because the shell is a
WIN32-subsystem app with no console, `--log FILE` redirects diagnostics to a file
for a headless run.

Threading: `ChatSession` owns a worker thread and a lock-free request ring; it
runs the engine and publishes decoded text into the `GuiBridge`. The UI thread
drains the bridge into a `ChatState` transcript every frame and never touches
the model. `chat_state.h` / `chat_session.h` are engine-free by construction,
which is why `test_lsxhome_chat` links Catch2 alone and runs without CUDA.

Engine text is normalized (`NormalizeEngineText`) before it is framed back into
the next turn: `arrow_tokenizer_decode` returns raw vocabulary bytes, so an
answer can carry byte-level BPE escapes or a lone high byte that the encoder
would reject. The normalizer runs on every streamed delta and every history
message, keeping the transcript, the prompt and the screen in agreement.

## Engine dependency

The inference engine (`lsxcommon`, and its Arrow/CUDA/abseil stack) comes from
[logestix](https://github.com/Alex7MV/logestix), pinned by a full commit SHA
(`LSXHOME_LOGESTIX_TAG`) and pulled with `FetchContent`. Logestix is built with
`LOGESTIX_BUILD_APPS=OFF`, `LOGESTIX_BUILD_TOOLS=OFF` and
`LOGESTIX_BUILD_TESTS=OFF`, so only the engine library is produced. The engine
needs no after-fetch patch at its pinned SHA.

The pin is a **FORCE** cache entry: bumping the SHA in `CMakeLists.txt` takes
effect on the next plain configure (no need to wipe `build/windows-*`, whose own
`CMakeCache.txt` would otherwise shadow the literal).

For joint development, point the build at a local logestix checkout instead of
the remote:

```powershell
cmake --preset windows-debug -DLSXHOME_LOGESTIX_SOURCE_DIR=C:/src/logestix
```

## Build

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug          # label `fast`
```

Release: `cmake --preset windows-release && cmake --build --preset windows-release`.

A `linux-check` preset configures the tree on Linux/macOS to verify the no-op
path.

## License

AGPL-3.0 (see `LICENSE`). Third-party notices in `THIRD_PARTY_NOTICES`; the
engine's own notices are shipped as `THIRD_PARTY_NOTICES.logestix`.

Release builds also bundle the libzmq Source Code Form (MPL-2.0) under
`third_party/libzmq/`.

# lsxhome

Native Windows production environment and control interface for the
[logestix](https://github.com/Alex7MV/logestix) synthesis core.

`lsxhome` is a Windows-native x64 desktop shell built on a hardware-accelerated
D3D12 + Dear ImGui immediate-mode stack. The engine's compute thread publishes
decoded tokens to a lock-free SPSC bridge; the UI thread drains them every
frame.

Windows-only: the build is a no-op on other hosts, so the repository configures
cleanly anywhere.

## Layout

```
CMakeLists.txt        top-level build (Windows-only)
CMakePresets.json     windows-debug / windows-release / linux-check
cmake/                font/asset embedding helpers
src/                  main_win32, d3d12_renderer, font_loader, gui_renderer,
                      lsx_generation_backend
include/lsxhome/      blackwell_theme, chat_session, chat_state, d3d12_renderer,
                      font_loader, generation_backend, gui_bridge, gui_renderer,
                      lsx_generation_backend, spsc_token_ring, imnodes shim
assets/               embedded TrueType fonts (Inter, JetBrains Mono)
tests/                test_lsxhome_chat, test_lsxhome_font, test_lsxhome_gui
                      (label `fast`)
```

## Chat

The main panel is a chat surface: type a question (Enter or **Send**), and the
logestix engine answers it token by token. **Stop** interrupts a running
generation, and every exchange is framed back to the model, so follow-up
questions keep their context. The model path comes from `--model`; the model
picker in the UI is still a stub. `--run [PROMPT]` submits its prompt as the
first chat turn (with no `--model` it shows the deterministic GLM-5.2
self-check token instead).

Threading: `ChatSession` owns a worker thread and a lock-free request ring; it
runs the engine and publishes decoded text into the `GuiBridge`. The UI thread
drains the bridge into a `ChatState` transcript every frame and never touches
the model. `chat_state.h` / `chat_session.h` are engine-free by construction,
which is why `test_lsxhome_chat` links Catch2 alone and runs without CUDA.

## Engine dependency

The inference engine (`lsxcommon`, and its Arrow/CUDA/abseil stack) comes from
[logestix](https://github.com/Alex7MV/logestix), pinned by a full commit SHA
(`LSXHOME_LOGESTIX_TAG`) and pulled with `FetchContent`. Logestix is built with
`LOGESTIX_BUILD_APPS=OFF`, `LOGESTIX_BUILD_TOOLS=OFF` and
`LOGESTIX_BUILD_TESTS=OFF`, so only the engine library is produced.

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

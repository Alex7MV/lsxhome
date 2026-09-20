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
src/                  main_win32, d3d12_renderer, font_loader, gui_renderer
include/lsxhome/      blackwell_theme, d3d12_renderer, font_loader,
                      gui_bridge, gui_renderer, spsc_token_ring, imnodes shim
assets/               embedded TrueType fonts (Inter, JetBrains Mono)
tests/                test_lsxhome_gui, test_lsxhome_font (label `fast`)
```

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
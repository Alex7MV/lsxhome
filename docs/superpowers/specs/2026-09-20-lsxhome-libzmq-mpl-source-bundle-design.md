# lsxhome — libzmq MPL-2.0 Source Bundle for Release Builds (Design)

- **Date:** 2026-09-20
- **Status:** Approved
- **Scope:** build system + compliance documentation

## Goal

Make every Release build of `lsxhome.exe` ship the full Source Code Form of
libzmq, so that distributing the executable satisfies the Mozilla Public
License 2.0, §3.2(a) (source availability for statically linked Covered
Software).

## Background

`lsxhome` statically links `lsxcommon` (`CMakeLists.txt:316`), and `lsxcommon`
links `libzmq-static` PUBLIC (`logestix/libs/lsxcommon/CMakeLists.txt:393`,
`libs/lsxnetwork/CMakeLists.txt:85`). libzmq is built statically and its
object code is therefore embedded in `lsxhome.exe`. libzmq is licensed under
MPL-2.0 (`logestix/third_party/libzmq/LICENSE`), a file-level copyleft that
requires, for Executable Form distributions, that the Source Code Form be made
available and that recipients be told how to obtain it.

The engine's `THIRD_PARTY_NOTICES` (shipped as `THIRD_PARTY_NOTICES.logestix`,
section "Mozilla Public License 2.0 — libzmq v4.3.5") states that libzmq
source is available from upstream at the pinned version and from the logestix
source distribution. `lsxhome` is a separate repository and currently bundles
no source, so this design closes that gap for Release artifacts.

## Scope & Placement

- Release builds only. Debug builds and test targets are unaffected.
- The full libzmq source tree is copied next to the Release binary:
  `<build>/windows-release/bin/third_party/libzmq/`.
- No CPack, no CI, no `install()` changes.

## Source Resolution

libzmq is a git submodule of logestix
(`logestix/.gitmodules` → `github.com/zeromq/libzmq`, v4.3.5) and is populated
by `FetchContent_MakeAvailable(logestix)`. After `LSXHOME_LOGESTIX_DIR` is
resolved (`CMakeLists.txt:71-75`), the top-level build defines:

```cmake
set(LSXHOME_LIBZMQ_SOURCE_DIR "${LSXHOME_LOGESTIX_DIR}/third_party/libzmq")
```

## Release Gating Mechanism

Use an external CMake script invoked with `-P`, so the configuration check and
error handling stay readable (option A of the three evaluated). After the
`THIRD_PARTY_NOTICES` copy block (`CMakeLists.txt:333`):

```cmake
add_custom_command(TARGET lsxhome POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        "-DLIBZMQ_SRC=${LSXHOME_LIBZMQ_SOURCE_DIR}"
        "-DOUT_DIR=$<TARGET_FILE_DIR:lsxhome>/third_party/libzmq"
        "-DCONFIG=$<CONFIG>"
        "-DLOGESTIX_TAG=${LSXHOME_LOGESTIX_TAG}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_libzmq_source.cmake"
    COMMENT "Bundling libzmq Source Code Form (MPL-2.0, Release only)"
)
```

The multi-config VS generator evaluates `$<CONFIG>` at build time, so the same
POST_BUILD step no-ops for Debug and only copies for Release.

## Script Behaviour: `cmake/copy_libzmq_source.cmake`

Inputs (all via `-D`): `LIBZMQ_SRC`, `OUT_DIR`, `CONFIG`, `LOGESTIX_TAG`.

1. If `CONFIG` is not `Release` → `return()` (no-op).
2. If `${LIBZMQ_SRC}/LICENSE` does not exist → `message(WARNING ...)` and
   `return()`; the build is not failed, but the missing Source Code Form is
   surfaced loudly.
3. Otherwise `file(COPY "${LIBZMQ_SRC}/" DESTINATION "${OUT_DIR}" PATTERN ".git" EXCLUDE)`.
4. Write `"${OUT_DIR}/README.lsxhome.txt"` stating: libzmq is MPL-2.0, version
   v4.3.5, the `LSXHOME_LOGESTIX_TAG` pin it came from, the upstream URL, and
   that this tree is the Source Code Form bundled to satisfy MPL-2.0 §3.2(a).

`.git` is the only pattern excluded; the whole tree (src, include, tests,
builds, doc, LICENSE, AUTHORS) is copied to avoid accidentally omitting a
covered file.

## Documentation Changes

- `THIRD_PARTY_NOTICES` intro (lines 9-12): state that Release artifacts bundle
  the libzmq Source Code Form under `third_party/libzmq/`.
- `README.md` License section: one line noting the bundled MPL-2.0 source.

## Non-Goals (YAGNI)

- No CPack / ZIP packaging.
- No CI workflow changes.
- No bundling of other transitive dependencies (Arrow, CCCL, FlatBuffers,
  simdjson, spdlog, abseil) — their notices already flow via
  `THIRD_PARTY_NOTICES.logestix`.
- No written-offer fallback document.

## Verification

- Windows Release: `cmake --build --preset windows-release`, then confirm
  `<build>/windows-release/bin/third_party/libzmq/LICENSE` exists.
- Windows Debug: confirm `third_party/libzmq/` is not created.
- Missing-source path: temporarily point `LSXHOME_LOGESTIX_DIR` at a tree
  without libzmq and confirm the warning is emitted and the build succeeds.

Linux hosts cannot run the Windows Release build; the checks above are manual
verification steps in the implementation plan.
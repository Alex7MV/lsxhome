# libzmq MPL-2.0 Release Source Bundle Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make every Release build of `lsxhome.exe` copy the full libzmq Source Code Form next to the binary, satisfying MPL-2.0 §3.2(a).

**Architecture:** A standalone CMake script (`cmake/copy_libzmq_source.cmake`) invoked from a `POST_BUILD` step on the `lsxhome` target. It no-ops unless `$<CONFIG>` is `Release`, warns (without failing) when the source tree is missing, and otherwise recursively copies the libzmq submodule tree plus a generated marker file into `<TARGET_FILE_DIR:lsxhome>/third_party/libzmq`.

**Tech Stack:** CMake 3.25+ script mode (`cmake -P`), Visual Studio multi-config generator, git submodule source from the pinned logestix dependency.

## Global Constraints

- Release only: `CONFIG != Release` must produce no output directory and exit successfully.
- Destination is exactly `$<TARGET_FILE_DIR:lsxhome>/third_party/libzmq` (i.e. `<build>/bin/third_party/libzmq`).
- Copy the whole tree; the only excluded pattern is `.git`.
- Missing source is a `message(WARNING ...)` + clean return, never a `FATAL_ERROR`.
- No CPack, no CI changes, no `install()` changes, no other dependencies bundled.
- Repo style: CMake scripts carry explanatory comments (see `cmake/embed_binary.cmake`).

---

### Task 1: Create and unit-test the copy script

**Files:**
- Create: `cmake/copy_libzmq_source.cmake`
- Test: direct `cmake -P` invocations against a `/tmp/opencode` fixture (no repo test file; the project has no CMake-script test harness)

**Interfaces:**
- Consumes: nothing (standalone script).
- Produces: a `-P` script accepting cache/`-D` variables `LIBZMQ_SRC`, `OUT_DIR`, `CONFIG`, `LOGESTIX_TAG`. Behaviour: Release-only copy; `.git` excluded; writes `${OUT_DIR}/README.lsxhome.txt`.

- [ ] **Step 1: Build the test fixture and run the script to verify it fails**

Run:
```bash
rm -rf /tmp/opencode/libzmq-test && mkdir -p /tmp/opencode/libzmq-test/src/include /tmp/opencode/libzmq-test/src/.git
printf 'Mozilla Public License Version 2.0\n' > /tmp/opencode/libzmq-test/src/LICENSE
printf '// zmq\n' > /tmp/opencode/libzmq-test/src/include/zmq.h
printf 'x\n' > /tmp/opencode/libzmq-test/src/.git/config
cmake -DLIBZMQ_SRC=/tmp/opencode/libzmq-test/src \
      -DOUT_DIR=/tmp/opencode/libzmq-test/out \
      -DCONFIG=Release -DLOGESTIX_TAG=testtag \
      -P cmake/copy_libzmq_source.cmake
```
Expected: FAIL — CMake reports it cannot find `cmake/copy_libzmq_source.cmake` (script does not exist yet).

- [ ] **Step 2: Write the script**

Create `cmake/copy_libzmq_source.cmake`:

```cmake
# copy_libzmq_source.cmake - bundle the libzmq Source Code Form next to the
# Release binary to satisfy Mozilla Public License 2.0, section 3.2(a).
#
# Invoked from a POST_BUILD step of the lsxhome target:
#   cmake -DLIBZMQ_SRC=<dir> -DOUT_DIR=<dir> -DCONFIG=$<CONFIG>
#         -DLOGESTIX_TAG=<sha> -P cmake/copy_libzmq_source.cmake
#
# Debug and any other non-Release configuration are a no-op: the compliance
# bundle is only produced for the artifacts that get distributed.

cmake_minimum_required(VERSION 3.25)

if(NOT CONFIG STREQUAL "Release")
    return()
endif()

if(NOT EXISTS "${LIBZMQ_SRC}/LICENSE")
    message(WARNING
        "lsxhome: libzmq source not found at '${LIBZMQ_SRC}' - cannot bundle "
        "the MPL-2.0 Source Code Form next to the Release binary")
    return()
endif()

# Whole tree, minus git metadata: MPL-2.0 Source Code Form must be complete,
# so no source subdirectory (src/include/tests/builds/doc) is filtered out.
file(COPY "${LIBZMQ_SRC}/" DESTINATION "${OUT_DIR}" PATTERN ".git" EXCLUDE)

file(WRITE "${OUT_DIR}/README.lsxhome.txt"
"libzmq Source Code Form
========================

libzmq is licensed under the Mozilla Public License, Version 2.0.

This directory contains the libzmq source tree as vendored by the pinned
logestix engine (logestix commit ${LOGESTIX_TAG}; libzmq v4.3.5, upstream
https://github.com/zeromq/libzmq). It is bundled next to the lsxhome Release
binary to satisfy MPL-2.0 section 3.2(a): the Source Code Form of statically
linked Covered Software is made available to every recipient of the executable.

The complete license text is in LICENSE in this directory and in the
THIRD_PARTY_NOTICES.logestix file shipped alongside the binary.
")
```

- [ ] **Step 3: Run the Release case and verify the copy**

Run:
```bash
rm -rf /tmp/opencode/libzmq-test/out
cmake -DLIBZMQ_SRC=/tmp/opencode/libzmq-test/src \
      -DOUT_DIR=/tmp/opencode/libzmq-test/out \
      -DCONFIG=Release -DLOGESTIX_TAG=testtag \
      -P cmake/copy_libzmq_source.cmake
test -f /tmp/opencode/libzmq-test/out/LICENSE
test -f /tmp/opencode/libzmq-test/out/include/zmq.h
test -f /tmp/opencode/libzmq-test/out/README.lsxhome.txt
test ! -e /tmp/opencode/libzmq-test/out/.git
echo "RELEASE OK"
```
Expected: no output from `cmake`, then `RELEASE OK`.

- [ ] **Step 4: Verify the Debug no-op and the missing-source warning**

Run:
```bash
rm -rf /tmp/opencode/libzmq-test/out-debug
cmake -DLIBZMQ_SRC=/tmp/opencode/libzmq-test/src \
      -DOUT_DIR=/tmp/opencode/libzmq-test/out-debug \
      -DCONFIG=Debug -DLOGESTIX_TAG=testtag \
      -P cmake/copy_libzmq_source.cmake
test ! -e /tmp/opencode/libzmq-test/out-debug
echo "DEBUG NOOP OK"

rm -rf /tmp/opencode/libzmq-test/out-missing
cmake -DLIBZMQ_SRC=/tmp/opencode/libzmq-test/absent \
      -DOUT_DIR=/tmp/opencode/libzmq-test/out-missing \
      -DCONFIG=Release -DLOGESTIX_TAG=testtag \
      -P cmake/copy_libzmq_source.cmake 2>&1 | grep -q "libzmq source not found"
test ! -e /tmp/opencode/libzmq-test/out-missing
echo "MISSING WARN OK"
```
Expected: `DEBUG NOOP OK` then `MISSING WARN OK`, and the build never fails.

- [ ] **Step 5: Commit**

```bash
git add cmake/copy_libzmq_source.cmake
git commit -m "build: add libzmq MPL-2.0 source-bundle script for Release"
```

---

### Task 2: Wire the script into the Release build

**Files:**
- Modify: `CMakeLists.txt` (add `LSXHOME_LIBZMQ_SOURCE_DIR` after line 75; add `POST_BUILD` after line 333)

**Interfaces:**
- Consumes: `cmake/copy_libzmq_source.cmake` from Task 1; `LSXHOME_LOGESTIX_DIR` (existing, `CMakeLists.txt:71-75`); `LSXHOME_LOGESTIX_TAG` (existing, `CMakeLists.txt:46`).
- Produces: `LSXHOME_LIBZMQ_SOURCE_DIR` cache-visible variable pointing at `<logestix>/third_party/libzmq`; a `POST_BUILD` step on target `lsxhome`.

- [ ] **Step 1: Define the libzmq source directory**

In `CMakeLists.txt`, immediately after the `if(LSXHOME_LOGESTIX_SOURCE_DIR) ... else() ... endif()` block that sets `LSXHOME_LOGESTIX_DIR` (ends at line 75), add:

```cmake
# libzmq is a logestix submodule (third_party/libzmq) and is linked statically
# into the engine; its MPL-2.0 Source Code Form is bundled with Release builds.
set(LSXHOME_LIBZMQ_SOURCE_DIR "${LSXHOME_LOGESTIX_DIR}/third_party/libzmq")
```

- [ ] **Step 2: Add the POST_BUILD step**

In `CMakeLists.txt`, immediately after the call `lsxhome_copy_third_party_notices(lsxhome)` (line 333), add:

```cmake
# MPL-2.0 section 3.2(a): ship the statically linked libzmq Source Code Form
# next to the Release binary. The script is a no-op for Debug/other configs.
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

- [ ] **Step 3: Verify the no-op host still configures**

Run:
```bash
cmake --preset linux-check
```
Expected: succeeds and prints `lsxhome: Windows-only desktop shell - nothing to build on Linux`.

- [ ] **Step 4: Syntax-check the new POST_BUILD block in isolation**

Run:
```bash
rm -rf /tmp/opencode/cmake-syntax && mkdir -p /tmp/opencode/cmake-syntax
printf 'int main(){return 0;}\n' > /tmp/opencode/cmake-syntax/main.cpp
cat > /tmp/opencode/cmake-syntax/CMakeLists.txt <<EOF
cmake_minimum_required(VERSION 3.25)
project(dummy CXX)
add_executable(dummy main.cpp)
set(LSXHOME_LIBZMQ_SOURCE_DIR "/tmp/opencode/libzmq-test/src")
set(LSXHOME_LOGESTIX_TAG "testtag")
add_custom_command(TARGET dummy POST_BUILD
    COMMAND \${CMAKE_COMMAND}
        "-DLIBZMQ_SRC=\${LSXHOME_LIBZMQ_SOURCE_DIR}"
        "-DOUT_DIR=\$<TARGET_FILE_DIR:dummy>/third_party/libzmq"
        "-DCONFIG=\$<CONFIG>"
        "-DLOGESTIX_TAG=\${LSXHOME_LOGESTIX_TAG}"
        -P "$(pwd)/cmake/copy_libzmq_source.cmake"
    COMMENT "Bundling libzmq Source Code Form (MPL-2.0, Release only)")
EOF
cmake -S /tmp/opencode/cmake-syntax -B /tmp/opencode/cmake-syntax/build
```
Expected: configure succeeds with no CMake error about the `add_custom_command` block.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt
git commit -m "build: bundle libzmq source in Release artifacts (MPL-2.0)"
```

---

### Task 3: Document the bundled source

**Files:**
- Modify: `THIRD_PARTY_NOTICES` (after the engine paragraph ending at line 12)
- Modify: `README.md` (License section, lines 55-58)

**Interfaces:**
- Consumes: the destination path `third_party/libzmq/` established in Task 2.
- Produces: nothing code-facing; documentation only.

- [ ] **Step 1: Note the bundle in THIRD_PARTY_NOTICES**

In `THIRD_PARTY_NOTICES`, after the paragraph ending `The components below are the ones introduced by this repository.` (line 12), add a blank line and:

```
Release builds additionally bundle the libzmq Source Code Form (Mozilla
Public License 2.0) next to the binary under `third_party/libzmq/`,
satisfying MPL-2.0 section 3.2(a) for the statically linked engine.
```

- [ ] **Step 2: Note the bundle in the README**

In `README.md`, append to the License section (after line 58):

```

Release builds also bundle the libzmq Source Code Form (MPL-2.0) under
`third_party/libzmq/`.
```

- [ ] **Step 3: Verify the edits**

Run:
```bash
grep -n "third_party/libzmq" THIRD_PARTY_NOTICES README.md
```
Expected: one matching line in each file.

- [ ] **Step 4: Commit**

```bash
git add THIRD_PARTY_NOTICES README.md
git commit -m "docs: note bundled libzmq MPL-2.0 source in release artifacts"
```

---

## Manual Windows Verification (deferred, not runnable on Linux hosts)

After Task 2, on a Windows host with the toolchain:

1. `cmake --preset windows-release && cmake --build --preset windows-release`
   - Expect a build step `Bundling libzmq Source Code Form (MPL-2.0, Release only)`.
   - Expect `build/windows-release/bin/third_party/libzmq/LICENSE` to exist.
2. `cmake --build --preset windows-debug`
   - Expect `build/windows-debug/bin/third_party/libzmq/` to NOT exist.

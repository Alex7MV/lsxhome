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

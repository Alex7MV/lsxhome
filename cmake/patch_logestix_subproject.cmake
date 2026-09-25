# patch_logestix_subproject.cmake — make the pinned logestix engine consumable
# by lsxhome as a CMake SUBPROJECT on Windows.
#
# The engine is authored as the top-level project: it resolves its bundled
# submodules (CCCL, simdjson, libzmq) and its own notice/script files through
# ${CMAKE_SOURCE_DIR}. When lsxhome pulls it in via FetchContent/add_subdirectory,
# ${CMAKE_SOURCE_DIR} is the lsxhome root, so those paths break (e.g. the libzmq
# version probe cannot read third_party/libzmq/include/zmq.h).
#
# Three CUDA backends additionally carry POSIX-only calls that MSVC/NVCC does
# not provide and that are not guarded: M_PI (DeepSeek-V4.1 host reference),
# setenv (Kimi diagnostic dump) and munmap (expert_store chunk release).
#
# Patch the vendored tree in place, idempotently — the same after-fetch pattern
# already used for ImNodes (TexRef) and FreeType (subpixel rendering). String
# replacement is stable because the engine is pinned to an immutable SHA.

function(lsxhome_replace_in_file path old new)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "patch_logestix_subproject: missing file ${path}")
    endif()
    file(READ "${path}" _content)
    string(REPLACE "${old}" "${new}" _content "${_content}")
    file(WRITE "${path}" "${_content}")
endfunction()

function(lsxhome_patch_logestix_subproject root)
    if(NOT EXISTS "${root}/CMakeLists.txt")
        message(FATAL_ERROR "patch_logestix_subproject: ${root} is not a logestix checkout")
    endif()

    # 1. Engine root: every ${CMAKE_SOURCE_DIR} there denotes the engine root.
    lsxhome_replace_in_file("${root}/CMakeLists.txt"
        "\${CMAKE_SOURCE_DIR}" "\${CMAKE_CURRENT_SOURCE_DIR}")

    # 2. lsxcommon sits two directories below the root, so its bundled-submodule
    #    include paths must climb back up.
    lsxhome_replace_in_file("${root}/libs/lsxcommon/CMakeLists.txt"
        "\${CMAKE_SOURCE_DIR}/third_party/"
        "\${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/")

    # 3. DeepSeek-V4.1 host reference: MSVC's <cmath> does not define M_PI.
    lsxhome_replace_in_file(
        "${root}/libs/lsxcommon/src/deepseek_moe_v41_device_math.h"
        "#include <algorithm>\n#include <cmath>\n#include <cstdint>"
        "#include <algorithm>\n#include <cmath>\n#ifndef M_PI\n#define M_PI 3.14159265358979323846\n#endif\n#include <cstdint>")

    # 4. Kimi diagnostic dump: setenv is POSIX-only (MSVC has _putenv_s).
    lsxhome_replace_in_file("${root}/libs/lsxcommon/src/kimi_moe_inference.cu"
        "setenv(\"LSX_KIMI_DUMP_DONE\", \"1\", 1);"
        "#ifdef _WIN32\n                            _putenv_s(\"LSX_KIMI_DUMP_DONE\", \"1\");\n#else\n                            setenv(\"LSX_KIMI_DUMP_DONE\", \"1\", 1);\n#endif")

    # 5. expert_store: the PinChunked mmap path is already inside a POSIX guard,
    #    but Free()'s release loop referenced munmap unconditionally. Guard it;
    #    PinChunked returns false on Windows, so pinned_pools stays empty and
    #    the caller falls back to Arrow mmap views.
    lsxhome_replace_in_file("${root}/libs/lsxcommon/include/lsxcommon/expert_store.h"
        "            if (reg) cudaHostUnregister(p);\n            if (bytes > 0) munmap(p, bytes);"
        "            if (reg) cudaHostUnregister(p);\n#if defined(__unix__) || defined(__APPLE__)\n            if (bytes > 0) munmap(p, bytes);\n#endif")

    # 6. CUDA 13.4 NVVM regression: in a Debug build (/MDd defines _DEBUG) the
    #    combination `--expt-relaxed-constexpr` + `_DEBUG` makes device codegen
    #    abort with "Invalid instruction with no BB" (LLVM 23) in some TUs
    #    (perf_calibration.cu, gigachat_moe_inference_pipeline.cu, ...).
    #    The flag only *permits* more relaxed constexpr, it does not change the
    #    meaning of code that already compiles, so drop it for Debug only.
    #    The search key includes the neighbouring `--extended-lambda;` so the
    #    patch is idempotent (the rewritten text no longer contains the key).
    lsxhome_replace_in_file("${root}/libs/lsxcommon/CMakeLists.txt"
        "--extended-lambda;--expt-relaxed-constexpr"
        "--extended-lambda;\$<\$<NOT:\$<CONFIG:Debug>>:--expt-relaxed-constexpr>")
endfunction()

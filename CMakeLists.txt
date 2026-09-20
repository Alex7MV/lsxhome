cmake_minimum_required(VERSION 3.25)

# lsxhome — LogestiX Home, Windows-native x64 desktop shell.
#
# Standalone repository. The inference engine (`lsxcommon`) is provided by a
# pinned `logestix` checkout, pulled with FetchContent (or supplied locally via
# the LSXHOME_LOGESTIX_SOURCE_DIR cache variable). Windows-only: on any other
# host the whole build is a no-op, so the repository configures cleanly
# everywhere (CI, Linux development hosts).
#
# Hardware-accelerated GUI (D3D12 immediate-mode stack) consuming the lock-free
# SPSC token bridge from the GPU compute thread.
#
# GUI stack (third-party, vendored as SEPARATE static archives — MIT):
#   Dear ImGui  (docking branch)  — core + Win32 platform + D3D12 renderer
#   ImPlot                        — real-time charting channels
#   ImNodes                       — graph-layout node editor
#   FreeType 2.14.3               — imgui_freetype rasterizer, LCD subpixel AA

# On non-Windows hosts the shell is not built at all — the tree configures as a
# no-op so CI and Linux development hosts do not need a CUDA toolkit.
if(NOT WIN32)
    project(lsxhome VERSION 0.1.0 LANGUAGES C CXX)
    message(STATUS "lsxhome: Windows-only desktop shell — nothing to build on ${CMAKE_SYSTEM_NAME}")
    return()
endif()

# CUDA must be enabled at the TOP level: under CMake 4.x a nested directory
# being the first to enable CUDA breaks. The engine's `project()` therefore
# finds the language already enabled when it is added below.
project(lsxhome VERSION 0.1.0 LANGUAGES C CXX CUDA)
find_package(CUDAToolkit REQUIRED)

# Arrow's bundled xsimd predates CMake 4.x policies; the nested sub-project must
# be allowed to declare an old minimum (same guard as the engine's presets).
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "Allow pre-4.x nested projects" FORCE)

# Binaries land in bin/ (the POST_BUILD arrow.dll copies target this dir).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

include(FetchContent)

# ── Engine: pinned logestix checkout ─────────────────────────────────────────
# The pin is a full commit SHA (logestix has no release tags). GIT_SHALLOW is
# FALSE because a raw SHA cannot be fetched shallowly.
set(LSXHOME_LOGESTIX_TAG "413b44e12eef959c81682ea702ae1298860fabf7" CACHE STRING "logestix commit SHA the engine is pinned to")
set(LSXHOME_LOGESTIX_SOURCE_DIR "" CACHE PATH "Local logestix checkout (development override)")

# Build only the engine library from logestix — never its sibling applications,
# offline tools or the engine test suite.
set(LOGESTIX_BUILD_APPS OFF CACHE BOOL "" FORCE)
set(LOGESTIX_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(LOGESTIX_BUILD_TESTS OFF CACHE BOOL "" FORCE)

if(LSXHOME_LOGESTIX_SOURCE_DIR)
    message(STATUS "lsxhome: using local logestix checkout at ${LSXHOME_LOGESTIX_SOURCE_DIR}")
    add_subdirectory("${LSXHOME_LOGESTIX_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/logestix-local")
else()
    FetchContent_Declare(
        logestix
        GIT_REPOSITORY "https://github.com/Alex7MV/logestix.git"
        GIT_TAG        "${LSXHOME_LOGESTIX_TAG}"
        GIT_SHALLOW    FALSE
    )
    FetchContent_MakeAvailable(logestix)
endif()

# Resolve the engine source directory so its own THIRD_PARTY_NOTICES can be
# shipped next to the binary (the statically linked engine brings Arrow, CCCL,
# FlatBuffers, libzmq, simdjson, spdlog and abseil-cpp).
if(LSXHOME_LOGESTIX_SOURCE_DIR)
    set(LSXHOME_LOGESTIX_DIR "${LSXHOME_LOGESTIX_SOURCE_DIR}")
else()
    set(LSXHOME_LOGESTIX_DIR "${logestix_SOURCE_DIR}")
endif()

# ── Dear ImGui docking branch (D3D12 + FreeType subpixel) ───────────────────
set(LSXHOME_IMGUI_TAG v1.92.9-docking CACHE STRING "Dear ImGui docking-branch tag")
FetchContent_Declare(
    dear-imgui
    GIT_REPOSITORY "https://github.com/ocornut/imgui.git"
    GIT_TAG        "${LSXHOME_IMGUI_TAG}"
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(dear-imgui)
if(NOT dear-imgui_POPULATED)
    FetchContent_Populate(dear-imgui)
endif()

# ── ImPlot (real-time charting channels) ────────────────────────────────────
set(LSXHOME_IMPLOT_TAG v1.0 CACHE STRING "ImPlot tag")
FetchContent_Declare(
    implot
    GIT_REPOSITORY "https://github.com/epezent/implot.git"
    GIT_TAG        "${LSXHOME_IMPLOT_TAG}"
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(implot)
if(NOT implot_POPULATED)
    FetchContent_Populate(implot)
endif()

# ── ImNodes (graph-layout node editor) ──────────────────────────────────────
set(LSXHOME_IMNODES_TAG v0.5 CACHE STRING "ImNodes tag")
FetchContent_Declare(
    imnodes
    GIT_REPOSITORY "https://github.com/Nelarius/imnodes.git"
    GIT_TAG        "${LSXHOME_IMNODES_TAG}"
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(imnodes)
if(NOT imnodes_POPULATED)
    FetchContent_Populate(imnodes)
endif()

# ImNodes v0.5 predates Dear ImGui 1.92's texture-refactoring: ImDrawCmd::TextureId
# became ImTextureRef TexRef and ImDrawList::_TextureIdStack became _TextureStack.
# Patch the vendored source (same pattern as the Arrow/Boost patches) so the
# node editor compiles against the docking imgui.1.92.9.
set(_imnodes_cpp "${imnodes_SOURCE_DIR}/imnodes.cpp")
if(EXISTS "${_imnodes_cpp}")
    file(READ "${_imnodes_cpp}" _imnodes_content)
    string(REPLACE
        "draw_cmd.TextureId = draw_list->_TextureIdStack.back();"
        "draw_cmd.TexRef = draw_list->_TextureStack.back();"
        _imnodes_content "${_imnodes_content}")
    file(WRITE "${_imnodes_cpp}" "${_imnodes_content}")
    unset(_imnodes_content)
endif()
unset(_imnodes_cpp)

# ── FreeType (imgui_freetype abstraction layer, LCD subpixel AA) ────────────
set(LSXHOME_FREETYPE_TAG VER-2-14-3 CACHE STRING "FreeType tag")
FetchContent_Declare(
    freetype
    GIT_REPOSITORY "https://gitlab.freedesktop.org/freetype/freetype.git"
    GIT_TAG        "${LSXHOME_FREETYPE_TAG}"
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(freetype)
if(NOT freetype_POPULATED)
    FetchContent_Populate(freetype)
endif()

# FreeType ships FT_CONFIG_OPTION_SUBPIXEL_RENDERING commented out inside
# ftoption.h (was disabled for patents). imgui_freetype baboon-fed subpixel
# glyphs need pixel-LCD rendering, so unmute the define — the same patch-
# after-fetch pattern as the Arrow/Boost patch in the engine.
set(FT_CONFIG_OPTION_SUBPIXEL_FILE "${freetype_SOURCE_DIR}/include/freetype/config/ftoption.h")
if(EXISTS "${FT_CONFIG_OPTION_SUBPIXEL_FILE}")
    file(READ "${FT_CONFIG_OPTION_SUBPIXEL_FILE}" FT_OPTION_CONTENT)
    string(REPLACE
        "/* #define FT_CONFIG_OPTION_SUBPIXEL_RENDERING */"
        "#define FT_CONFIG_OPTION_SUBPIXEL_RENDERING"
        FT_OPTION_CONTENT "${FT_OPTION_CONTENT}")
    file(WRITE "${FT_CONFIG_OPTION_SUBPIXEL_FILE}" "${FT_OPTION_CONTENT}")
    unset(FT_OPTION_CONTENT)
endif()
unset(FT_CONFIG_OPTION_SUBPIXEL_FILE)

# Freetype targets must be static and must not pull system codec discovery
# (zlib/harfbuzz/brotli/png) — the GUI rasterizer only needs TrueType+LCD.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "lsxhome: freetype static" FORCE)
set(FT_DISABLE_ZLIB ON CACHE BOOL "lsxhome: no zlib in freetype" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "lsxhome: no bzip2 in freetype" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "lsxhome: no brotli in freetype" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "lsxhome: no harfbuzz in freetype" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "lsxhome: no libpng in freetype" FORCE)
add_subdirectory(${freetype_SOURCE_DIR} ${freetype_BINARY_DIR} EXCLUDE_FROM_ALL)

# ── Dear ImGui static archive (docking headers + D3D12 + Win32) ─────────────
add_library(lsxhome_imgui STATIC
    ${dear-imgui_SOURCE_DIR}/imgui.cpp
    ${dear-imgui_SOURCE_DIR}/imgui_draw.cpp
    ${dear-imgui_SOURCE_DIR}/imgui_tables.cpp
    ${dear-imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${dear-imgui_SOURCE_DIR}/imgui_demo.cpp
    ${dear-imgui_SOURCE_DIR}/misc/freetype/imgui_freetype.cpp
    ${dear-imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${dear-imgui_SOURCE_DIR}/backends/imgui_impl_dx12.cpp
)
target_include_directories(lsxhome_imgui PUBLIC
    $<BUILD_INTERFACE:${dear-imgui_SOURCE_DIR}>
    $<BUILD_INTERFACE:${dear-imgui_SOURCE_DIR}/backends>
    $<BUILD_INTERFACE:${dear-imgui_SOURCE_DIR}/misc/freetype>
)
# imgui_freetype must see the compiled-in FreeType subpixel rasterizer, and
# imgui.cpp must default to the FreeType loader (imconfig-level IMGUI_ENABLE_FREETYPE).
# IMGUI_DEFINE_MATH_OPERATORS is the docking-branch contract for ImNodes/ImPlot
# (they need ImVec2/ImVec4 operators) and must be visible to every TU that
# includes imgui.h, hence PUBLIC.
target_compile_definitions(lsxhome_imgui PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    IMGUI_ENABLE_FREETYPE
)
# NOTE: IMGUI_DISABLE_OBSOLETE_FUNCTIONS is intentionally NOT used. It guards
# layout-affecting members of ImGuiIO/ImGuiStyle (imgui.h), so it MUST be
# identical across every TU that includes imgui.h AND calls IMGUI_CHECKVERSION().
# Because imnodes/implot still call obsolete ImDrawList APIs, enabling it only
# inside imgui.cpp would desync sizeof(ImGuiIO) vs. the app TUs and trip the
# "Mismatched struct layout!" debug assert at startup.
target_compile_definitions(lsxhome_imgui PUBLIC
    IMGUI_DEFINE_MATH_OPERATORS
)
# Execution-subsystem layers forced natively: D3D12 + Win32 graphics stack.
# /MACHINE:X64 is a LINKER option only — the x64 machine block is applied to
# the final exe below; feeding it to cl.exe would warn D9002.
target_link_libraries(lsxhome_imgui PUBLIC freetype)
if(MSVC)
    target_link_options(lsxhome_imgui PUBLIC /MACHINE:X64)
endif()

# ── ImPlot static (real-time charting channels) ─────────────────────────────
add_library(lsxhome_implot STATIC
    ${implot_SOURCE_DIR}/implot.cpp
    ${implot_SOURCE_DIR}/implot_items.cpp
)
target_include_directories(lsxhome_implot PUBLIC
    $<BUILD_INTERFACE:${implot_SOURCE_DIR}>
)
target_link_libraries(lsxhome_implot PUBLIC lsxhome_imgui)
if(MSVC)
    target_link_options(lsxhome_implot PUBLIC /MACHINE:X64)
endif()

# ── ImNodes static (graph-layout node editor) ───────────────────────────────
add_library(lsxhome_imnodes STATIC
    ${imnodes_SOURCE_DIR}/imnodes.cpp
)
target_include_directories(lsxhome_imnodes PUBLIC
    $<BUILD_INTERFACE:${imnodes_SOURCE_DIR}>
)
# ImNodes v0.5 still uses IM_OFFSETOF, which Dear ImGui obsoleted in 1.90
# (removed from imgui.h). Force-include a shim that restores the std::offsetof
# classic for the vendored imnodes TU only. A /D define is NOT viable here:
# cl.exe truncates function-like macro definitions at the comma, breaking
# <type_traits>. /FI is the MSVC-native, precise mechanism.
if(MSVC)
    target_compile_options(lsxhome_imnodes PRIVATE
        "/FI${CMAKE_CURRENT_SOURCE_DIR}/include/lsxhome/imnodes_offsetof_shim.h"
    )
endif()
target_link_libraries(lsxhome_imnodes PUBLIC lsxhome_imgui)
if(MSVC)
    target_link_options(lsxhome_imnodes PUBLIC /MACHINE:X64)
endif()

# ── TrueType font binary payload → static execution memory ──────────────────
# The resource loader ingests the TTF byte vector straight into the pre-allocated
# static memory space via a CMake embed pass (byte array in .rdata). Default font:
# Inter is freely redistributable; drop a TTF into lsxhome/assets/ and reconfigure.
set(LSXHOME_FONT_TTF "${CMAKE_CURRENT_SOURCE_DIR}/assets/Inter-Regular.ttf" CACHE FILEPATH
    "TrueType font vector embedded into the lsxhome static execution memory")
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_binary.cmake)
lsx_embed_binary("${LSXHOME_FONT_TTF}"
    lsxhome_font_payload
    "${CMAKE_CURRENT_BINARY_DIR}/lsxhome_font_payload.cpp")
add_library(lsxhome_font STATIC "${CMAKE_CURRENT_BINARY_DIR}/lsxhome_font_payload.cpp")
target_include_directories(lsxhome_font PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>)
if(MSVC)
    target_link_options(lsxhome_font PUBLIC /MACHINE:X64)
endif()

# ── JetBrains Mono (monospace telemetry font) → static execution memory ──────
# Introduced under TDD (tests/test_lsxhome_font.cpp): the RED build failed with
# C1083 (missing lsxhome_mono_payload.h); this target is the minimal GREEN that
# bakes the JetBrains Mono TrueType vector into its own static archive.
set(LSXHOME_MONO_TTF "${CMAKE_CURRENT_SOURCE_DIR}/assets/JetBrainsMono-Regular.ttf" CACHE FILEPATH
    "JetBrains Mono TrueType vector embedded into the lsxhome static execution memory")
lsx_embed_binary("${LSXHOME_MONO_TTF}"
    lsxhome_mono_payload
    "${CMAKE_CURRENT_BINARY_DIR}/lsxhome_mono_payload.cpp")
add_library(lsxhome_mono STATIC "${CMAKE_CURRENT_BINARY_DIR}/lsxhome_mono_payload.cpp")
target_include_directories(lsxhome_mono PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>)
if(MSVC)
    target_link_options(lsxhome_mono PUBLIC /MACHINE:X64)
endif()

# THIRD_PARTY_NOTICES must accompany every distributed binary. Defined here (not
# inherited from the engine, whose function is scoped to its own directory tree).
function(lsxhome_copy_third_party_notices target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES"
            "$<TARGET_FILE_DIR:${target}>/THIRD_PARTY_NOTICES"
        COMMENT "Copy THIRD_PARTY_NOTICES next to ${target}"
    )
    if(EXISTS "${LSXHOME_LOGESTIX_DIR}/THIRD_PARTY_NOTICES")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${LSXHOME_LOGESTIX_DIR}/THIRD_PARTY_NOTICES"
                "$<TARGET_FILE_DIR:${target}>/THIRD_PARTY_NOTICES.logestix"
            COMMENT "Copy the engine THIRD_PARTY_NOTICES next to ${target}"
        )
    endif()
endfunction()

# ── lsxhome executable (WIN32 subsystem, no console) ────────────────────────
add_executable(lsxhome WIN32
    src/main_win32.cpp
    src/d3d12_renderer.cpp
    src/font_loader.cpp
    src/gui_renderer.cpp
)
target_include_directories(lsxhome PRIVATE include)
target_compile_definitions(lsxhome PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(lsxhome PRIVATE
    lsxhome_imgui
    lsxhome_implot
    lsxhome_imnodes
    lsxhome_font
    lsxhome_mono
    lsxcommon
    arrow_shared
    arrow_cuda_shared
    absl::flat_hash_map
    absl::span
    absl::strings
    absl::flags_parse
    absl::flags
    d3d12
    dxgi
    d3dcompiler
    dwmapi
    user32
    gdi32
    shell32
)

lsxhome_copy_third_party_notices(lsxhome)

# Full x64 machine optimization block: /MACHINE:X64 emits the PE32+ image,
# /OPT:REF folds dead code, /OPT:ICF merges identical COMDATs (cross-TU,
# characteristically at high value for the IMGUI static stack).
if(MSVC)
    target_link_options(lsxhome PRIVATE /MACHINE:X64 /OPT:REF /OPT:ICF=5)
endif()

# arrow.dll / arrow_cuda.dll next to the exe (same POST_BUILD pattern as the
# engine applications).
add_custom_command(TARGET lsxhome POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:arrow_shared>"
        "$<TARGET_FILE_DIR:lsxhome>"
)
add_custom_command(TARGET lsxhome POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:arrow_cuda_shared>"
        "$<TARGET_FILE_DIR:lsxhome>"
)

install(TARGETS lsxhome RUNTIME DESTINATION bin)
install(FILES "${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES" DESTINATION bin)
install(FILES "${LSXHOME_LOGESTIX_DIR}/THIRD_PARTY_NOTICES"
        DESTINATION bin RENAME THIRD_PARTY_NOTICES.logestix)

if(BUILD_TESTING)
    enable_testing()
    set(CATCH_BUILD_TESTING OFF CACHE INTERNAL "")
    set(CATCH_BUILD_EXAMPLES OFF CACHE INTERNAL "")
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY "https://github.com/catchorg/Catch2.git"
        GIT_TAG        "v3.16.0"
    )
    FetchContent_MakeAvailable(Catch2)
    add_subdirectory(tests)
endif()

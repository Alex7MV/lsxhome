# patch_vendored_sources.cmake — after-fetch patching helper for the third-party
# sources lsxhome consumes through FetchContent.
#
# Vendored dependencies are patched in place right after they are fetched,
# because lsxhome needs behaviour the upstream release does not provide:
#
#   * ImNodes v0.5 predates Dear ImGui 1.92's texture refactoring
#     (ImDrawCmd::TextureId became ImTextureRef TexRef), so the node editor
#     would not compile against the pinned docking ImGui.
#   * FreeType ships FT_CONFIG_OPTION_SUBPIXEL_RENDERING commented out, but
#     imgui_freetype needs pixel-LCD rendering for LCD glyphs.
#
# The logestix engine needs exactly one patch (CUDA 13.4, below). Everything
# else lsxhome once patched there is fixed upstream: the `${CMAKE_SOURCE_DIR}/
# third_party` paths became `PROJECT_SOURCE_DIR`, `M_PI` became
# `std::numbers::pi`, `expert_store.h` guards `munmap` itself, and
# `kimi_moe_inference.cu` guards `setenv` since bbf6771. A patch for a problem
# the dependency already fixed is not harmless dead weight — its search key
# usually survives inside an upstream `#else` branch, so the rewrite re-wraps the
# dependency's own fix on every configure (the Kimi patch did exactly that,
# five nested `#ifdef` layers deep). Verify a patch's necessity against
# `git show <pin>:<file>` on the pristine checkout first.
#
# The engine pins to an immutable commit SHA, so these rewrites are stable.

# Rewrites `old` to `new` inside `path`, exactly once, or not at all.
#
# Three outcomes, all deliberate:
#
#   * key present, applied-marker absent -> rewrite (first configure).
#   * key present, applied-marker present -> already applied; return, so the
#     rewrite never re-wraps its own output. The marker only counts as proof
#     when it is independent of the key: FreeType ships its define commented
#     out, so the key "/* #define X */" *contains* the replacement "#define X",
#     and probing for the replacement there would match the untouched upstream
#     line and skip the patch forever.
#   * key absent -> either the rewrite already landed (the vendored tree
#     persists, so every patch runs again on the next configure) or upstream
#     drifted. The replacement being present tells those apart. When it is not,
#     stop the configure: a silent no-op would surface much later as an
#     unrelated compiler error with nothing pointing back at the patch.
#
# `marker` is an optional fourth argument for replacements that still contain
# their own search key; it defaults to `new`.
function(lsxhome_replace_in_file path old new)
    set(marker "${new}")
    if(ARGC GREATER 3)
        set(marker "${ARGV3}")
    endif()
    set(marker_is_independent FALSE)
    string(FIND "${old}" "${marker}" _marker_in_key)
    if(_marker_in_key EQUAL -1)
        set(marker_is_independent TRUE)
    endif()

    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "lsxhome_replace_in_file: missing file ${path}")
    endif()
    file(READ "${path}" _content)
    string(FIND "${_content}" "${old}" _old_pos)
    if(_old_pos EQUAL -1)
        string(FIND "${_content}" "${new}" _new_pos)
        if(NOT _new_pos EQUAL -1)
            return()
        endif()
        message(FATAL_ERROR
            "lsxhome_replace_in_file: patch key not found in ${path}.\n"
            "The pinned dependency moved the patched line; update this patch "
            "before bumping the pin.\n  key: ${old}")
    endif()
    string(FIND "${_content}" "${marker}" _marker_pos)
    if(NOT _marker_pos EQUAL -1 AND marker_is_independent)
        return()  # already applied: never re-wrap our own replacement
    endif()
    string(REPLACE "${old}" "${new}" _content "${_content}")
    file(WRITE "${path}" "${_content}")
endfunction()

# The one patch the engine still needs, applied before its CMakeLists runs.
function(lsxhome_patch_logestix root)
    if(NOT EXISTS "${root}/CMakeLists.txt")
        message(FATAL_ERROR "lsxhome_patch_logestix: ${root} is not a logestix checkout")
    endif()

    # CUDA 13.4 NVVM regression. In a Debug build (/MDd implies _DEBUG) the
    # combination `--expt-relaxed-constexpr` + `_DEBUG` makes device codegen
    # abort with
    #     <unnamed>: parse Invalid instruction with no BB
    #                (Producer: 'LLVM23.0.0' Reader: 'LLVM 23.0.0')
    # in perf_calibration.cu and gigachat_moe_inference_pipeline.cu. The engine
    # passes the flag to its own object libraries with no Debug guard.
    # The flag only *permits* more relaxed constexpr — it does not change the
    # meaning of code that already compiles — so dropping it for Debug is safe.
    # The search key includes the neighbouring `--extended-lambda;` so the
    # replacement no longer contains the key.
    # Reproduced on CUDA 13.4.92 / LLVM 23 by building the engine in Debug with
    # this patch disabled.
    lsxhome_replace_in_file("${root}/libs/lsxcommon/CMakeLists.txt"
        "--extended-lambda;--expt-relaxed-constexpr"
        "--extended-lambda;\$<\$<NOT:\$<CONFIG:Debug>>:--expt-relaxed-constexpr>")
endfunction()
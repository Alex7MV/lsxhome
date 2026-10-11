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
# The logestix engine no longer needs any patch: it carries its own Debug guard
# around `--expt-relaxed-constexpr` and its KV-cache geometry helpers are
# `__host__ __device__` (f99008b), so a Debug build compiles without the flag.
# Everything lsxhome once patched there is now fixed upstream: the
# `${CMAKE_SOURCE_DIR}/third_party` paths became `PROJECT_SOURCE_DIR`, `M_PI`
# became `std::numbers::pi`, `expert_store.h` guards `munmap` itself, and
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
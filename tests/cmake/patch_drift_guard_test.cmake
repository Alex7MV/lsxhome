# Verifies the after-fetch patch helper refuses to run silently when the pinned
# upstream text has drifted, and stays idempotent when it does not.
#
# lsxhome rewrites third-party sources in place right after FetchContent. If a
# future dependency bump moves the patched line, a plain string(REPLACE) is a
# no-op: the build then fails much later with an unrelated compiler error and
# nothing points back at the patch. The helper must stop the configure instead.
#
# Every case runs through patch_replace_probe.cmake in a sub-process so the exit
# code is observable from here.

set(module "${CMAKE_CURRENT_LIST_DIR}/../../cmake/patch_vendored_sources.cmake")
set(probe "${CMAKE_CURRENT_LIST_DIR}/patch_replace_probe.cmake")
set(fixture "${CMAKE_CURRENT_BINARY_DIR}/patch_drift_fixture.txt")

function(lsxhome_test_run_probe key value marker out_rc out_text)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DLSXHOME_PATCH_MODULE=${module}"
            "-DPATCH_FIXTURE=${fixture}"
            "-DPATCH_KEY=${key}"
            "-DPATCH_VALUE=${value}"
            "-DPATCH_MARKER=${marker}"
            -P "${probe}"
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err
    )
    set(${out_rc} "${rc}" PARENT_SCOPE)
    set(${out_text} "${out}${err}" PARENT_SCOPE)
endfunction()

# ── A live key must be rewritten ──────────────────────────────────────────────
file(WRITE "${fixture}" "alpha\nbeta\n")
lsxhome_test_run_probe("beta" "BETA" "" rc text)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "a present patch key should be rewritten, rc=${rc}: ${text}")
endif()
file(READ "${fixture}" patched)
if(NOT patched MATCHES "BETA")
    message(FATAL_ERROR "replacement did not land: ${patched}")
endif()
if(patched MATCHES "beta")
    message(FATAL_ERROR "original text survived the replacement: ${patched}")
endif()

# ── A drifted key must abort, naming the patch helper ─────────────────────────
file(WRITE "${fixture}" "alpha\nbeta\n")
lsxhome_test_run_probe("gamma" "delta" "" rc text)
if(rc EQUAL 0)
    message(FATAL_ERROR
        "a drifted patch key was accepted silently (rc=0); the after-fetch "
        "patches will skip their rewrite and fail later with an unrelated error")
endif()
if(NOT text MATCHES "lsxhome_replace_in_file")
    message(FATAL_ERROR "the abort did not name the patch helper: ${text}")
endif()

# ── An already-applied patch must not abort on the next configure ────────────
# The vendored tree persists across configures, so every patch runs again the
# second time around. Once the rewrite has landed the original key is gone; the
# helper must read that as "already applied", not as drift.
file(WRITE "${fixture}" "alpha\nBETA\n")
lsxhome_test_run_probe("beta" "BETA" "" rc text)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR
        "an already-applied patch must stay idempotent, rc=${rc}: ${text}")
endif()

# ── A replacement containing its own key must not nest ───────────────────────
# The Kimi dump patch wraps the call in #ifdef/#else, so its own #else branch
# still contains the search key. Without an explicit applied-marker the helper
# matches that branch on the next configure and wraps it again — one more layer
# of #ifdef per configure, forever.
set(kimi_key "setenv(\"LSX_KIMI_DUMP_DONE\", \"1\", 1);")
set(kimi_new "#ifdef _WIN32\n    _putenv_s(\"LSX_KIMI_DUMP_DONE\", \"1\");\n#else\n    ${kimi_key}\n#endif")
set(kimi_marker "_putenv_s(\"LSX_KIMI_DUMP_DONE\", \"1\");")

file(WRITE "${fixture}" "    ${kimi_key}\n")
lsxhome_test_run_probe("${kimi_key}" "${kimi_new}" "${kimi_marker}" rc text)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "the wrapper patch should apply, rc=${rc}: ${text}")
endif()

# Second configure against the already-patched tree.
lsxhome_test_run_probe("${kimi_key}" "${kimi_new}" "${kimi_marker}" rc text)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR
        "re-applying the wrapper patch must stay idempotent, rc=${rc}: ${text}")
endif()

file(READ "${fixture}" wrapped)
string(REGEX MATCHALL "#ifdef _WIN32" wrapper_layers "${wrapped}")
list(LENGTH wrapper_layers layer_count)
if(NOT layer_count EQUAL 1)
    message(FATAL_ERROR
        "the wrapper patch nested ${layer_count} #ifdef layers; it must apply once")
endif()

# ── A replacement that is a substring of its own key must still apply ────────
# FreeType ships the define commented out, so the search key
# "/* #define X */" *contains* the replacement "#define X". A naive
# "already applied?" probe on the replacement therefore matches the untouched
# upstream line and skips the patch entirely — FreeType would silently keep
# subpixel rendering disabled.
set(ft_key "/* #define FT_SUBPIXEL_PROBE */")
set(ft_new "#define FT_SUBPIXEL_PROBE")
file(WRITE "${fixture}" "${ft_key}\n")
lsxhome_test_run_probe("${ft_key}" "${ft_new}" "" rc text)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "the uncomment patch should apply, rc=${rc}: ${text}")
endif()
file(READ "${fixture}" ft_patched)
string(STRIP "${ft_patched}" ft_stripped)
if(NOT ft_stripped STREQUAL ft_new)
    message(FATAL_ERROR
        "the uncomment patch did not land (replacement is a substring of its "
        "key): ${ft_patched}")
endif()

# And the second configure must recognise the landed patch.
lsxhome_test_run_probe("${ft_key}" "${ft_new}" "" rc text)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR
        "re-applying the uncomment patch must stay idempotent, rc=${rc}: ${text}")
endif()
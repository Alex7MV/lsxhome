# Applies exactly one patch to a scratch fixture and nothing else.
#
# Invoked as a sub-process by patch_drift_guard_test.cmake so the caller can
# observe whether the helper returned normally or refused:
#   exit 0  - the key was found and rewritten (or was already applied)
#   exit !0 - the helper aborted (upstream drift)
#
# Required cache entries: LSXHOME_PATCH_MODULE, PATCH_FIXTURE, PATCH_KEY,
# PATCH_VALUE. Optional: PATCH_MARKER - text whose presence means "this patch is
# already applied", for replacements that still contain their own search key.

if(NOT LSXHOME_PATCH_MODULE OR NOT PATCH_FIXTURE OR NOT PATCH_KEY OR NOT PATCH_VALUE)
    message(FATAL_ERROR "probe requires LSXHOME_PATCH_MODULE, PATCH_FIXTURE, PATCH_KEY, PATCH_VALUE")
endif()

include("${LSXHOME_PATCH_MODULE}")

if(PATCH_MARKER)
    lsxhome_replace_in_file("${PATCH_FIXTURE}" "${PATCH_KEY}" "${PATCH_VALUE}" "${PATCH_MARKER}")
else()
    lsxhome_replace_in_file("${PATCH_FIXTURE}" "${PATCH_KEY}" "${PATCH_VALUE}")
endif()
add_executable(test_lsxhome_gui test_lsxhome_gui.cpp)
target_include_directories(test_lsxhome_gui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(test_lsxhome_gui PRIVATE lsxcommon Catch2::Catch2WithMain)

add_executable(test_lsxhome_font test_lsxhome_font.cpp)
target_include_directories(test_lsxhome_font PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
    ${CMAKE_CURRENT_BINARY_DIR}/..
)
target_link_libraries(test_lsxhome_font PRIVATE lsxhome_mono Catch2::Catch2WithMain)

set(LSXHOME_TEST_TARGETS test_lsxhome_gui test_lsxhome_font)

foreach(test_target IN LISTS LSXHOME_TEST_TARGETS)
    add_custom_command(TARGET ${test_target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:arrow_shared>"
            "$<TARGET_FILE_DIR:${test_target}>"
    )
    add_custom_command(TARGET ${test_target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:arrow_cuda_shared>"
            "$<TARGET_FILE_DIR:${test_target}>"
    )
endforeach()

# Literal add_test calls: the engine's test_ctest_labels_guard statically
# resolves names in the standalone monorepo; keep the same discipline here.
add_test(NAME test_lsxhome_gui COMMAND test_lsxhome_gui)
add_test(NAME test_lsxhome_font COMMAND test_lsxhome_font)
set_tests_properties(test_lsxhome_gui test_lsxhome_font PROPERTIES LABELS fast)

# ── Build-system tests ───────────────────────────────────────────────────────
# The after-fetch patches rewrite pinned third-party sources in place. When a
# dependency bump moves the patched line, a silent no-op turns into a confusing
# compile error far from the patch, so the helper refuses to continue.
add_test(NAME patch_drift_guard
    COMMAND ${CMAKE_COMMAND}
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_drift_guard_test.cmake")
set_tests_properties(patch_drift_guard PROPERTIES LABELS fast)

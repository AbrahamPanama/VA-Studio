# SPDX-License-Identifier: GPL-2.0-or-later
#
# External-process clipboard paste harness (milestone A, E-series).
#
# Roles:
#   vacards-clipboard-publisher  test-only native clipboard owner (macOS, AppKit)
#   test_text-paste-external     per-case receiver GTest binary, created by the
#                                TEST_SOURCES loop in testfiles/CMakeLists.txt
#   vacards-clipboard-external   CTest entry running run-external-paste.py
#
# Safety contract:
#   * The publisher never links inkscape_base/GDK: a GDK publication would be
#     local to the receiver and would not prove native interoperability.
#   * The suite is opt-in. Without --allow-clipboard-takeover (or
#     VACARDS_CLIP_ALLOW=1) the driver records env-blocked:not-opted-in and
#     exits 77, so a plain `ctest` skips instead of taking the user's clipboard
#     or reporting a false pass. SKIP_RETURN_CODE makes that a CTest "Skipped".
#   * RESOURCE_LOCK os_clipboard + RUN_SERIAL serialize every clipboard test in
#     one ctest run; the driver's flock lease serializes parallel runs from
#     different shells. The lock file is <session-root>/lease.lock, i.e. it does
#     NOT depend on the unique run id, so two concurrent drivers actually contend
#     for one lock (each run keeps its own diagnostic lease.json in its session
#     directory).
#   * vacards-clipboard-protocol-self-test runs run-external-paste.py
#     --protocol-self-test: pure Python protocol checks (paths, two-process lease
#     contention, skip-aware receiver wrapper exit codes, fixture/representation
#     hash binding and publication-exactness oracles, summary accounting, restore
#     decisions with injected failures, snapshot refusal/retention). It never
#     starts the publisher/receiver and never touches the clipboard, so it needs
#     no RESOURCE_LOCK.

if(NOT TARGET test_text-paste-external)
    message(FATAL_ERROR
        "test_text-paste-external is not defined. Add 'text-paste-external-test' to TEST_SOURCES "
        "in testfiles/CMakeLists.txt (next to 'text-paste-test') before add_subdirectory(clipboard). "
        "See testfiles/clipboard/README.md for the exact registration lines.")
endif()

if(APPLE)
    enable_language(OBJCXX)
    add_executable(vacards-clipboard-publisher macos/clipboard-publisher.mm)
    target_link_libraries(vacards-clipboard-publisher PRIVATE "-framework AppKit" "-framework Foundation")
    target_compile_options(vacards-clipboard-publisher PRIVATE -fobjc-arc)
    set_target_properties(vacards-clipboard-publisher PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        OBJCXX_STANDARD 17
        OBJCXX_STANDARD_REQUIRED ON)
    target_compile_options(vacards-clipboard-publisher PRIVATE -Wall -Wextra)

    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/sessions")
    # Every other GUI test profile directory is created at configure time; the
    # receiver exits early when the profile path cannot be created, so an absent
    # clipboard-profile turned a config problem into a confusing receiver error.
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/clipboard-profile")
    add_test(NAME vacards-clipboard-external
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run-external-paste.py"
                --publisher "$<TARGET_FILE:vacards-clipboard-publisher>"
                --receiver "$<TARGET_FILE:test_text-paste-external>"
                --fixtures "${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
                --session-root "${CMAKE_CURRENT_BINARY_DIR}/sessions"
                --evidence "${CMAKE_CURRENT_BINARY_DIR}/clipboard-evidence"
                --source-dir "${CMAKE_SOURCE_DIR}"
                --cases "E01,E02,E04,E05,E06,E07_at_cap,E07_multibyte,E07_over_cap,E13_object_priority_svg_plain,B15_object_priority_svg_html,F13_plain_svg_object_path,E08_object_target_stall,R04_fallback_plain_distinct")
    set_tests_properties(vacards-clipboard-external PROPERTIES
        RESOURCE_LOCK os_clipboard
        RUN_SERIAL TRUE
        TIMEOUT 1800
        SKIP_RETURN_CODE 77
        LABELS "clipboard-external")
    set_property(TEST vacards-clipboard-external APPEND PROPERTY ENVIRONMENT
        "INKSCAPE_TEST_GUI=1"
        "INKSCAPE_FONTCONFIG=${CMAKE_SOURCE_DIR}/testfiles/rendering_tests/fonts/isolated.conf"
        "INKSCAPE_PROFILE_DIR=${CMAKE_CURRENT_BINARY_DIR}/clipboard-profile")
else()
    # No Windows/Linux native publisher is implemented in this checkout (there is
    # no Windows host here). Keep the file valid and the build honest: a clearly
    # labelled stub target, no clipboard test registration.
    message(STATUS
        "vacards-clipboard-external: native publisher implemented for macOS only; "
        "creating a stub vacards-clipboard-publisher target on ${CMAKE_SYSTEM_NAME}, "
        "no vacards-clipboard-external test registered")
    add_custom_target(vacards-clipboard-publisher
        COMMAND "${CMAKE_COMMAND}" -E echo
                "vacards-clipboard-publisher is a macOS-only test helper; no publisher on ${CMAKE_SYSTEM_NAME}")
endif()

# Pure protocol self-test. It spawns only Python children (a fake gtest report
# writer and two lease contenders) and never starts the publisher or receiver,
# so it is safe to run everywhere and needs no clipboard resource lock. fcntl is
# required, which excludes Windows.
if(NOT WIN32)
    add_test(NAME vacards-clipboard-protocol-self-test
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/run-external-paste.py" --protocol-self-test)
    set_tests_properties(vacards-clipboard-protocol-self-test PROPERTIES
        TIMEOUT 180
        LABELS "clipboard-self-test")
endif()

# The publisher is a runtime input of the registered test, not a link input.
add_dependencies(tests vacards-clipboard-publisher)

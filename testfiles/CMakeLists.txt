# SPDX-License-Identifier: GPL-2.0-or-later
# -----------------------------------------------------------------------------

include_directories(${PROJECT_SOURCE_DIR})

# custom "check" target with proper dependencies (builds inkscape and tests)
file(TO_NATIVE_PATH "/" _separator)
add_definitions(-DINKSCAPE_TESTS_DIR="${CMAKE_SOURCE_DIR}/testfiles")
add_definitions(-DINKSCAPE_SHARE_DIR="${CMAKE_SOURCE_DIR}/share")
add_custom_target(check COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
                        DEPENDS tests unit_tests
                        WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_dependencies(check inkscape)

# Keep the complete suite available through `check`, including known upstream
# platform-sensitive tests. `check-regressions` is the actionable gate for the
# current platform and excludes only tests explicitly labelled below.
add_custom_target(check-regressions
                  COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
                          --label-exclude upstream-platform-sensitive
                  DEPENDS tests unit_tests
                  WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_dependencies(check-regressions inkscape)

# create symlink "inkscape_datadir" to use as INKSCAPE_DATADIR
# - ensures tests can be run without installing the project
# - also helpful for running Inkscape uninstalled: 'INKSVAPE_DATADIR=inkscape_datadir bin/inkscape'
set(INKSCAPE_DATADIR ${CMAKE_BINARY_DIR}/inkscape_datadir)
if(NOT EXISTS ${INKSCAPE_DATADIR}/inkscape)
    set(link_source ${INKSCAPE_DATADIR}/inkscape)
    set(link_target ${CMAKE_SOURCE_DIR}/share)
    message(STATUS "Creating link '${link_source}' --> '${link_target}'")
    file(MAKE_DIRECTORY "${INKSCAPE_DATADIR}")
    file(CREATE_LINK "${link_target}" "${link_source}" SYMBOLIC RESULT result)
    if(result)
        message(WARNING "Creation of link failed: ${result}")
    endif()
endif()
# check if creation succeeded
if(EXISTS ${INKSCAPE_DATADIR}/inkscape)
    set(CMAKE_CTEST_ENV INKSCAPE_DATADIR=${INKSCAPE_DATADIR})
else()
    message(WARNING "Directory 'inkscape_datadir/inkscape' missing. Tests might not run properly.\n"
                    "Possible solutions:\n"
                    " - create a suitable symlink yourself, e.g.\n"
                    "   ln -s ${CMAKE_SOURCE_DIR}/share ${INKSCAPE_DATADIR}/inkscape\n"
                    " - run '${CMAKE_MAKE_PROGRAM} install' before running tests (only for not relocatable packages.\n"
                    " - set the environment variable 'INKSCAPE_DATADIR' manually (every time you run tests)")
endif()


# Set custom profile directory for tests using environment variable.
# Copy CTestCustom.cmake into binary dir, where it will be picked up automatically by ctest for cleanup.
set(INKSCAPE_TEST_PROFILE_DIR ${CMAKE_CURRENT_BINARY_DIR}/test_profile_dir)
set(INKSCAPE_TEST_PROFILE_DIR_ENV INKSCAPE_PROFILE_DIR=${INKSCAPE_TEST_PROFILE_DIR})
configure_file(CTestCustom.cmake.in ${CMAKE_BINARY_DIR}/CTestCustom.cmake)

### tests using gtest

if(${CMAKE_SIZEOF_VOID_P} EQUAL 8)
    set(LPE_TESTS_64bit
        #0.92 or lower LPEs
        # (test not stable on 32bit Windows)
        lpe64-test
    )
endif()

set(LIBNRTYPE_TESTS
    libnrtype/Layout-TNG-Compute
    libnrtype/Layout-TNG-Compute-lang
    )

# Integration tests (linking all of Inkscape)
set(TEST_SOURCES
    welcome-recent-model-test
    new-object-visibility-test
    export-formats-test
    clipped-bitmaps-test
    svg-viewer-guide-test
    text-break-apart-test
    paint-order-query-test
    paint-order-widget-test
    tiff-edge-clean-test
    welcome-drawing-preview-test
    preview-css-admission-test
    preview-svg-resource-policy-test
    preview-xml-input-test
    artwork-library-archive-test
    artwork-library-catalog-test
    artwork-library-document-test
    artwork-library-insert-test
    artwork-library-lbart-test
    artwork-library-lightburn-test
    artwork-library-encoding-test
    artwork-library-manifest-test
    artwork-library-package-test
    artwork-library-storage-test
    artwork-library-svg-preflight-test
    artwork-library-svg-preflight-native-test
    artwork-library-host-quit-test
    artwork-library-host-test
    artwork-library-controller-test
    artwork-library-workspace-test
    artwork-library-view-test
    artwork-library-thumbnail-test
    export-destination-test
    export-hints-test
    actions-svg-processing
    async_channel-test
    async_funclog-test
    async_progress-test
    boolop-attr-test
    bitmap-adjustment-chemistry-test
    bitmap-adjustments-controller-test
    bitmap-adjustment-preview-test
    bitmap-eraser-test
    destructive-bitmap-clip-test
    destructive-bitmap-coverage-test
    destructive-bitmap-clip-feedback-test
    dialog-notebook-test
    bitmap-histogram-test
    save-filename-test
    save-target-test
    colors/cms-system-test
    colors/document-cms-test
    colors/dragndrop-test
    colors/color-set-test
    colors/xml-color-test
    compare-paths-test
    object/color-profile-test
    uri-test
    uri-references-test
    util-test
    util-expression-evaluator-test
    util-uri-test
    drag-and-drop-svgz
    drawing-pattern-test
    font-catalog-test
    font-discovery-subscription-test
    font-list-compact-test
    font-list-lifecycle-test
    xml-save-revision-test
    prepared-content-proof-test
    namedview-window-state-test
    desktop-document-binding-test
    desktop-origin-lifetime-test
    preferences-presenter-test
    overwrite-parent-test
    preferences-admission-test
    document-file-operation-test
    document-file-transaction-test
    existing-file-replacement-test
    auto-save-recovery-test
    stroke-width-controller-test
    explode-bitmap-budget-test
    explode-bitmap-label-test
    explode-bitmap-header-test
    explode-bitmap-enclosure-test
    explode-bitmap-memory-test
    explode-bitmap-specks-test
    explode-bitmap-decode-test
    explode-bitmap-target-test
    explode-bitmap-undo-test
    explode-bitmap-jobs-test
    explode-bitmap-output-test
    explode-bitmap-grid-test
    explode-bitmap-png-test
    bitmap-contour-test
    bitmap-contour-offset-test
    bitmap-contour-fit-test
    explode-bitmap-contour-stage-test
    vacards-cli-registry-test
    explode-bitmap-composer-test
    explode-bitmap-candidate-test
    explode-bitmap-view-test
    explode-bitmap-session-test
    explode-bitmap-dependencies-test
    explode-bitmap-overlay-test
    explode-bitmap-publish-test
    explode-bitmap-boundaries-test
    explode-bitmap-panel-test
    explode-bitmap-register-test
    explode-bitmap-cli-test
    explode-calibration-test
    stroke-width-statusbar-test
    stroke-width-row-test
    stroke-legacy-routes-test
    stroke-object-properties-test
    stroke-dash-text-test
    image-open-limits-test
    spin-button-policies-test
    global-palettes-test
    split-path-test
    attributes-test
    dir-util-test
    sp-item-test
    sp-object-test
    sp-object-lang-test
    sp-object-tags-test
    object-links-test
    object-set-test
    object-style-test
    page-management
    path-reverse-lpe-test
    preferences-test
    pure-transform-test
    rebase-hrefs-test
    selector-interaction-test
    selection-rotation-feedback-test
    selector-percent-scale-test
    stream-test
    style-elem-test
    style-internal-test
    style-test
    text-font-preview-test
    text-multi-style-test
    text-paste-test
    text-paste-html-test
    text-paste-rtf-test
    vacards-cli-result-test
    text-paste-font-test
    text-paste-external-test
    text-paste-unit-test
    text-paragraph-target-test
    text-shape-none-editing-test
    svg-affine-test
    svg-box-test
    svg-output-completion-test
    xml-read-report-test
    svg-path-parser-test
    sp-gradient-test
    svg-path-geom-test
    visual-bounds-test
    geom-pathstroke-test
    livarot-pathoutline-test
    object-test
    sp-glyph-kerning-test
    cairo-utils-test
    svg-extension-test
    metafile-removal-test
    2geom-characterization-test
    test-feDropShadow
    xml-test
    xml-attribute-batch-test
    sp-item-group-test
    store-test
    lpe-test
    fillet-chamfer-edit-test
    corner-rounding-controller-test
    canvas-preview-test
    canvas-opengl-policy-test
    ui-util-test
    sp-document-test
    object-colors-test
    multi-marker-color-wheel-test
    modifiers-test
    pixel_align-test
    ${LPE_TESTS_64bit}
    ${LIBNRTYPE_TESTS}
    )

if(WITH_VACARDS_NESTING)
    list(APPEND TEST_SOURCES
        nesting-document-test
        nesting-tool-test
        nesting-responsiveness-test
        offset-shapes-test
        offset-shapes-tool-test
    )
    get_target_property(_vacards_sparrow_bundled inkscape_base VACARDS_SPARROW_BUNDLED)
    if(_vacards_sparrow_bundled)
        # Every platform shipping Sparrow must run the same acceptance corpus.
        list(APPEND TEST_SOURCES nesting-sparrow-test)
    endif()
endif()

if(WITH_POPPLER)
    list(APPEND TEST_SOURCES
        pdf-utils-test
        poppler-utils-test
    )
endif()

if(WITH_CAPYPDF)
    list(APPEND TEST_SOURCES
        extensions-pdfoutput-test
    )
endif()

if(WIN32)
    # Link one copy of the static application/2Geom state in each Windows test.
    set(_vacards_test_helper_type STATIC)
else()
    set(_vacards_test_helper_type SHARED)
endif()
add_library(cpp_test_static_library ${_vacards_test_helper_type} inkscape-test.cpp doc-per-case-test.cpp compare-paths-test.h lpespaths-test.h store-integrity-test.h test-with-svg-object-pairs.cpp)
target_link_libraries(cpp_test_static_library PUBLIC ${GTEST_LIBRARIES} inkscape_base)

add_custom_target(tests)
# Resolve the interpreter before registering tests that use Python wrappers.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

foreach(test_source ${TEST_SOURCES})
    string(REPLACE "-test" "" testname "test_${test_source}")
    string(REPLACE "/" "_" testname "${testname}")
    add_executable(${testname} src/${test_source}.cpp)
    target_include_directories(${testname} SYSTEM PRIVATE ${GTEST_INCLUDE_DIRS})
    target_link_libraries(${testname} cpp_test_static_library 2Geom::2geom)
    if(test_source STREQUAL "artwork-library-host-test")
        # Same test inventory/target, but verify the saved session from a second
        # process using a fresh, isolated profile (no explicit preferences save).
        add_test(NAME ${testname}
                 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/artwork-library-session-test.py"
                         "$<TARGET_FILE:${testname}>")
    elseif(test_source STREQUAL "text-paste-external-test")
        # The receiver skips without VACARDS_CLIP_SESSION. A bare gtest run would
        # exit 0 and CTest would report Passed for a run that executed nothing, so
        # the skip-aware wrapper maps "all skipped / zero executed" to 77
        # (SKIP_RETURN_CODE => CTest Skipped) and any bypassed exit-0 skip is
        # caught by the FAIL_REGULAR_EXPRESSION guard. RESOURCE_LOCK serializes it
        # with test_text-paste, the other test that drives the live pasteboard.
        add_test(NAME ${testname}
                 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/clipboard/run-receiver-skip-aware.py"
                         --receiver "$<TARGET_FILE:${testname}>"
                         --gtest-output "${CMAKE_CURRENT_BINARY_DIR}/${testname}-gtest.json")
        set_tests_properties(${testname} PROPERTIES
            SKIP_RETURN_CODE 77
            RESOURCE_LOCK os_clipboard
            RUN_SERIAL TRUE
            TIMEOUT 300)
        # Multiple regexes need set_property (set_tests_properties takes one value
        # per property in CMake 4.x).
        set_property(TEST ${testname} APPEND PROPERTY FAIL_REGULAR_EXPRESSION
            "\\[ +SKIPPED +\\]" "\\[==========\\] Running 0 tests")
    elseif(test_source STREQUAL "text-paste-test")
        # The in-process suite is opt-in for the live pasteboard (the fixture
        # skips before the first clipboard read without
        # VACARDS_CLIP_ALLOW_LIVE=1) and its six clipboard-free fragment tests
        # always run. Register it through the same skip-aware wrapper as the
        # receiver so a run that executes nothing is CTest Skipped (77) rather
        # than a green pass; a normal run reports the skips individually.
        add_test(NAME ${testname}
                 COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/clipboard/run-receiver-skip-aware.py"
                         --receiver "$<TARGET_FILE:${testname}>"
                         --gtest-output "${CMAKE_CURRENT_BINARY_DIR}/${testname}-gtest.json")
        set_tests_properties(${testname} PROPERTIES SKIP_RETURN_CODE 77)
    else()
        add_test(NAME ${testname} COMMAND ${testname})
    endif()
    # Keep saves and opens in tests out of the user's Welcome thumbnail cache.
    set_tests_properties(${testname} PROPERTIES ENVIRONMENT
        "${INKSCAPE_TEST_PROFILE_DIR_ENV}/${testname};VACARDS_WELCOME_THUMBNAIL_TEST_CACHE=off;${CMAKE_CTEST_ENV}")
    add_dependencies(tests ${testname})
endforeach()
# Manual, unsupported-scenario diagnostic: built only on explicit request
# (EXCLUDE_FROM_ALL). Deliberately not registered with add_test or the 'tests'
# aggregate, and absent from every acceptance inventory. Running its disabled
# case requires an exact gtest filter plus --gtest_also_run_disabled_tests.
add_executable(test_selector-interaction-unsupported-owner-diagnostic EXCLUDE_FROM_ALL src/selector-interaction-test.cpp)
target_include_directories(test_selector-interaction-unsupported-owner-diagnostic SYSTEM PRIVATE ${GTEST_INCLUDE_DIRS})
target_link_libraries(test_selector-interaction-unsupported-owner-diagnostic cpp_test_static_library 2Geom::2geom)
target_compile_definitions(test_selector-interaction-unsupported-owner-diagnostic PRIVATE INKSCAPE_UNSUPPORTED_OWNER_DIAGNOSTIC)
# Runtime policy is an immutable process singleton: each policy must execute in
# its own process. Neither invocation may silently skip or run an empty filter.
add_executable(test_preview-runtime-policy src/preview-runtime-policy-test.cpp)
target_include_directories(test_preview-runtime-policy SYSTEM PRIVATE ${GTEST_INCLUDE_DIRS})
target_link_libraries(test_preview-runtime-policy cpp_test_static_library 2Geom::2geom)
add_dependencies(tests test_preview-runtime-policy)
foreach(policy Editor Helper)
    string(TOLOWER "${policy}" policy_name)
    set(policy_test "test_preview-runtime-policy-${policy_name}")
    add_test(NAME ${policy_test}
        COMMAND test_preview-runtime-policy "--gtest_filter=PreviewRuntimePolicy${policy}.*")
    set_tests_properties(${policy_test} PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE
        ENVIRONMENT "${INKSCAPE_TEST_PROFILE_DIR_ENV}/${policy_test};${CMAKE_CTEST_ENV}")
    set_property(TEST ${policy_test} APPEND PROPERTY ENVIRONMENT
        "G_DEBUG=fatal-criticals"
        "INKSCAPE_FONTCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/rendering_tests/fonts/isolated.conf")
    set_property(TEST ${policy_test} APPEND PROPERTY FAIL_REGULAR_EXPRESSION
        "\\[ *SKIPPED *\\]" "YOU HAVE [1-9][0-9]* DISABLED TEST" "\\[==========\\] Running 0 tests")
endforeach()
# Completed Quit requires a fresh process for each native application case.
add_executable(test_preferences-app-binding src/preferences-app-binding-test.cpp)
target_include_directories(test_preferences-app-binding SYSTEM PRIVATE ${GTEST_INCLUDE_DIRS})
target_link_libraries(test_preferences-app-binding cpp_test_static_library 2Geom::2geom)
add_dependencies(tests test_preferences-app-binding)
set(PREFERENCES_APP_BINDING_TESTS)
foreach(binding_case IN ITEMS
    "zero-document|PreferencesAppBindingTest.ZeroDocumentActionCreatesAndReusesOwnedHost"
    "last-document|PreferencesAppBindingTest.ClosingLastDocumentRetainsOwnedHostAndClearsDesktop"
    "desktop-switch|PreferencesAppBindingTest.ActiveDesktopSwitchRebindsOwnedPanel"
    "quit-cancel|PreferencesAppBindingQuitTest.ShutdownObserverDocumentCancelKeepsDocumentAndFreshPreferences"
    "quit-complete|PreferencesAppBindingQuitTest.ExplicitQuitDestroysOwnedHost"
)
    string(REPLACE "|" ";" binding_parts "${binding_case}")
    list(GET binding_parts 0 binding_name)
    list(GET binding_parts 1 binding_filter)
    set(binding_test "test_preferences-app-binding-${binding_name}")
    list(APPEND PREFERENCES_APP_BINDING_TESTS "${binding_test}")
    add_test(NAME ${binding_test}
        COMMAND test_preferences-app-binding "--gtest_filter=${binding_filter}")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${binding_test}-profile")
    set_tests_properties(${binding_test} PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE
        ENVIRONMENT "${CMAKE_CTEST_ENV}")
    set_property(TEST ${binding_test} APPEND PROPERTY ENVIRONMENT
        "INKSCAPE_TEST_GUI=1"
        "INKSCAPE_PROFILE_DIR=${CMAKE_CURRENT_BINARY_DIR}/${binding_test}-profile"
        "INKSCAPE_FONTCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/rendering_tests/fonts/isolated.conf"
        "LC_ALL=C" "LANG=C" "G_DEBUG=fatal-criticals")
    set_property(TEST ${binding_test} APPEND PROPERTY FAIL_REGULAR_EXPRESSION
        "\\[ *SKIPPED *\\]" "YOU HAVE [1-9][0-9]* DISABLED TEST" "\\[==========\\] Running 0 tests")
endforeach()
if(TARGET test_nesting-sparrow)
    target_compile_definitions(test_nesting-sparrow PRIVATE
        NESTING_SPARROW_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/nesting/sparrow")
    set_tests_properties(test_nesting-sparrow PROPERTIES TIMEOUT 420 RUN_SERIAL TRUE)
    # T16 publishes and reopens 20,000 staged pieces (about 200 s on the M2 Mac).
    set_tests_properties(test_explode-bitmap-publish PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE)
endif()
if(TARGET test_nesting-document)
    # R1 parity gate corpus (consolidated work order 8.7).
    target_compile_definitions(test_nesting-document PRIVATE
        NESTING_SPARROW_CORPUS="${CMAKE_CURRENT_SOURCE_DIR}/nesting/sparrow"
        NESTING_RESPONSIVENESS_FIXTURES="${CMAKE_CURRENT_BINARY_DIR}/nesting-responsiveness")
endif()
if(TARGET test_nesting-responsiveness)
    # Measurement suite (consolidated nesting work order R0/R5a). Timings are
    # machine-dependent: never part of VACARDS_CRITICAL_TESTS.
    # The fixtures (about 22 MB) are generated into the build tree, not committed.
    set(_nesting_responsiveness_fixtures "${CMAKE_CURRENT_BINARY_DIR}/nesting-responsiveness")
    target_compile_definitions(test_nesting-responsiveness PRIVATE
        NESTING_RESPONSIVENESS_FIXTURES="${_nesting_responsiveness_fixtures}"
        NESTING_SPARROW_CORPUS="${CMAKE_CURRENT_SOURCE_DIR}/nesting/sparrow")
    add_test(NAME test_nesting-responsiveness-fixtures
             COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/nesting/responsiveness/make_fixtures.py"
                     "${_nesting_responsiveness_fixtures}")
    set_tests_properties(test_nesting-responsiveness-fixtures PROPERTIES
        FIXTURES_SETUP nesting_responsiveness_fixtures LABELS nesting-perf TIMEOUT 300)
    set_tests_properties(test_nesting-responsiveness PROPERTIES TIMEOUT 900 RUN_SERIAL TRUE LABELS nesting-perf
        FIXTURES_REQUIRED nesting_responsiveness_fixtures)
endif()
set_tests_properties(test_corner-rounding-controller test_canvas-preview PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
target_compile_definitions(test_welcome-drawing-preview PRIVATE
    WELCOME_DRAWING_PREVIEW_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/data/welcome-drawing-preview")
set_tests_properties(test_welcome-drawing-preview PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
set_property(TEST test_welcome-drawing-preview APPEND PROPERTY ENVIRONMENT
    "G_DEBUG=fatal-criticals"
    "INKSCAPE_FONTCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/rendering_tests/fonts/isolated.conf")
set_property(TEST test_welcome-drawing-preview APPEND PROPERTY FAIL_REGULAR_EXPRESSION
    "\\[ *SKIPPED *\\]" "YOU HAVE [1-9][0-9]* DISABLED TEST" "\\[==========\\] Running 0 tests")
set_property(TEST test_corner-rounding-controller test_canvas-preview APPEND PROPERTY ENVIRONMENT
    "INKSCAPE_TEST_GUI=1" "G_DEBUG=fatal-criticals"
    "INKSCAPE_FONTCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/rendering_tests/fonts/isolated.conf")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/metafile-removal-extensions")
set_tests_properties(test_artwork-library-svg-preflight test_artwork-library-svg-preflight-native
    test_artwork-library-insert
    test_artwork-library-lightburn
    PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
set_property(TEST test_artwork-library-svg-preflight test_artwork-library-svg-preflight-native
    test_artwork-library-insert
    test_artwork-library-thumbnail
    APPEND PROPERTY ENVIRONMENT "G_DEBUG=fatal-criticals"
    "INKSCAPE_FONTCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/rendering_tests/fonts/isolated.conf")
set_property(TEST test_export-hints APPEND PROPERTY ENVIRONMENT
    "INKSCAPE_EXPORT_HINTS_TEST_BINARY=$<TARGET_FILE:inkscape>"
    "INKSCAPE_TEST_GUI=1"
    "G_DEBUG=fatal-criticals"
    "INKSCAPE_FONTCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/rendering_tests/fonts/isolated.conf")
set_tests_properties(test_export-hints PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
add_dependencies(test_export-hints inkscape)
set_property(TEST test_metafile-removal APPEND PROPERTY ENVIRONMENT
    "INKSCAPE_EXTENSIONS_DIR=${CMAKE_CURRENT_BINARY_DIR}/metafile-removal-extensions")

# These fixtures are known to produce architecture-dependent geometry on
# Apple Silicon in an unmodified upstream build. Do not disable them: the full
# suite must continue to expose their status. The label only keeps those known
# failures from obscuring regressions in the supported-platform gate.
if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    set_tests_properties(test_geom-pathstroke test_lpe test_lpe64 PROPERTIES
                         LABELS upstream-platform-sensitive)
endif()

foreach(test_source ${LIBNRTYPE_TESTS})
    string(REPLACE "/" "_" testname "test_${test_source}")
    set_property(TEST ${testname} APPEND PROPERTY ENVIRONMENT "INKSCAPE_FONTCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/rendering_tests/fonts/isolated.conf")
endforeach()
set_property(TEST test_text-multi-style APPEND PROPERTY ENVIRONMENT
    "INKSCAPE_FONTCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/rendering_tests/fonts/isolated.conf")
set_property(TEST test_text-shape-none-editing APPEND PROPERTY ENVIRONMENT
    "INKSCAPE_FONTCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/rendering_tests/fonts/isolated.conf"
    "G_DEBUG=fatal-criticals")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/text-multi-style-profile")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/stroke-width-controller-profile")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/font-discovery-subscription-profile")
set_tests_properties(test_font-discovery-subscription PROPERTIES RUN_SERIAL TRUE TIMEOUT 240)
set_property(TEST test_font-discovery-subscription APPEND PROPERTY ENVIRONMENT
    "INKSCAPE_PROFILE_DIR=${CMAKE_CURRENT_BINARY_DIR}/font-discovery-subscription-profile"
    "INKSCAPE_FONTCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/rendering_tests/fonts/isolated.conf"
    "G_DEBUG=fatal-criticals")
set_property(TEST test_font-discovery-subscription APPEND PROPERTY FAIL_REGULAR_EXPRESSION
    "\\[ *SKIPPED *\\]" "YOU HAVE [1-9][0-9]* DISABLED TEST" "\\[==========\\] Running 0 tests")
set_tests_properties(test_stroke-width-controller PROPERTIES RUN_SERIAL TRUE TIMEOUT 180)
# Autosave exercises the native Recent manager; isolate both settings and data.
foreach(autosave_dir IN ITEMS profile data cache)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/auto-save-recovery-${autosave_dir}")
endforeach()
set_tests_properties(test_auto-save-recovery PROPERTIES RUN_SERIAL TRUE TIMEOUT 180)
set_property(TEST test_auto-save-recovery APPEND PROPERTY ENVIRONMENT
    "INKSCAPE_TEST_GUI=1"
    "INKSCAPE_PROFILE_DIR=${CMAKE_CURRENT_BINARY_DIR}/auto-save-recovery-profile"
    "XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/auto-save-recovery-data"
    "XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/auto-save-recovery-cache"
    "INKSCAPE_FONTCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/rendering_tests/fonts/isolated.conf"
    "LC_ALL=C" "LANG=C" "G_DEBUG=fatal-criticals")
set_property(TEST test_auto-save-recovery APPEND PROPERTY FAIL_REGULAR_EXPRESSION
    "\\[ *SKIPPED *\\]" "YOU HAVE [1-9][0-9]* DISABLED TEST" "\\[==========\\] Running 0 tests")
set_property(TEST test_stroke-width-controller APPEND PROPERTY ENVIRONMENT
    "INKSCAPE_PROFILE_DIR=${CMAKE_CURRENT_BINARY_DIR}/stroke-width-controller-profile"
    "INKSCAPE_FONTCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/rendering_tests/fonts/isolated.conf"
    "G_DEBUG=fatal-criticals")
set_tests_properties(test_stroke-width-statusbar PROPERTIES RUN_SERIAL TRUE TIMEOUT 180)
set_tests_properties(test_paint-order-widget PROPERTIES RUN_SERIAL TRUE TIMEOUT 180)
foreach(_vacards_gui_suite IN ITEMS test_stroke-width-row test_spin-button-policies)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${_vacards_gui_suite}-profile")
    set_tests_properties(${_vacards_gui_suite} PROPERTIES RUN_SERIAL TRUE TIMEOUT 180)
    set_property(TEST ${_vacards_gui_suite} APPEND PROPERTY ENVIRONMENT
        "INKSCAPE_TEST_GUI=1"
        "INKSCAPE_PROFILE_DIR=${CMAKE_CURRENT_BINARY_DIR}/${_vacards_gui_suite}-profile"
        "INKSCAPE_FONTCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/rendering_tests/fonts/isolated.conf"
        "G_DEBUG=fatal-criticals")
endforeach()
set_property(TEST test_paint-order-widget APPEND PROPERTY ENVIRONMENT
    "INKSCAPE_TEST_GUI=1"
    "INKSCAPE_FONTCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/rendering_tests/fonts/isolated.conf"
    "G_DEBUG=fatal-criticals")
set_property(TEST test_stroke-width-statusbar APPEND PROPERTY ENVIRONMENT
    "INKSCAPE_TEST_GUI=1"
    "INKSCAPE_FONTCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/rendering_tests/fonts/isolated.conf"
    "G_DEBUG=fatal-criticals")
set_property(TEST test_text-multi-style APPEND PROPERTY ENVIRONMENT
    "INKSCAPE_PROFILE_DIR=${CMAKE_CURRENT_BINARY_DIR}/text-multi-style-profile")
# Real FontList consumers and mixed paragraph controls must execute in the
# isolated GUI environment; a skipped fixture cannot qualify these repairs.
set_tests_properties(test_font-list-lifecycle test_text-multi-style
    PROPERTIES RUN_SERIAL TRUE TIMEOUT 180)
set_property(TEST test_font-list-lifecycle test_text-multi-style
    APPEND PROPERTY ENVIRONMENT "INKSCAPE_TEST_GUI=1" "G_DEBUG=fatal-criticals"
    "INKSCAPE_FONTCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/rendering_tests/fonts/isolated.conf")
set_property(TEST test_font-list-lifecycle test_text-multi-style
    APPEND PROPERTY FAIL_REGULAR_EXPRESSION
    "\\[ *SKIPPED *\\]" "YOU HAVE [1-9][0-9]* DISABLED TEST" "\\[==========\\] Running 0 tests")
# Context-aware rich text paste: real Gdk clipboard + actions in an isolated
# GUI profile; font-dependent style assertions use the shared isolated fontconfig.
# Live pasteboard use is OPT-IN and is NOT enabled by this registration: an
# ordinary `ctest` run must never take the user's clipboard. The fixture skips
# before the first clipboard read unless BOTH INKSCAPE_TEST_GUI=1 and
# VACARDS_CLIP_ALLOW_LIVE=1 are present in the environment (the second is the
# explicit disposable-clipboard consent; non-text clipboard content cannot be
# restored in process). A deliberate run sets both.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/text-paste-profile")
# RESOURCE_LOCK os_clipboard serializes this suite with the external-process
# clipboard driver: on macOS GdkClipboard is the user's NSPasteboard, so two
# clipboard tests must never run concurrently. The fixture records a clipboard
# change token and restores only while it still owns the clipboard, so a newer
# user copy is never overwritten.
set_tests_properties(test_text-paste PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE RESOURCE_LOCK os_clipboard)
set_property(TEST test_text-paste APPEND PROPERTY ENVIRONMENT
    "INKSCAPE_FONTCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/rendering_tests/fonts/isolated.conf"
    "INKSCAPE_PROFILE_DIR=${CMAKE_CURRENT_BINARY_DIR}/text-paste-profile")
# The release-critical font policy suite contains a real-FontFactory case that
# needs the isolated font fixture. Without this environment it skips silently and
# check-vacards-critical would report the suite green with a case that never ran
# (AGENTS.md: skipped cases are not passes). The FAIL_REGULAR_EXPRESSION guard
# turns any [ SKIPPED ] or zero-test run into a failure.
set_property(TEST test_text-paste-font APPEND PROPERTY ENVIRONMENT
    "INKSCAPE_FONTCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/rendering_tests/fonts/isolated.conf")
set_property(TEST test_text-paste-font APPEND PROPERTY FAIL_REGULAR_EXPRESSION
    "\\[ +SKIPPED +\\]" "\\[==========\\] Running 0 tests")
# The portable transaction test file is POSIX-only. Keep the same suite name
# on Windows, but compile the real NTFS adapter cases so it cannot pass empty.
if(WIN32)
    target_sources(test_document-file-transaction PRIVATE
        src/document-file-transaction-win32-test.cpp)
endif()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/document-file-operation-profile")
set_tests_properties(test_document-file-operation test_document-file-transaction
    PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
set_property(TEST test_document-file-operation APPEND PROPERTY ENVIRONMENT
    "INKSCAPE_PROFILE_DIR=${CMAKE_CURRENT_BINARY_DIR}/document-file-operation-profile"
    "INKSCAPE_FONTCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/rendering_tests/fonts/isolated.conf"
    "G_DEBUG=fatal-criticals")
set_property(TEST test_document-file-operation test_document-file-transaction
    APPEND PROPERTY FAIL_REGULAR_EXPRESSION
    "\\[ *SKIPPED *\\]" "YOU HAVE [1-9][0-9]* DISABLED TEST" "\\[==========\\] Running 0 tests")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/dialog-notebook-profile")
set_property(TEST test_dialog-notebook APPEND PROPERTY ENVIRONMENT
    "INKSCAPE_TEST_GUI=1"
    "INKSCAPE_PROFILE_DIR=${CMAKE_CURRENT_BINARY_DIR}/dialog-notebook-profile"
    "INKSCAPE_FONTCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/rendering_tests/fonts/isolated.conf"
    "G_DEBUG=fatal-criticals")

set_property(TEST test_dialog-notebook APPEND PROPERTY FAIL_REGULAR_EXPRESSION
    "\\[ *SKIPPED *\\]" "YOU HAVE [1-9][0-9]* DISABLED TEST" "\\[==========\\] Running 0 tests")


# Real native Preferences and namedview outcomes; never accept a skipped GUI run.
foreach(native_gui_test IN ITEMS namedview-window-state desktop-document-binding desktop-origin-lifetime preferences-presenter preferences-admission overwrite-parent new-object-visibility export-formats)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${native_gui_test}-profile")
    set_tests_properties(test_${native_gui_test} PROPERTIES RUN_SERIAL TRUE TIMEOUT 180)
    set_property(TEST test_${native_gui_test} APPEND PROPERTY ENVIRONMENT
        "INKSCAPE_TEST_GUI=1"
        "INKSCAPE_PROFILE_DIR=${CMAKE_CURRENT_BINARY_DIR}/${native_gui_test}-profile"
        "INKSCAPE_FONTCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/rendering_tests/fonts/isolated.conf"
        "G_DEBUG=fatal-criticals")
    set_property(TEST test_${native_gui_test} APPEND PROPERTY FAIL_REGULAR_EXPRESSION
        "\\[ *SKIPPED *\\]" "YOU HAVE [1-9][0-9]* DISABLED TEST" "\\[==========\\] Running 0 tests")
endforeach()
set_tests_properties(test_preview-css-admission PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)
set_property(TEST test_preview-css-admission APPEND PROPERTY FAIL_REGULAR_EXPRESSION
    "\\[ *SKIPPED *\\]" "YOU HAVE [1-9][0-9]* DISABLED TEST" "\\[==========\\] Running 0 tests")
set_tests_properties(test_preview-svg-resource-policy PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)
set_property(TEST test_preview-svg-resource-policy APPEND PROPERTY FAIL_REGULAR_EXPRESSION
    "\\[ *SKIPPED *\\]" "YOU HAVE [1-9][0-9]* DISABLED TEST" "\\[==========\\] Running 0 tests")
set_tests_properties(test_preview-xml-input PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)
set_property(TEST test_preview-xml-input APPEND PROPERTY FAIL_REGULAR_EXPRESSION
    "\\[ *SKIPPED *\\]" "YOU HAVE [1-9][0-9]* DISABLED TEST" "\\[==========\\] Running 0 tests")
set_tests_properties(test_xml-save-revision test_prepared-content-proof PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)
set_property(TEST test_xml-save-revision test_prepared-content-proof APPEND PROPERTY FAIL_REGULAR_EXPRESSION
    "\\[ *SKIPPED *\\]" "YOU HAVE [1-9][0-9]* DISABLED TEST" "\\[==========\\] Running 0 tests")

include(${CMAKE_SOURCE_DIR}/CMakeScripts/UnitTest.cmake)

### Unit tests
#
# In order to add a unit test, call add_unit_test() providing a unique name for the test,
# and then the following named parameters:
# TEST_SOURCE followed by the file containing the test code (relative to testfiles/src)
# SOURCES     followed by a list of files containing the code under test (relative to src/)
# EXTRA_LIBS  followed by libraries which you need to be linked to the test.
#
add_unit_test(assertions-in-tests TEST_SOURCE "assertions-in-tests.cpp")
add_unit_test(version-test TEST_SOURCE "version-test.cpp"
                               SOURCES "version.cpp")

add_unit_test(bitmap-tone-test TEST_SOURCE "bitmap-tone-test.cpp"
                                SOURCES "display/bitmap-tone.cpp")

find_package(Threads REQUIRED)
add_unit_test(canvas-rendering-stats-test
              TEST_SOURCE "ui/widget/canvas-rendering-stats-test.cpp"
              EXTRA_LIBS Threads::Threads)
set_tests_properties(canvas-rendering-stats-test PROPERTIES TIMEOUT 30)

add_unit_test(windows-rendering-test TEST_SOURCE "windows-rendering-test.cpp"
              EXTRA_LIBS GLibmm::GLibmm)
set_tests_properties(windows-rendering-test PROPERTIES TIMEOUT 30 RUN_SERIAL TRUE)

add_unit_test(tiff-export-test TEST_SOURCE "tiff-export-test.cpp"
                                SOURCES "io/tiff-export.cpp"
                                EXTRA_LIBS PkgConfig::INKSCAPE_DEP PNG::PNG TIFF::TIFF)

add_unit_test(vacards-cli-params-test TEST_SOURCE "vacards-cli-params-test.cpp"
                                      SOURCES "actions/vacards-cli-params.cpp"
                                      EXTRA_LIBS GLibmm::GLibmm)

add_unit_test(string-convert-test TEST_SOURCE "string-convert-test.cpp"
                                  SOURCES "util-string/string-convert.cpp"
                                  EXTRA_LIBS GLibmm::GLibmm)

# These all depend on each other as a single unit
add_unit_tests(TEST_SOURCES "svg-length-test.cpp"
                            "svg-stringstream-test.cpp"
                            "util-units-test.cpp"
                    SOURCES "svg/css-ostringstream.cpp"
                            "svg/svg-length.cpp"
                            "svg/stringstream.cpp"
                            "svg/strip-trailing-zeros.cpp"
                            "util/numeric/precision.cpp"
                            "util/units.cpp"
                            # MOCK object files to unitise these tests
                            "mocks/util-units-filename.cpp"
                    EXTRA_LIBS GLibmm::GLibmm 2Geom::2geom)

include("${CMAKE_SOURCE_DIR}/src/libnrtype/CMakeUnit.txt")
get_libnrtype_unit_lib()

add_unit_test(font-feature-utils-test
              TEST_SOURCE "libnrtype/font-feature-utils-test.cpp"
              SOURCES "libnrtype/font-feature-utils.cpp"
              EXTRA_LIBS GLibmm::GLibmm)

add_unit_test(font-resource-list-test
              TEST_SOURCE "libnrtype/font-resource-list-test.cpp"
              SOURCES "io/resource-list.cpp"
              EXTRA_LIBS GLibmm::GLibmm)

add_unit_tests(TEST_SOURCES "libnrtype/font-factory-test.cpp"
                            "libnrtype/font-instance-test.cpp"
                            "libnrtype/font-utils-test.cpp"

                ENVIRONMENT "INKSCAPE_FONTCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/rendering_tests/fonts/isolated.conf"
                            "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan.suppression"
                 EXTRA_LIBS libnrtype_unit_lib)

include("${CMAKE_SOURCE_DIR}/src/colors/CMakeUnit.txt")
get_color_unit_lib()
add_unit_tests(TEST_SOURCES "colors/cms-profile-test.cpp"
                            "colors/cms-transform-color-test.cpp"
                            "colors/cms-transform-surface-test.cpp"
                            "colors/color-test.cpp"
                            "colors/gamut-test.cpp"
                            "colors/manager-test.cpp"
                            "colors/parser-test.cpp"
                            "colors/printer-test.cpp"
                            "colors/gradient-averager-test.cpp"
                            "colors/spaces-alpha-test.cpp"
                            "colors/spaces-cms-test.cpp"
                            "colors/spaces-cmyk-test.cpp"
                            "colors/spaces-convert-static-test.cpp"
                            "colors/spaces-gray-test.cpp"
                            "colors/spaces-hsl-test.cpp"
                            "colors/spaces-hsluv-test.cpp"
                            "colors/spaces-hsv-test.cpp"
                            "colors/spaces-lab-test.cpp"
                            "colors/spaces-lch-test.cpp"
                            "colors/spaces-linear-rgb-test.cpp"
                            "colors/spaces-luv-test.cpp"
                            "colors/spaces-named-test.cpp"
                            "colors/spaces-okhsl-test.cpp"
                            "colors/spaces-okhsv-test.cpp"
                            "colors/spaces-oklab-test.cpp"
                            "colors/spaces-oklch-test.cpp"
                            "colors/spaces-rgb-test.cpp"
                            "colors/spaces-xyz-test.cpp"
                            "colors/utils-test.cpp"
                 EXTRA_LIBS colors_unit_lib)

include("${CMAKE_SOURCE_DIR}/src/path/CMakeUnit.txt")
get_path_unit_lib()
add_unit_tests(TEST_SOURCES "path/path-boolop-test.cpp"
                            "path/path-curve-test.cpp"
                 EXTRA_LIBS path_unit_lib)

add_unit_test(css-syntactic-decomposition-test TEST_SOURCE "css-syntactic-decomposition-test.cpp"
                                                   SOURCES "css/syntactic-decomposition.cpp"
                                                EXTRA_LIBS GLibmm::GLibmm croco_LIB)
if(NOT WIN32)
    # This test does not link on Windows
    add_unit_test(xslt-extension-test TEST_SOURCE "xslt-extension-test.cpp"
                                          SOURCES "extension/implementation/xslt.cpp"
                                       EXTRA_LIBS LibXml2::LibXml2 PkgConfig::SIGC++ GLibmm::GLibmm 2Geom::2geom)
endif()
add_unit_test(mod360-test TEST_SOURCE "mod360-test.cpp"
                              SOURCES "mod360.cpp"
                           EXTRA_LIBS 2Geom::2geom)
add_unit_test(treeify-test TEST_SOURCE "treeify-test.cpp"
                               SOURCES "util/treeify.cpp")

add_dependencies(tests unit_tests)

### CLI rendering tests and LPE
# Runtime library path shared by the maintained CTest wrappers and the
# rendering-test wrappers. The macOS build-tool/shell launch chain can discard
# inherited DYLD_* variables before the test binaries start; explicit
# `cmake -E env` arguments re-supply the pinned dependency paths. Compose the
# colon list from the non-empty configured prefixes only (Cairo first, then
# libcdr, then GTK); never append /lib for an empty prefix and do not inherit an
# unvalidated external DYLD/search path. Defined here, before
# add_subdirectory(rendering_tests), so the rendering wrappers can consume the
# same composed path that the maintained wrapper targets use.
set(VACARDS_TEST_RUNTIME_LIBRARY_PATH "")
set(VACARDS_CTEST_COMMAND ${CMAKE_CTEST_COMMAND})
if(APPLE AND VACARDS_REQUIRE_PATCHED_CAIRO)
    set(_vacards_test_runtime_dirs)
    foreach(_vacards_test_prefix IN ITEMS
            "${VACARDS_RESOLVED_CAIRO_PREFIX}"
            "${VACARDS_RESOLVED_LIBCDR_PREFIX}"
            "${GTK_PREFIX}")
        if(NOT _vacards_test_prefix STREQUAL "")
            list(APPEND _vacards_test_runtime_dirs "${_vacards_test_prefix}/lib")
        endif()
    endforeach()
    if(_vacards_test_runtime_dirs)
        list(JOIN _vacards_test_runtime_dirs ":" VACARDS_TEST_RUNTIME_LIBRARY_PATH)
        set(VACARDS_CTEST_COMMAND ${CMAKE_COMMAND} -E env
            "DYLD_LIBRARY_PATH=${VACARDS_TEST_RUNTIME_LIBRARY_PATH}"
            ${CMAKE_CTEST_COMMAND})
    endif()
endif()

add_subdirectory(cli_tests)
add_subdirectory(rendering_tests)
add_subdirectory(lpe_tests)

foreach(mode IN ITEMS batch shell headless)
    add_test(NAME vacards-immediate-quit-${mode}-test
             COMMAND "${CMAKE_COMMAND}"
                     "-DINKSCAPE_EXECUTABLE=$<TARGET_FILE:inkscape>"
                     "-DOUTPUT_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/immediate-quit-evidence"
                     "-DMODE=${mode}"
                     -P "${CMAKE_CURRENT_SOURCE_DIR}/cli_tests/immediate-quit-smoke.cmake")
    set_tests_properties(vacards-immediate-quit-${mode}-test PROPERTIES
                         ENVIRONMENT "${CMAKE_CTEST_ENV};LC_ALL=C"
                         TIMEOUT 75 RUN_SERIAL TRUE)
    if(NOT mode STREQUAL "headless")
        set_property(TEST vacards-immediate-quit-${mode}-test APPEND PROPERTY LABELS vacards-gui)
    endif()
endforeach()

add_test(NAME vacards-versioning-test
         COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/vacards-versioning-test.sh"
                    "${CMAKE_SOURCE_DIR}/packaging/vacards/vacards-version.sh"
                    "${CMAKE_SOURCE_DIR}/packaging/vacards/VERSION.env"
                    "${CMAKE_SOURCE_DIR}/CMakeScripts/inkscape-version.cmake")
add_test(NAME vacards-dependencies-test
         COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/vacards-dependencies-test.sh"
                    "${CMAKE_SOURCE_DIR}/packaging/vacards/vacards-dependencies.sh"
                    "${CMAKE_SOURCE_DIR}/packaging/macos/vacards/verify-vacards-libcdr.sh"
                    "${CMAKE_SOURCE_DIR}/packaging/macos/vacards/verify-vacards-cairo-prefix.sh"
                    "${CMAKE_SOURCE_DIR}/VACARDS-DEPENDENCIES.env")
add_executable(make-test-rgb-profile make-test-rgb-profile.cpp)
target_link_libraries(make-test-rgb-profile PRIVATE PkgConfig::INKSCAPE_DEP)
add_dependencies(tests make-test-rgb-profile)
add_test(NAME metafile-removal-cli-test
         COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/metafile-removal-cli-test.py"
                 $<TARGET_FILE:inkscape> $<TARGET_FILE:make-test-rgb-profile>)
set_tests_properties(metafile-removal-cli-test PROPERTIES
                    ENVIRONMENT "${CMAKE_CTEST_ENV};LC_ALL=C"
                    TIMEOUT 600)
add_test(NAME text-vertical-anchor-test
         COMMAND "${Python3_EXECUTABLE}"
                 "${CMAKE_CURRENT_SOURCE_DIR}/rendering_tests/text-vertical-anchor-test.py"
                 "$<TARGET_FILE:inkscape>")
set_tests_properties(text-vertical-anchor-test PROPERTIES
    ENVIRONMENT "${INKSCAPE_TEST_PROFILE_DIR_ENV}/text-vertical-anchor-test;XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/text-vertical-anchor-cache;${CMAKE_CTEST_ENV}"
    TIMEOUT 90)
add_test(NAME vacards-workflow-test
         COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/vacards-workflow-test.py")
# VACards agent command line: one test per case group in cli_tests/vacards-agent/<group>.
# Adding a case file needs no reconfigure; an empty group reports Skipped (77).
foreach(group IN ITEMS core query clip geometry bitmap library text nest save)
    add_test(NAME vacards-agent-cli-${group}
             COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/vacards-agent-cli-test.py"
                     "$<TARGET_FILE:inkscape>" "${CMAKE_CURRENT_SOURCE_DIR}/cli_tests/vacards-agent/${group}")
    set_tests_properties(vacards-agent-cli-${group} PROPERTIES
        ENVIRONMENT "INKSCAPE_FONTCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/rendering_tests/fonts/isolated.conf;${CMAKE_CTEST_ENV};LC_ALL=C"
        SKIP_RETURN_CODE 77
        LABELS vacards-agent-cli
        TIMEOUT 600)
endforeach()
add_test(NAME vacards-rust-target-test
         COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/vacards-rust-target-test.py")
foreach(contract IN ITEMS shared-metadata hash-evidence test-results)
    add_test(NAME vacards-${contract}-test
             COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/vacards-${contract}-test.py")
endforeach()
# Native Windows process startup makes the complete gate fixture exceed the
# default 180 seconds. Give each unchanged unittest method its own bounded test;
# do not extend the gate operations' internal deadlines or omit subcases.
set(_vacards_gate_fixture "${CMAKE_CURRENT_SOURCE_DIR}/vacards-gate-run-test.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_vacards_gate_fixture}")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${_vacards_gate_fixture}" --list-ctest-methods
                RESULT_VARIABLE _vacards_gate_discovery_status
                OUTPUT_VARIABLE _vacards_gate_methods OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _vacards_gate_discovery_status EQUAL 0 OR _vacards_gate_methods STREQUAL "")
    message(FATAL_ERROR "Cannot discover VACards gate fixture methods")
endif()
string(REPLACE "\n" ";" _vacards_gate_methods "${_vacards_gate_methods}")
set(VACARDS_GATE_CONTRACT_TESTS vacards-gate-run-test.inventory)
add_test(NAME vacards-gate-run-test.inventory
         COMMAND "${Python3_EXECUTABLE}" "${_vacards_gate_fixture}"
                 --check-ctest-inventory "${CMAKE_CTEST_COMMAND}" "${CMAKE_BINARY_DIR}")
foreach(_method IN LISTS _vacards_gate_methods)
    if(NOT _method MATCHES "^test_[A-Za-z0-9_]+$")
        message(FATAL_ERROR "Invalid VACards gate fixture method: ${_method}")
    endif()
    set(_name "vacards-gate-run-test.${_method}")
    add_test(NAME "${_name}" COMMAND "${Python3_EXECUTABLE}" "${_vacards_gate_fixture}"
             "GateRunTest.${_method}" -v)
    list(APPEND VACARDS_GATE_CONTRACT_TESTS "${_name}")
endforeach()
set_tests_properties(${VACARDS_GATE_CONTRACT_TESTS} PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
if(APPLE)
    # Real POSIX/macOS release-gate orchestration over compiler-free fixtures.
    # This is not native Windows packaging or application qualification.
    add_test(NAME vacards-gate-wiring-test
             COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/vacards-gate-wiring-test.py")
    # The expanded 47-case serial fixture measured 575.550 s on the 8 GB Mac.
    # Allow orchestration overhead without changing any case/product deadline.
    set_tests_properties(vacards-gate-wiring-test PROPERTIES LABELS vacards-critical TIMEOUT 900)
    add_test(NAME vacards-release-attestation-test
             COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/vacards-release-attestation-test.py")
    set_tests_properties(vacards-release-attestation-test PROPERTIES LABELS vacards-critical TIMEOUT 120)
endif()
if(WITH_VACARDS_NESTING)
    add_test(NAME vacards-phase0-metadata-test
             COMMAND "${CMAKE_COMMAND}" -E env
                     "VACARDS_TEST_RUSTC=${VACARDS_NESTING_RUSTC_EXECUTABLE}"
                     "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/vacards-phase0-metadata-test.py")
endif()
if(APPLE)
    add_test(NAME vacards-tiff-loader-launcher-test
             COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/vacards-tiff-loader-launcher-test.py")
    set_tests_properties(vacards-tiff-loader-launcher-test PROPERTIES
                         LABELS vacards-critical TIMEOUT 60)
    # Native internal-test bundle bootstrap: real helper, temp app-shaped layouts.
    add_unit_test(macos-bundle-bootstrap-test TEST_SOURCE "macos-bundle-bootstrap-test.cpp"
                  EXTRA_LIBS inkscape_base)
    set_tests_properties(macos-bundle-bootstrap-test PROPERTIES
                         LABELS vacards-critical TIMEOUT 60)
endif()

add_test(NAME vacards-packaging-inputs-test
         COMMAND "${Python3_EXECUTABLE}" -X utf8 "${CMAKE_CURRENT_SOURCE_DIR}/src/vacards-packaging-inputs-test.py")
add_test(NAME vacards-windows-cairo-policy-test
         COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/vacards-windows-cairo-test.py")
if(WIN32)
    # The 26-case Windows Cairo policy unittest measured 179.078 s; the inherited
    # 180 s default timed out after 24 cases. Raise only this Windows budget.
    set_tests_properties(vacards-windows-cairo-policy-test PROPERTIES TIMEOUT 600)
endif()
add_test(NAME vacards-packager-env-test
         COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/vacards-packager-env-test.py")
add_executable(vacards-cairo-replay-test vacards-cairo-replay-test.c)
set_target_properties(vacards-cairo-replay-test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_link_libraries(vacards-cairo-replay-test PRIVATE PkgConfig::INKSCAPE_DEP)
add_test(NAME vacards-cairo-replay-test COMMAND vacards-cairo-replay-test)
if(APPLE AND VACARDS_REQUIRE_PATCHED_CAIRO)
    set_property(TEST vacards-cairo-replay-test PROPERTY
                 ENVIRONMENT "DYLD_LIBRARY_PATH=${VACARDS_RESOLVED_CAIRO_PREFIX}/lib")
elseif(WIN32 AND VACARDS_REQUIRE_PATCHED_CAIRO)
    set_property(TEST vacards-cairo-replay-test PROPERTY ENVIRONMENT_MODIFICATION
                 "PATH=path_list_prepend:${VACARDS_RESOLVED_CAIRO_PREFIX}/bin")
endif()

# Shared target contract and its application consumers.
add_subdirectory(selection-contract)
# External-process clipboard harness (native publisher + opt-in E-series driver).
add_subdirectory(clipboard)
add_dependencies(tests test_operation-targets)
set_property(TEST test_bitmap-adjustments-controller test_bitmap-adjustment-chemistry
    test_destructive-bitmap-clip test_stroke-width-controller test_stroke-width-statusbar
    test_stroke-width-row test_spin-button-policies test_paint-order-query test_paint-order-widget
    APPEND PROPERTY LABELS selection-contract)
# GUI fixtures that skip without a display must not make this contract green.
set_property(TEST test_bitmap-adjustments-controller test_bitmap-adjustment-chemistry
    test_destructive-bitmap-clip test_stroke-width-controller test_stroke-width-statusbar
    test_stroke-width-row test_spin-button-policies test_paint-order-query test_paint-order-widget
    APPEND PROPERTY FAIL_REGULAR_EXPRESSION "\\[ *SKIPPED *\\]" "YOU HAVE [1-9][0-9]* DISABLED TEST" "\\[==========\\] Running 0 tests")

add_custom_target(check-selection-contract
    COMMAND ${VACARDS_CTEST_COMMAND} --output-on-failure --parallel 1 --no-tests=error
            --tests-regex "^test_(operation-targets|bitmap-adjustments-controller|bitmap-adjustment-chemistry|destructive-bitmap-clip|stroke-width-controller|stroke-width-statusbar|stroke-width-row|spin-button-policies|paint-order-query|paint-order-widget)$"
    DEPENDS test_operation-targets test_bitmap-adjustments-controller test_bitmap-adjustment-chemistry
            test_destructive-bitmap-clip test_stroke-width-controller test_stroke-width-statusbar
            test_stroke-width-row test_spin-button-policies test_paint-order-query test_paint-order-widget
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    VERBATIM)

# VACards release-critical coverage. This deliberately names the tests that
# protect customized behavior instead of relying on a broad name pattern. New
# VACards features must add their regression tests here before they can be
# considered release-protected.
set(VACARDS_CRITICAL_TESTS
    test_preview-runtime-policy-editor
    test_preview-runtime-policy-helper
    test_font-discovery-subscription
    test_stroke-width-controller
    test_stroke-width-statusbar
    test_stroke-width-row
    test_spin-button-policies
    test_paint-order-query
    test_paint-order-widget
    test_welcome-recent-model
    test_welcome-drawing-preview
    test_preview-css-admission
    test_preview-svg-resource-policy
    test_preview-xml-input
    test_fillet-chamfer-edit
    test_corner-rounding-controller
    test_canvas-preview
    vacards-immediate-quit-batch-test
    vacards-immediate-quit-shell-test
    vacards-immediate-quit-headless-test
    test_artwork-library-archive
    test_artwork-library-catalog
    test_artwork-library-document
    test_artwork-library-insert
    test_xml-attribute-batch
    test_artwork-library-lbart
    test_artwork-library-lightburn
    test_artwork-library-encoding
    test_artwork-library-manifest
    test_artwork-library-package
    test_artwork-library-storage
    test_artwork-library-svg-preflight
    test_artwork-library-svg-preflight-native
    test_artwork-library-host-quit
    test_artwork-library-host
    test_artwork-library-controller
    test_artwork-library-workspace
    test_artwork-library-view
    test_artwork-library-thumbnail
    test_export-destination
    test_export-hints
    metafile-removal-cli-test
    test_metafile-removal
    vacards-dependencies-test
    vacards-workflow-test
    vacards-rust-target-test
    vacards-shared-metadata-test
    vacards-hash-evidence-test
    ${VACARDS_GATE_CONTRACT_TESTS}
    vacards-test-results-test
    vacards-packaging-inputs-test
    vacards-windows-cairo-policy-test
    vacards-cairo-replay-test
    vacards-versioning-test
    text-vertical-anchor-test
    test_boolop-attr
    test_bitmap-adjustment-chemistry
    test_bitmap-adjustments-controller
    test_bitmap-adjustment-preview
    test_bitmap-eraser
    test_destructive-bitmap-clip
    test_destructive-bitmap-coverage
    test_destructive-bitmap-clip-feedback
    test_dialog-notebook
    test_bitmap-histogram
    test_save-filename
    test_save-target
    test_font-catalog
    test_font-list-compact
    test_font-list-lifecycle
    test_xml-save-revision
    test_prepared-content-proof
    test_namedview-window-state
    test_desktop-document-binding
    test_desktop-origin-lifetime
    test_preferences-presenter
    test_overwrite-parent
    test_preferences-admission
    ${PREFERENCES_APP_BINDING_TESTS}
    test_document-file-operation
    test_document-file-transaction
    test_auto-save-recovery
    test_preferences
    test_sp-item
    test_object-set
    test_new-object-visibility
    test_export-formats
    test_clipped-bitmaps
    test_selector-interaction
    test_selection-rotation-feedback
    test_selector-percent-scale
    test_text-font-preview
    test_text-multi-style
    test_text-paragraph-target
    test_text-shape-none-editing
    # External text paste: the clipboard-free decoder suites always run and are
    # release-protected here. test_text-paste drives the display clipboard and is
    # opt-in (VACARDS_CLIP_ALLOW_LIVE=1; no INKSCAPE_TEST_GUI injection), and
    # vacards-clipboard-external is gated by VACARDS_CLIP_ALLOW=1, so neither is
    # mandatory in this inventory. Only vacards-clipboard-external carries the
    # "clipboard-external" label; test_text-paste carries RESOURCE_LOCK
    # os_clipboard + RUN_SERIAL but deliberately no label.
    test_text-paste-html
    test_text-paste-rtf
    test_text-paste-font
    test_text-paste-unit
    test_libnrtype_Layout-TNG-Compute
    test_libnrtype_Layout-TNG-Compute-lang
    bitmap-tone-test
    canvas-rendering-stats-test
    windows-rendering-test
    font-feature-utils-test
    libnrtype-font-factory-test
    libnrtype-font-instance-test
    libnrtype-font-utils-test
    path-path-boolop-test
    string-convert-test
    tiff-export-test
)

if(WITH_VACARDS_NESTING)
    list(APPEND VACARDS_CRITICAL_TESTS
        vacards-phase0-metadata-test
        test_nesting-document
        test_nesting-tool
        test_offset-shapes
        test_offset-shapes-tool
    )
endif()

if(TARGET test_nesting-sparrow)
    list(APPEND VACARDS_CRITICAL_TESTS test_nesting-sparrow)
endif()

foreach(testname IN LISTS VACARDS_CRITICAL_TESTS)
    if(NOT TEST ${testname})
        message(FATAL_ERROR "VACards critical test is not registered: ${testname}")
    endif()
    set_property(TEST ${testname} APPEND PROPERTY LABELS vacards-critical)
endforeach()

set(VACARDS_CRITICAL_TARGETS
    test_preview-runtime-policy
    test_font-discovery-subscription
    test_stroke-width-controller
    test_stroke-width-statusbar
    test_stroke-width-row
    test_spin-button-policies
    test_paint-order-query
    test_paint-order-widget
    test_welcome-recent-model
    test_welcome-drawing-preview
    test_preview-css-admission
    test_preview-svg-resource-policy
    test_preview-xml-input
    test_fillet-chamfer-edit
    test_corner-rounding-controller
    test_canvas-preview
    test_artwork-library-archive
    test_artwork-library-catalog
    test_artwork-library-document
    test_artwork-library-insert
    test_xml-attribute-batch
    test_artwork-library-lbart
    test_artwork-library-lightburn
    test_artwork-library-encoding
    test_artwork-library-manifest
    test_artwork-library-package
    test_artwork-library-storage
    test_artwork-library-svg-preflight
    test_artwork-library-svg-preflight-native
    test_artwork-library-host-quit
    test_artwork-library-host
    test_artwork-library-controller
    test_artwork-library-workspace
    test_artwork-library-view
    test_artwork-library-thumbnail
    test_export-destination
    test_export-hints
    make-test-rgb-profile
    test_metafile-removal
    vacards-cairo-replay-test
    test_boolop-attr
    test_bitmap-adjustment-chemistry
    test_bitmap-adjustments-controller
    test_operation-targets
    test_bitmap-adjustment-preview
    test_bitmap-eraser
    test_destructive-bitmap-clip
    test_destructive-bitmap-coverage
    test_destructive-bitmap-clip-feedback
    test_dialog-notebook
    test_bitmap-histogram
    test_save-filename
    test_save-target
    test_font-catalog
    test_font-list-compact
    test_font-list-lifecycle
    test_xml-save-revision
    test_prepared-content-proof
    test_namedview-window-state
    test_desktop-document-binding
    test_desktop-origin-lifetime
    test_preferences-presenter
    test_overwrite-parent
    test_preferences-admission
    test_preferences-app-binding
    test_document-file-operation
    test_document-file-transaction
    test_auto-save-recovery
    test_preferences
    test_sp-item
    test_object-set
    test_new-object-visibility
    test_export-formats
    test_clipped-bitmaps
    test_selector-interaction
    test_selection-rotation-feedback
    test_selector-percent-scale
    test_text-font-preview
    test_text-multi-style
    test_text-paragraph-target
    test_text-shape-none-editing
    test_text-paste-html
    test_text-paste-rtf
    test_text-paste-font
    test_text-paste-unit
    test_libnrtype_Layout-TNG-Compute
    test_libnrtype_Layout-TNG-Compute-lang
    bitmap-tone-test
    canvas-rendering-stats-test
    windows-rendering-test
    font-feature-utils-test
    libnrtype-font-factory-test
    libnrtype-font-instance-test
    libnrtype-font-utils-test
    path-path-boolop-test
    string-convert-test
    tiff-export-test
)

if(WITH_VACARDS_NESTING)
    list(APPEND VACARDS_CRITICAL_TARGETS
        test_nesting-document
        test_nesting-tool
        test_offset-shapes
        test_offset-shapes-tool
    )
endif()

if(TARGET test_nesting-sparrow)
    list(APPEND VACARDS_CRITICAL_TARGETS test_nesting-sparrow)
endif()

# vacards-critical test built by add_unit_test on macOS only.
if(TARGET macos-bundle-bootstrap-test)
    list(APPEND VACARDS_CRITICAL_TARGETS macos-bundle-bootstrap-test)
endif()

add_custom_target(build-vacards-critical
    DEPENDS inkscape ${VACARDS_CRITICAL_TARGETS}
)

# CTest does not rebuild executables. Keep the whole library test family in
# sync with inkscape_base after shared headers/layouts change, including tests
# outside the most recently edited component. Derive this from the maintained
# critical target inventory instead of introducing a second test-name list.
set(VACARDS_ARTWORK_LIBRARY_TARGETS ${VACARDS_CRITICAL_TARGETS})
list(FILTER VACARDS_ARTWORK_LIBRARY_TARGETS INCLUDE REGEX "^test_artwork-library-")
add_custom_target(build-artwork-library-tests
    DEPENDS inkscape ${VACARDS_ARTWORK_LIBRARY_TARGETS}
)

add_custom_target(check-vacards-critical
    COMMAND ${VACARDS_CTEST_COMMAND} --output-on-failure
            --label-regex vacards-critical
    DEPENDS build-vacards-critical
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

### Fuzz test
if(WITH_FUZZ)
# to use the fuzzer, make sure you use the right compiler (clang)
    # with the right flags -fsanitize=address -fsanitize-coverage=edge,trace-pc-guard,indirect-calls,trace-cmp,trace-div,trace-gep -fno-omit-frame-pointer
    # (see libfuzzer doc for info in flags)
    # first line is for integration into oss-fuzz https://github.com/google/oss-fuzz
    add_executable(fuzz fuzzer.cpp)
    if(LIB_FUZZING_ENGINE)
        target_link_libraries(fuzz inkscape_base -lFuzzingEngine)
    else()
        target_link_libraries(fuzz inkscape_base -lFuzzer)
    endif()
endif()

# Native controller and public entry-point development tests.
set_tests_properties(test_artwork-library-controller test_artwork-library-workspace
    PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
set_property(TEST test_artwork-library-controller test_artwork-library-workspace
    APPEND PROPERTY ENVIRONMENT "G_DEBUG=fatal-criticals")
set_property(TEST test_artwork-library-controller APPEND PROPERTY ENVIRONMENT
    "INKSCAPE_TEST_GUI=1"
    "INKSCAPE_FONTCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/rendering_tests/fonts/isolated.conf")

# Host lifecycle core uses synthetic collections and injected close decisions, no native UI.
set_tests_properties(test_artwork-library-host PROPERTIES RUN_SERIAL TRUE TIMEOUT 180)
set_property(TEST test_artwork-library-host APPEND PROPERTY ENVIRONMENT "G_DEBUG=fatal-criticals")
set_tests_properties(test_artwork-library-host-quit PROPERTIES RUN_SERIAL TRUE TIMEOUT 180)
set_property(TEST test_artwork-library-host-quit APPEND PROPERTY ENVIRONMENT
    "INKSCAPE_TEST_GUI=1" "G_DEBUG=fatal-criticals"
    "INKSCAPE_FONTCONFIG=${CMAKE_CURRENT_SOURCE_DIR}/rendering_tests/fonts/isolated.conf")

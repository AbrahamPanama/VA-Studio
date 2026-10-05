# SPDX-License-Identifier: GPL-2.0-or-later

add_library(vacards_nesting_cpp STATIC nesting-ffi.cpp)
target_compile_features(vacards_nesting_cpp PUBLIC cxx_std_20)
target_include_directories(vacards_nesting_cpp PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(vacards_nesting_cpp PUBLIC Vacards::NestingRust)
add_dependencies(vacards_nesting_cpp vacards_nesting_rust_build)
add_library(Vacards::NestingCpp ALIAS vacards_nesting_cpp)

if(TARGET inkscape_base)
    # Bundle only pinned native artifacts. Other platforms retain the existing
    # solver; never discover a helper through PATH or download one at runtime.
    if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
        set(_sparrow_platform darwin-arm64)
        set(_sparrow_hash_key sparrow_darwin_arm64_sha256)
        set(_sparrow_suffix "")
    elseif(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND
           CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64)$")
        set(_sparrow_platform windows-x64)
        set(_sparrow_hash_key sparrow_windows_x64_sha256)
        set(_sparrow_suffix .exe)
    endif()
    if(DEFINED _sparrow_platform)
        file(STRINGS "${CMAKE_SOURCE_DIR}/VACARDS-DEPENDENCIES.env" _sparrow_pin
             REGEX "^${_sparrow_hash_key}=")
        string(REPLACE "${_sparrow_hash_key}=" "" _sparrow_hash "${_sparrow_pin}")
        set(_sparrow_source "${CMAKE_SOURCE_DIR}/src/3rdparty/sparrow/bin/${_sparrow_platform}/sparrow${_sparrow_suffix}")
        file(SHA256 "${_sparrow_source}" _sparrow_actual)
        if(NOT _sparrow_hash STREQUAL _sparrow_actual)
            message(FATAL_ERROR "Pinned Sparrow artifact mismatch")
        endif()
        file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
        configure_file("${_sparrow_source}" "${CMAKE_BINARY_DIR}/bin/vacards-sparrow${_sparrow_suffix}" COPYONLY)
        install(PROGRAMS "${_sparrow_source}" DESTINATION bin RENAME "vacards-sparrow${_sparrow_suffix}"
                COMPONENT inkscape)
        install(FILES "${CMAKE_SOURCE_DIR}/src/3rdparty/sparrow/LICENSE"
                DESTINATION share/inkscape/sparrow COMPONENT inkscape)
        set_property(TARGET inkscape_base PROPERTY VACARDS_SPARROW_BUNDLED TRUE)
        set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/sparrow-adapter.cpp"
            TARGET_DIRECTORY inkscape_base PROPERTIES
            COMPILE_DEFINITIONS "VACARDS_SPARROW_SHA256=\"${_sparrow_hash}\"")
    endif()
    target_sources(inkscape_base PRIVATE
    nesting-cli-service.h
    nesting-cli-service.cpp

        sparrow-adapter.cpp
        sparrow-adapter.h
        nesting-document.cpp
        nesting-capture.h
        nesting-document.h
        nesting-settings.cpp
        nesting-settings.h)
    target_link_libraries(inkscape_base PRIVATE Vacards::NestingCpp)
endif()

if(VACARDS_NESTING_BUILD_PHASE2_TESTS)
    if(MSVC)
        target_compile_options(vacards_nesting_cpp PRIVATE /W4 /WX /permissive-)
    else()
        target_compile_options(vacards_nesting_cpp PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
    endif()

    add_executable(vacards_nesting_cpp_contract_test
        tests/nesting-ffi-test.cpp)
    target_compile_features(vacards_nesting_cpp_contract_test PRIVATE cxx_std_20)
    target_link_libraries(vacards_nesting_cpp_contract_test PRIVATE
        Vacards::NestingCpp)
    if(MSVC)
        target_compile_options(vacards_nesting_cpp_contract_test PRIVATE
            /W4 /WX /permissive-)
    else()
        target_compile_options(vacards_nesting_cpp_contract_test PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
    endif()

    if(BUILD_TESTING)
        add_test(NAME vacards-nesting-cpp-contract
                 COMMAND vacards_nesting_cpp_contract_test)
        add_test(NAME vacards-nesting-differential-selfcheck
            COMMAND "${CMAKE_COMMAND}"
                "-DBASELINE_RUNNER=$<TARGET_FILE:vacards_nesting_cpp_contract_test>"
                "-DEXPERIMENTAL_RUNNER=$<TARGET_FILE:vacards_nesting_cpp_contract_test>"
                "-DCOMPARATOR_RUNNER=$<TARGET_FILE:vacards_nesting_cpp_contract_test>"
                "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/differential-baseline-repeatability"
                -DBUDGET_MS=0
                -DBASELINE_WORKER_COUNT=1
                -DEXPERIMENTAL_WORKER_COUNT=1
                -DREQUIRE_IDENTICAL=ON
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run-nesting-differential.cmake")

        # These quality gates are registered but skipped unless their named
        # environment variable is exactly 1. This keeps ordinary CTest runs
        # deterministic and bounded while preserving one discoverable command
        # for activation evidence. Short budgets must escape an approved
        # adversarial trap. At 60 seconds the frozen baseline may already have
        # reached the same adversarial optimum, so that gate instead requires
        # at least one strict corpus improvement while still forbidding every
        # canonical-rank regression. The long test has a separate opt-in so
        # enabling the normal 100/1000/5000 ms suite cannot spend 20 minutes.
        function(_vacards_add_portfolio_differential_test budget_ms required_environment timeout_seconds
                 check_input_order require_strict_improvement require_strict_adversarial_improvement)
            set(_test_name "vacards-nesting-portfolio-differential-${budget_ms}ms")
            add_test(NAME "${_test_name}"
                COMMAND "${CMAKE_COMMAND}"
                    "-DBASELINE_RUNNER=$<TARGET_FILE:vacards_nesting_cpp_contract_test>"
                    "-DEXPERIMENTAL_RUNNER=$<TARGET_FILE:vacards_nesting_cpp_contract_test>"
                    "-DCOMPARATOR_RUNNER=$<TARGET_FILE:vacards_nesting_cpp_contract_test>"
                    "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/portfolio-differential-${budget_ms}ms"
                    "-DBUDGET_MS=${budget_ms}"
                    -DBASELINE_WORKER_COUNT=1
                    -DEXPERIMENTAL_WORKER_COUNT=2
                    "-DREQUIRE_STRICT_IMPROVEMENT=${require_strict_improvement}"
                    "-DREQUIRE_STRICT_ADVERSARIAL_IMPROVEMENT=${require_strict_adversarial_improvement}"
                    "-DCHECK_INPUT_ORDER_PERMUTATION=${check_input_order}"
                    "-DREQUIRED_ENVIRONMENT=${required_environment}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run-nesting-differential.cmake")
            set(_labels "vacards-nesting-differential;vacards-nesting-opt-in")
            if(budget_ms EQUAL 60000)
                list(APPEND _labels vacards-nesting-extended)
            endif()
            set_tests_properties("${_test_name}" PROPERTIES
                LABELS "${_labels}"
                TIMEOUT "${timeout_seconds}"
                SKIP_REGULAR_EXPRESSION "VACARDS_NESTING_DIFFERENTIAL_SKIPPED")
        endfunction()

        _vacards_add_portfolio_differential_test(
            100 VACARDS_NESTING_RUN_PORTFOLIO_DIFFERENTIAL 60 ON OFF ON)
        _vacards_add_portfolio_differential_test(
            1000 VACARDS_NESTING_RUN_PORTFOLIO_DIFFERENTIAL 120 OFF OFF ON)
        _vacards_add_portfolio_differential_test(
            5000 VACARDS_NESTING_RUN_PORTFOLIO_DIFFERENTIAL 300 OFF OFF ON)
        _vacards_add_portfolio_differential_test(
            60000 VACARDS_NESTING_RUN_PORTFOLIO_DIFFERENTIAL_60S 1800 OFF ON OFF)

        set_tests_properties(
            vacards-nesting-cpp-contract
            vacards-nesting-differential-selfcheck
            PROPERTIES
                LABELS vacards-nesting-phase2
                TIMEOUT 60)
    endif()

    if(TARGET check-vacards-nesting-phase0 AND
       TARGET vacards_nesting_job_contract_c)
        add_custom_target(check-vacards-nesting-phase2
            COMMAND "$<TARGET_FILE:vacards_nesting_job_contract_c>"
            COMMAND "$<TARGET_FILE:vacards_nesting_cpp_contract_test>"
            COMMAND "${CMAKE_COMMAND}"
                    "-DBASELINE_RUNNER=$<TARGET_FILE:vacards_nesting_cpp_contract_test>"
                    "-DEXPERIMENTAL_RUNNER=$<TARGET_FILE:vacards_nesting_cpp_contract_test>"
                    "-DCOMPARATOR_RUNNER=$<TARGET_FILE:vacards_nesting_cpp_contract_test>"
                    "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/differential-phase2"
                    -DBUDGET_MS=0
                    -DBASELINE_WORKER_COUNT=1
                    -DEXPERIMENTAL_WORKER_COUNT=1
                    -DREQUIRE_IDENTICAL=ON
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run-nesting-differential.cmake"
            DEPENDS
                check-vacards-nesting-phase0
                vacards_nesting_job_contract_c
                vacards_nesting_cpp_contract_test
            COMMENT "Running VACards nesting Phase 2 ABI and RAII gates"
            VERBATIM
            USES_TERMINAL)
    endif()
endif()

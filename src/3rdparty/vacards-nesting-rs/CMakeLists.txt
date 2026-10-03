# SPDX-License-Identifier: GPL-2.0-or-later

cmake_minimum_required(VERSION 3.24)

set(_vacards_nesting_standalone OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(vacards_nesting_bridge VERSION 0.1.0 LANGUAGES C CXX)
    include(CTest)
    set(_vacards_nesting_standalone ON)
endif()

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/rust-toolchain.toml"
     _vacards_rust_channel_line REGEX "^channel = \"[0-9]+\\.[0-9]+\\.[0-9]+\"$")
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+"
       VACARDS_NESTING_RUST_VERSION "${_vacards_rust_channel_line}")
if(NOT VACARDS_NESTING_RUST_VERSION)
    message(FATAL_ERROR "rust-toolchain.toml must pin one exact Rust version")
endif()

find_program(VACARDS_NESTING_CARGO_EXECUTABLE cargo
    HINTS "$ENV{CARGO_HOME}/bin"
    REQUIRED)
find_program(VACARDS_NESTING_RUSTC_EXECUTABLE rustc
    HINTS "$ENV{CARGO_HOME}/bin"
    REQUIRED)
find_program(VACARDS_NESTING_RUSTDOC_EXECUTABLE rustdoc
    HINTS "$ENV{CARGO_HOME}/bin"
    REQUIRED)

execute_process(
    COMMAND "${VACARDS_NESTING_RUSTC_EXECUTABLE}" --version --verbose
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE _vacards_rustc_status
    OUTPUT_VARIABLE _vacards_rustc_output
    ERROR_VARIABLE _vacards_rustc_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _vacards_rustc_status EQUAL 0)
    message(FATAL_ERROR
        "Unable to query rustc for VACards nesting: ${_vacards_rustc_error}")
endif()

string(REGEX MATCH "^rustc ([0-9]+\\.[0-9]+\\.[0-9]+)" _vacards_rustc_match
       "${_vacards_rustc_output}")
if(NOT CMAKE_MATCH_1 STREQUAL VACARDS_NESTING_RUST_VERSION)
    message(FATAL_ERROR
        "VACards nesting requires rustc ${VACARDS_NESTING_RUST_VERSION}; "
        "resolved '${_vacards_rustc_output}'")
endif()

string(REGEX MATCH "host: ([A-Za-z0-9_-]+)" _vacards_host_match "${_vacards_rustc_output}")
set(_vacards_rust_host "${CMAKE_MATCH_1}")
set(VACARDS_NESTING_RUST_TARGET "" CACHE STRING "Rust target matching the nesting C/C++ toolchain")
set(_vacards_compiler_family "${CMAKE_CXX_COMPILER_ID}")
if(MSVC)
    set(_vacards_compiler_family MSVC)
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/RustTarget.cmake")
vacards_nesting_rust_target("${_vacards_rust_host}" "${CMAKE_SYSTEM_NAME}"
    "${CMAKE_SYSTEM_PROCESSOR}" "${CMAKE_SIZEOF_VOID_P}" "${_vacards_compiler_family}"
    "${CMAKE_OSX_ARCHITECTURES}" "${VACARDS_NESTING_RUST_TARGET}" _vacards_rust_target)
set(VACARDS_NESTING_RUST_TARGET "${_vacards_rust_target}" CACHE STRING
    "Rust target matching the nesting C/C++ toolchain" FORCE)
execute_process(COMMAND "${VACARDS_NESTING_RUSTC_EXECUTABLE}"
    --print target-libdir --target "${VACARDS_NESTING_RUST_TARGET}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE _vacards_target_status OUTPUT_VARIABLE _vacards_target_libdir
    ERROR_VARIABLE _vacards_target_error OUTPUT_STRIP_TRAILING_WHITESPACE)
vacards_nesting_require_stdlib("${_vacards_target_status}" "${_vacards_target_libdir}"
    "${VACARDS_NESTING_RUST_TARGET}")
message(STATUS "VACards nesting Rust target: ${VACARDS_NESTING_RUST_TARGET} (host ${_vacards_rust_host})")

set(_vacards_cargo_target_dir "${CMAKE_CURRENT_BINARY_DIR}/cargo-target")
set(_vacards_cargo_home "${CMAKE_CURRENT_BINARY_DIR}/cargo-home")
set(_vacards_rust_environment
    "CARGO_HOME=${_vacards_cargo_home}"
    "CARGO_TARGET_DIR=${_vacards_cargo_target_dir}"
    "CARGO_NET_OFFLINE=true"
    "CARGO_BUILD_JOBS=2"
    "RUSTC=${VACARDS_NESTING_RUSTC_EXECUTABLE}"
    "RUSTDOC=${VACARDS_NESTING_RUSTDOC_EXECUTABLE}"
    "RUSTUP_TOOLCHAIN=${VACARDS_NESTING_RUST_VERSION}")
if(DEFINED ENV{RUSTUP_HOME} AND NOT "$ENV{RUSTUP_HOME}" STREQUAL "")
    list(APPEND _vacards_rust_environment "RUSTUP_HOME=$ENV{RUSTUP_HOME}")
endif()
if(WIN32 AND NOT MSVC)
    string(TOUPPER "${VACARDS_NESTING_RUST_TARGET}" _vacards_linker_target)
    string(REPLACE "-" "_" _vacards_linker_target "${_vacards_linker_target}")
    list(APPEND _vacards_rust_environment
        "CARGO_TARGET_${_vacards_linker_target}_LINKER=${CMAKE_C_COMPILER}")
endif()
if(APPLE AND CMAKE_OSX_DEPLOYMENT_TARGET)
    list(APPEND _vacards_rust_environment "MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif()
if(WIN32 AND MSVC)
    set(_vacards_rust_archive
        "${_vacards_cargo_target_dir}/${VACARDS_NESTING_RUST_TARGET}/release/vacards_nesting.lib")
else()
    set(_vacards_rust_archive
        "${_vacards_cargo_target_dir}/${VACARDS_NESTING_RUST_TARGET}/release/libvacards_nesting.a")
endif()

add_custom_target(vacards_nesting_rust_build ALL
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_vacards_cargo_home}"
    COMMAND "${CMAKE_COMMAND}" -E env
            ${_vacards_rust_environment}
            "${VACARDS_NESTING_CARGO_EXECUTABLE}"
            build --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
                  --release --locked --offline
                  --target "${VACARDS_NESTING_RUST_TARGET}"
    BYPRODUCTS "${_vacards_rust_archive}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Building pinned VACards nesting Rust static library offline"
    VERBATIM
    USES_TERMINAL)

add_library(vacards_nesting_rust STATIC IMPORTED GLOBAL)
set_target_properties(vacards_nesting_rust PROPERTIES
    IMPORTED_LOCATION "${_vacards_rust_archive}"
    INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/include")
add_dependencies(vacards_nesting_rust vacards_nesting_rust_build)
add_library(Vacards::NestingRust ALIAS vacards_nesting_rust)

if(APPLE)
    set_property(TARGET vacards_nesting_rust APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES "m")
elseif(UNIX)
    set_property(TARGET vacards_nesting_rust APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES "dl;pthread;m")
elseif(WIN32)
    set_property(TARGET vacards_nesting_rust APPEND PROPERTY
        INTERFACE_LINK_LIBRARIES
        "advapi32;ws2_32;userenv;bcrypt;ntdll;synchronization;psapi")
endif()

if(_vacards_nesting_standalone)
    set(_vacards_nesting_tests_default ON)
else()
    set(_vacards_nesting_tests_default OFF)
endif()
option(VACARDS_NESTING_BUILD_PHASE0_TESTS
       "Build the VACards nesting Rust and C ABI Phase 0 tests"
       ${_vacards_nesting_tests_default})
option(VACARDS_NESTING_BUILD_PHASE2_TESTS
       "Build the VACards nesting job ABI and C++ wrapper Phase 2 tests"
       ${_vacards_nesting_tests_default})

# In the complete Inkscape tree this subdirectory is configured before the
# historical top-level test harness calls include(CTest). Registering testing
# here prevents enabled ABI gates from silently disappearing from root CTest.
if(BUILD_TESTING AND
   (VACARDS_NESTING_BUILD_PHASE0_TESTS OR VACARDS_NESTING_BUILD_PHASE2_TESTS))
    enable_testing()
endif()

if(VACARDS_NESTING_BUILD_PHASE0_TESTS)
    add_executable(vacards_nesting_abi_smoke_c tests/abi_smoke.c)
    target_link_libraries(vacards_nesting_abi_smoke_c PRIVATE Vacards::NestingRust)
    add_dependencies(vacards_nesting_abi_smoke_c vacards_nesting_rust_build)

    add_executable(vacards_nesting_abi_smoke_cpp tests/abi_smoke.cpp)
    target_compile_features(vacards_nesting_abi_smoke_cpp PRIVATE cxx_std_20)
    target_link_libraries(vacards_nesting_abi_smoke_cpp PRIVATE Vacards::NestingRust)
    add_dependencies(vacards_nesting_abi_smoke_cpp vacards_nesting_rust_build)

    add_custom_target(check-vacards-nesting-phase0
        COMMAND "${CMAKE_COMMAND}"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                "-DDEPENDENCY_MANIFEST=${CMAKE_CURRENT_SOURCE_DIR}/../../../VACARDS-DEPENDENCIES.env"
                "-DRUSTC_EXECUTABLE=${VACARDS_NESTING_RUSTC_EXECUTABLE}"
                "-DRUST_ARCHIVE=${_vacards_rust_archive}"
                "-DOUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/VACARDS-NESTING-PROVENANCE.env"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyPhase0.cmake"
        COMMAND "${CMAKE_COMMAND}" -E env
                ${_vacards_rust_environment}
                "${VACARDS_NESTING_CARGO_EXECUTABLE}"
                test --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
                     --locked --offline
                     --target "${VACARDS_NESTING_RUST_TARGET}"
        COMMAND "$<TARGET_FILE:vacards_nesting_abi_smoke_c>"
        COMMAND "$<TARGET_FILE:vacards_nesting_abi_smoke_cpp>"
        DEPENDS vacards_nesting_abi_smoke_c vacards_nesting_abi_smoke_cpp
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Running VACards nesting Phase 0 offline and ABI gates"
        VERBATIM
        USES_TERMINAL)

    if(BUILD_TESTING)
        add_test(NAME vacards-nesting-rust-unit
                 COMMAND "${CMAKE_COMMAND}" -E env
                         ${_vacards_rust_environment}
                         "${VACARDS_NESTING_CARGO_EXECUTABLE}"
                         test --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
                              --locked --offline
                              --target "${VACARDS_NESTING_RUST_TARGET}")
        add_test(NAME vacards-nesting-randomized-feasibility
                 COMMAND "${CMAKE_COMMAND}" -E env
                         ${_vacards_rust_environment}
                         "${VACARDS_NESTING_CARGO_EXECUTABLE}"
                         test --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
                              --release --locked --offline
                              --target "${VACARDS_NESTING_RUST_TARGET}"
                              solver::tests::ten_thousand_randomized_rectangle_cases_remain_feasible
                              -- --exact --ignored)
        add_test(NAME vacards-nesting-abi-smoke-c
                 COMMAND vacards_nesting_abi_smoke_c)
        add_test(NAME vacards-nesting-abi-smoke-cpp
                 COMMAND vacards_nesting_abi_smoke_cpp)
        set_tests_properties(
            vacards-nesting-rust-unit
            vacards-nesting-abi-smoke-c
            vacards-nesting-abi-smoke-cpp
            PROPERTIES LABELS vacards-nesting-phase0)
        # cargo compiles the debug test binary inside this test, which takes
        # minutes on Windows; the default 180 s limit ended it there.
        set_tests_properties(vacards-nesting-rust-unit PROPERTIES
            TIMEOUT 900
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
        set_tests_properties(vacards-nesting-randomized-feasibility PROPERTIES
            LABELS vacards-nesting-extended
            TIMEOUT 300
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
endif()

if(VACARDS_NESTING_BUILD_PHASE2_TESTS)
    if(NOT VACARDS_NESTING_BUILD_PHASE0_TESTS)
        message(FATAL_ERROR
            "VACARDS_NESTING_BUILD_PHASE2_TESTS requires the Phase 0 gate")
    endif()
    add_executable(vacards_nesting_job_contract_c tests/job_contract.c)
    target_compile_features(vacards_nesting_job_contract_c PRIVATE c_std_11)
    target_link_libraries(vacards_nesting_job_contract_c PRIVATE Vacards::NestingRust)
    add_dependencies(vacards_nesting_job_contract_c vacards_nesting_rust_build)
    if(MSVC)
        target_compile_options(vacards_nesting_job_contract_c PRIVATE /W4 /WX)
    else()
        target_compile_options(vacards_nesting_job_contract_c PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
    endif()

    if(_vacards_nesting_standalone)
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../nesting"
                         "${CMAKE_CURRENT_BINARY_DIR}/nesting-cpp")
    endif()

    if(BUILD_TESTING)
        add_test(NAME vacards-nesting-job-contract-c
                 COMMAND vacards_nesting_job_contract_c)
        set_tests_properties(vacards-nesting-job-contract-c PROPERTIES
            LABELS vacards-nesting-phase2)
    endif()
endif()

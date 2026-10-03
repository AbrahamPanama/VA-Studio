# SPDX-License-Identifier: GPL-2.0-or-later

foreach(variable SOURCE_DIR DEPENDENCY_MANIFEST RUSTC_EXECUTABLE RUST_ARCHIVE OUTPUT_FILE)
    if(NOT DEFINED ${variable} OR "${${variable}}" STREQUAL "")
        message(FATAL_ERROR "VerifyPhase0.cmake requires ${variable}")
    endif()
endforeach()

set(jagua_version "0.8.0")
set(jagua_commit "9a19409bd38f3643c3d6d2d7571cddfba548ee17")
set(rust_version "1.88.0")
set(expected_license_sha256
    "1f256ecad192880510e84ad60474eab7589218784b9a50bc7ceee34c2b91f1d5")

function(require_literal variable_name literal description)
    string(FIND "${${variable_name}}" "${literal}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "Missing or divergent ${description}: ${literal}")
    endif()
endfunction()

foreach(path
        "${SOURCE_DIR}/Cargo.toml"
        "${SOURCE_DIR}/Cargo.lock"
        "${SOURCE_DIR}/rust-toolchain.toml"
        "${SOURCE_DIR}/.cargo/config.toml"
        "${SOURCE_DIR}/vendor/jagua-rs/.cargo-checksum.json"
        "${SOURCE_DIR}/LICENSES/jagua-rs-MPL-2.0.txt"
        "${SOURCE_DIR}/include/vacards_nesting.h"
        "${SOURCE_DIR}/src/job.rs"
        "${DEPENDENCY_MANIFEST}"
        "${RUST_ARCHIVE}")
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Missing VACards nesting Phase 0 input: ${path}")
    endif()
endforeach()

# Delegate all manifest parsing/validation to the canonical shared CLI. Keep
# this path anchored to the verifier, not the process cwd or manifest location.
find_program(vacards_sh NAMES sh REQUIRED)
get_filename_component(dependency_tool
    "${CMAKE_CURRENT_LIST_DIR}/../../../../packaging/vacards/vacards-dependencies.sh"
    ABSOLUTE)
function(dependency_value field output)
    execute_process(
        COMMAND "${vacards_sh}" "${dependency_tool}"
                --file "${DEPENDENCY_MANIFEST}" --get "${field}"
        RESULT_VARIABLE status OUTPUT_VARIABLE value ERROR_VARIABLE error
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT "${status}" STREQUAL "0")
        message(FATAL_ERROR "Invalid nesting dependency metadata: ${error}")
    endif()
    set(${output} "${value}" PARENT_SCOPE)
endfunction()

foreach(field_value
        "nesting_engine_repository|https://github.com/JeroenGar/jagua-rs.git"
        "nesting_engine_version|${jagua_version}"
        "nesting_engine_commit|${jagua_commit}"
        "nesting_rust_toolchain|${rust_version}")
    string(REPLACE "|" ";" pair "${field_value}")
    list(GET pair 0 field)
    list(GET pair 1 expected)
    dependency_value("${field}" actual)
    if(NOT "${actual}" STREQUAL "${expected}")
        message(FATAL_ERROR "Divergent ${field}: expected ${expected}, got ${actual}")
    endif()
endforeach()
dependency_value(nesting_ffi_api_version ffi_api_version)

# Validate both source declarations against the manifest. Runtime C/C++ tests
# in check-vacards-nesting-phase0 remain required; this is source coherence only.
file(STRINGS "${SOURCE_DIR}/include/vacards_nesting.h" header_abi
    REGEX "^[ \t]*#[ \t]*define[ \t]+VAC_NESTING_API_VERSION([ \t]|$)")
list(LENGTH header_abi header_count)
if(NOT header_count EQUAL 1 OR NOT header_abi MATCHES
        "^[ \t]*#[ \t]*define[ \t]+VAC_NESTING_API_VERSION[ \t]+([1-9][0-9]*)u?[ \t]*$")
    message(FATAL_ERROR "Expected one numeric VAC_NESTING_API_VERSION declaration")
endif()
if(NOT "${CMAKE_MATCH_1}" STREQUAL "${ffi_api_version}")
    message(FATAL_ERROR "Public header ABI differs from validated manifest ABI ${ffi_api_version}")
endif()
file(STRINGS "${SOURCE_DIR}/src/job.rs" rust_abi
    REGEX "^[ \t]*pub[ \t]+const[ \t]+API_VERSION[ \t]*:")
list(LENGTH rust_abi rust_count)
if(NOT rust_count EQUAL 1)
    message(FATAL_ERROR "Expected one numeric Rust API_VERSION declaration")
endif()
# file(STRINGS) returns a CMake list with the Rust semicolon escaped.
list(GET rust_abi 0 rust_abi)
if(NOT rust_abi MATCHES
        "^[ \t]*pub[ \t]+const[ \t]+API_VERSION[ \t]*:[ \t]*u32[ \t]*=[ \t]*([1-9][0-9]*)[ \t]*;$")
    message(FATAL_ERROR "Expected one numeric Rust API_VERSION declaration")
endif()
if(NOT "${CMAKE_MATCH_1}" STREQUAL "${ffi_api_version}")
    message(FATAL_ERROR "Rust ABI differs from validated manifest ABI ${ffi_api_version}")
endif()

file(READ "${SOURCE_DIR}/Cargo.toml" cargo_manifest)
require_literal(cargo_manifest "rev = \"${jagua_commit}\"" "Cargo jagua-rs pin")
require_literal(cargo_manifest "rust-version = \"1.88\"" "Cargo Rust version")

file(READ "${SOURCE_DIR}/Cargo.lock" cargo_lock)
require_literal(cargo_lock "name = \"jagua-rs\"" "locked jagua-rs package")
require_literal(cargo_lock "#${jagua_commit}" "locked jagua-rs revision")

file(READ "${SOURCE_DIR}/.cargo/config.toml" cargo_config)
require_literal(cargo_config "replace-with = \"vendored-sources\""
                "Cargo vendor replacement")
require_literal(cargo_config "offline = true" "Cargo offline policy")

file(SHA256 "${SOURCE_DIR}/LICENSES/jagua-rs-MPL-2.0.txt" license_sha256)
if(NOT license_sha256 STREQUAL expected_license_sha256)
    message(FATAL_ERROR
        "The vendored jagua-rs MPL-2.0 license differs from upstream")
endif()

execute_process(
    COMMAND "${RUSTC_EXECUTABLE}" --version
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE rustc_status
    OUTPUT_VARIABLE rustc_output
    ERROR_VARIABLE rustc_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT rustc_status EQUAL 0 OR NOT rustc_output MATCHES "^rustc ${rust_version} ")
    message(FATAL_ERROR
        "Expected rustc ${rust_version}; got '${rustc_output}' (${rustc_error})")
endif()

file(SHA256 "${SOURCE_DIR}/Cargo.lock" cargo_lock_sha256)
file(SHA256 "${RUST_ARCHIVE}" rust_archive_sha256)
file(WRITE "${OUTPUT_FILE}"
    "format=1\n"
    "nesting_ffi_api_version=${ffi_api_version}\n"
    "jagua_version=${jagua_version}\n"
    "jagua_commit=${jagua_commit}\n"
    "rustc_version=${rust_version}\n"
    "cargo_lock_sha256=${cargo_lock_sha256}\n"
    "rust_archive_sha256=${rust_archive_sha256}\n"
    "jagua_license_sha256=${license_sha256}\n")

message(STATUS "VACards nesting Phase 0 provenance: ${OUTPUT_FILE}")

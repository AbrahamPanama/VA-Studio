# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.24.0)

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW) # use FindBoost from boost 1.70+ (cmake 3.30+)
endif()

message("------------------------------")
message("Building Makefile for Inkscape")
message("------------------------------")
message("Source Dir: ${CMAKE_CURRENT_SOURCE_DIR}")
message("Binary Dir: ${CMAKE_CURRENT_BINARY_DIR}")

# -----------------------------------------------------------------------------
# CMake Configuration
# -----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# set(CMAKE_CXX_EXTENSIONS OFF) # enforces -std=c++20 instead of -std=gnu++20
                                # TODO: build currently fails with it as we actually depend on GNU compiler extensions...
                                #       mostly use of the non-Standard M_PI et al. TODO: C++20: Use the <numbers> header.

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/CMakeScripts/Modules")

# Build in Release mode by default.
set(CMAKE_BUILD_TYPE "Release" CACHE STRING
    "Choose the type of build, options are: None(CMAKE_CXX_FLAGS or CMAKE_C_FLAGS used) Debug Release RelWithDebInfo MinSizeRel Strict.")

project(inkscape)

include(CMakeScripts/HelperFunctions.cmake)
include(CMakeScripts/ConfigEnv.cmake)

set(CMAKE_INSTALL_DEFAULT_COMPONENT_NAME inkscape) # needs to be before any install() commands

include(GNUInstallDirs) # for the CMAKE_INSTALL_LIBDIR variable
include(CMakeScripts/ConfigPaths.cmake)
include(CMakeDependentOption)

set(PROJECT_NAME inkscape)

# see https://gitlab.com/inkscape/inkscape/-/issues/551 for versioning suffixes
set(INKSCAPE_VERSION_MAJOR  1)
set(INKSCAPE_VERSION_MINOR  5)
set(INKSCAPE_VERSION_PATCH  0)
set(INKSCAPE_VERSION_SUFFIX "-dev")

set(INKSCAPE_VERSION ${INKSCAPE_VERSION_MAJOR}.${INKSCAPE_VERSION_MINOR})
if(INKSCAPE_VERSION_PATCH)
    set(INKSCAPE_VERSION ${INKSCAPE_VERSION}.${INKSCAPE_VERSION_PATCH})
endif()
if(INKSCAPE_VERSION_SUFFIX)
    set(INKSCAPE_VERSION ${INKSCAPE_VERSION}${INKSCAPE_VERSION_SUFFIX})
endif()

set(CMAKE_INCLUDE_CURRENT_DIR TRUE)

# console output is slow as hell on Windows and as a result status messages of the "install" target slow down
# the whole build process considerably (especially since we also copy a lot of files from the devlibs)
# TODO: Is this worth to be configurable / also applicable to other platforms?
if(WIN32 AND NOT CMAKE_INSTALL_MESSAGE)
    set(CMAKE_INSTALL_MESSAGE "LAZY")
endif()

# Define a very strict set of build flags that will prevent any use of deprecated symbols.
# This will almost certainly cause compilation failure and is intended only for developer use.
set(CMAKE_CXX_FLAGS_STRICT "${CMAKE_CXX_FLAGS_DEBUG} -Werror=deprecated-declarations -DGTK_DISABLE_DEPRECATED -DGDK_DISABLE_DEPRECATED -DGTKMM_DISABLE_DEPRECATED -DGDKMM_DISABLE_DEPRECATED -DGLIBMM_DISABLE_DEPRECATED"
    CACHE STRING
    "Flags used by C++ compiler during Strict builds"
    FORCE)
set(CMAKE_C_FLAGS_STRICT   "${CMAKE_C_FLAGS_DEBUG}   -Werror=deprecated-declarations -DGTK_DISABLE_DEPRECATED -DGDK_DISABLE_DEPRECATED -DGTKMM_DISABLE_DEPRECATED -DGDKMM_DISABLE_DEPRECATED"
    CACHE STRING
    "Flags used by C compiler during Strict builds"
    FORCE)
mark_as_advanced(
    CMAKE_CXX_FLAGS_STRICT
    CMAKE_C_FLAGS_STRICT)

# -----------------------------------------------------------------------------
# Redirect output files
# -----------------------------------------------------------------------------
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_BINDIR} CACHE PATH "Output directory for runtime binaries")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_LIBDIR} CACHE PATH "Output directory for shared libraries")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/${CMAKE_INSTALL_LIBDIR} CACHE PATH "Output directory for static libraries")

# -----------------------------------------------------------------------------
# Options
# -----------------------------------------------------------------------------
option(WITH_GNU_READLINE "Compile with GNU Readline support (shell mode)" ON)
option(ENABLE_LCMS "Compile with LCMS support" ON)
option(WITH_SVG2 "Compile with support for new SVG2 features" ON)
option(WITH_LPETOOL "Compile with LPE Tool" OFF)
option(LPE_ENABLE_TEST_EFFECTS "Compile with test experimental LPEs enabled" OFF)
option(WITH_PROFILING "Turn on profiling" OFF) # Set to true if compiler/linker should enable profiling
option(BUILD_SHARED_LIBS "Compile libraries as shared and not static" ON)

option(WITH_POPPLER "Compile with support of libpoppler" ON)
option(WITH_CAPYPDF "Compile with support of libcapypdf" ON)
option(ENABLE_POPPLER_CAIRO "Compile with support of libpoppler-cairo for rendering PDF preview (depends on WITH_POPPLER)" ON)
option(WITH_IMAGE_MAGICK "Compile with support of ImageMagick for raster extensions and image import resolution (requires ImageMagick 6; set to OFF if you prefer GraphicsMagick)" ON)
option(WITH_GRAPHICS_MAGICK "Compile with support of GraphicsMagick for raster extensions and image import resolution" ON)
option(WITH_LIBCDR "Compile with support of libcdr for CorelDRAW Diagrams" ON)
option(VACARDS_REQUIRE_MODERN_CDR "Reject libcdr builds without VACards modern CorelDRAW support" ON)
option(VACARDS_REQUIRE_PATCHED_LIBREVENGE "Require the VACards patched librevenge with the versioned CDR fidelity extension" ON)
option(WITH_VACARDS_NESTING
       "Build the experimental VACards native nesting engine"
       OFF)
if(APPLE OR WIN32)
    option(VACARDS_REQUIRE_PATCHED_CAIRO "Require verified VACards Cairo clipping-fix provenance" ON)
else()
    option(VACARDS_REQUIRE_PATCHED_CAIRO "Require verified VACards Cairo clipping-fix provenance" OFF)
endif()
option(WITH_LIBVISIO "Compile with support of libvisio for Microsoft Visio Diagrams" ON)
option(WITH_LIBWPG "Compile with support of libwpg for WordPerfect Graphics" ON)
option(WITH_LIBSPELLING "Compile with support of libspelling" ON)
option(WITH_GSOURCEVIEW "Compile with support of gsourceview (text syntax coloring)" ON)
option(WITH_NLS "Compile with Native Language Support (using gettext)" ON)
option(WITH_JEMALLOC "Compile with JEMALLOC support" OFF)
option(WITH_ASAN "Compile with Clang's AddressSanitizer (for debugging purposes)" OFF)
option(WITH_INTERNAL_2GEOM "Prefer internal copy of lib2geom" OFF)
option(WITH_INTERNAL_DEPIXELIZE "Prefer internal copy of libdepixelize" OFF)
option(WITH_INTERNAL_AUTOTRACE "Prefer internal copy of autotrace" OFF)
option(WITH_INTERNAL_ADAPTAGRAMS "Prefer internal copy of adaptagrams" OFF)
option(WITH_INTERNAL_CAPYPDF "Prefer internal copy of capypdf" OFF)
option(WITH_CROSSINK "Cross-compiling with Crossroads" OFF)
if(WIN32)
    option(TESTS_WITH_ASAN "Compile unit tests with Clang's AddressSanitizer" OFF)
else()
    option(TESTS_WITH_ASAN "Compile unit tests with Clang's AddressSanitizer" ON)
endif()

option(WITH_FUZZ "Compile for fuzzing purpose (use 'make fuzz' only)" OFF)
mark_as_advanced(WITH_FUZZ)
option(WITH_MANPAGE_COMPRESSION "gzips manpages if gzip is available" ON)
mark_as_advanced(WITH_MANPAGE_COMPRESSION)

option(ENABLE_BINRELOC "Enable relocatable binaries" OFF)


# Product identity is independent of the upstream ABI/file-format version.
find_program(VACARDS_VERSION_SHELL NAMES sh REQUIRED)
foreach(field IN ITEMS name display release pe)
    execute_process(COMMAND "${VACARDS_VERSION_SHELL}"
        "${CMAKE_SOURCE_DIR}/packaging/vacards/vacards-version.sh" --${field}
        OUTPUT_VARIABLE VACARDS_PRODUCT_${field} OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _vacards_version_status)
    if(NOT _vacards_version_status EQUAL 0)
        message(FATAL_ERROR "Invalid VA Studio product version")
    endif()
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/packaging/vacards/VERSION.env"
    "${CMAKE_SOURCE_DIR}/packaging/vacards/vacards-version.sh")
# Owner-decided public identity (support URL, copyright holder) with neutral fallbacks.
include(CMakeScripts/VACardsPublicIdentity.cmake)

include(CMakeScripts/DefineDependsandFlags.cmake) # Includes, Compiler Flags, and Link Libraries

# Record and enforce the resolved feature set. CMake cache options describe
# what was requested, but several upstream probes shadow them when a dependency
# is unavailable. Releases need the effective result, not the stale request.
set(_vacards_dependency_manifest "${CMAKE_SOURCE_DIR}/VACARDS-DEPENDENCIES.env")
function(_vacards_manifest_feature_list key output)
    file(STRINGS "${_vacards_dependency_manifest}" lines REGEX "^${key}=[A-Z0-9_,]+$")
    list(LENGTH lines count)
    if(NOT count EQUAL 1)
        message(FATAL_ERROR "VACARDS-DEPENDENCIES.env must define one ${key}")
    endif()
    string(REGEX REPLACE "^[^=]+=" "" value "${lines}")
    string(REPLACE "," ";" value "${value}")
    set(${output} "${value}" PARENT_SCOPE)
endfunction()

_vacards_manifest_feature_list(required_cmake_features VACARDS_REQUIRED_CMAKE_FEATURES)
_vacards_manifest_feature_list(disabled_cmake_features VACARDS_DISABLED_CMAKE_FEATURES)
set(VACARDS_RESOLVED_FEATURES_FILE "${CMAKE_BINARY_DIR}/VACARDS-CMAKE-FEATURES.env")
file(WRITE "${VACARDS_RESOLVED_FEATURES_FILE}" "format=1\n")
foreach(feature IN LISTS VACARDS_REQUIRED_CMAKE_FEATURES)
    if(NOT DEFINED "${feature}" OR NOT "${${feature}}")
        message(FATAL_ERROR
            "VACards release capability ${feature} resolved OFF. Install its dependency "
            "instead of creating a reduced build.")
    endif()
    file(APPEND "${VACARDS_RESOLVED_FEATURES_FILE}" "${feature}=ON\n")
endforeach()
foreach(feature IN LISTS VACARDS_DISABLED_CMAKE_FEATURES)
    if(DEFINED "${feature}" AND "${${feature}}")
        message(FATAL_ERROR
            "VACards release policy requires ${feature}=OFF until it has pinned dependencies "
            "and regression coverage.")
    endif()
    file(APPEND "${VACARDS_RESOLVED_FEATURES_FILE}" "${feature}=OFF\n")
endforeach()

# Allow static libraries to be linked into shared libraries.
if(BUILD_SHARED_LIBS AND NOT WIN32)
    add_compile_options(-fPIC)
endif()



# -----------------------------------------------------------------------------
# Subdirectories
# -----------------------------------------------------------------------------
# Release builds set BUILD_TESTING explicitly. Enable CTest before traversing
# src/ so self-contained C, C++, and Rust contract tests registered there are
# visible from the root build instead of being silently omitted.
if(BUILD_TESTING)
    enable_testing()
endif()
add_subdirectory(src)

if(ENABLE_NLS)
    add_subdirectory(po)
endif()

if(NOT WIN32)
    include(CMakeScripts/Pod2man.cmake)
    add_subdirectory(man)
endif()



# -----------------------------------------------------------------------------
# Check License Headers
# -----------------------------------------------------------------------------
add_custom_target(check-license-headers WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} COMMAND ./buildtools/check_license_headers.py)


# -----------------------------------------------------------------------------
# Test Harness
# -----------------------------------------------------------------------------

if(BUILD_TESTING OR NOT DEFINED BUILD_TESTING)
    find_package(GTest)
    if(GTEST_FOUND)
        set(DART_TESTING_TIMEOUT 180 CACHE STRING "Test timeout") # if running for > 180 s something must be wrong)
        include(CTest)
        add_subdirectory(testfiles EXCLUDE_FROM_ALL)
    else()
        set(BUILD_TESTING OFF)
        message(WARNING "No gtest found! Disabling testing...\n"
                        "Consider installing it via package manager (install 'libgtest-dev' or equivalent).")
    endif()
endif()


# -----------------------------------------------------------------------------
# Clean Targets
# -----------------------------------------------------------------------------
add_custom_target(clean-cmake-files
    COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_SOURCE_DIR}/CMakeScripts/CleanAll.cmake"
)

add_custom_target(clean-all
    COMMAND ${CMAKE_BUILD_TOOL} clean
    COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_SOURCE_DIR}/CMakeScripts/CleanAll.cmake"
)

# -----------------------------------------------------------------------------
# Install Target
# -----------------------------------------------------------------------------
add_subdirectory(share)

if(WIN32)
  include(CMakeScripts/InstallMSYS2.cmake)
endif()

# -----------------------------------------------------------------------------
# Uninstall Target
# -----------------------------------------------------------------------------
if(WIN32)
    add_custom_target(uninstall
        "${CMAKE_COMMAND}" -E remove_directory "${CMAKE_INSTALL_PREFIX}")
else()
    configure_file(
        "${CMAKE_SOURCE_DIR}/CMakeScripts/cmake_uninstall.cmake.in"
        "${CMAKE_BINARY_DIR}/cmake_uninstall.cmake"
        @ONLY)
    add_custom_target(uninstall
        "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake")
endif()

# -----------------------------------------------------------------------------
# Dist Targets
# -----------------------------------------------------------------------------
include(CMakeScripts/Dist.cmake)

# -----------------------------------------------------------------------------
# Packaging (CPack)
# -----------------------------------------------------------------------------
include(CMakeScripts/ConfigCPack.cmake)



# ----------------------------------------------------------------------
# Information Summary
# ----------------------------------------------------------------------
message("------------------------------------------------------------------------")
message("Configuration Summary")
message("------------------------------------------------------------------------")
# project info
message("PROJECT_NAME:                ${PROJECT_NAME}")
message("INKSCAPE_VERSION:            ${INKSCAPE_VERSION}")
message("INKSCAPE_DIST_PREFIX:        ${INKSCAPE_DIST_PREFIX}")
message("")

# cmake info
message("CMAKE_BINARY_DIR:            ${CMAKE_BINARY_DIR}")
message("CMAKE_INSTALL_PREFIX:        ${CMAKE_INSTALL_PREFIX}")
message("CMAKE_INSTALL_LIBDIR:        ${CMAKE_INSTALL_LIBDIR}")
message("PACKAGE_LOCALE_DIR           ${PACKAGE_LOCALE_DIR}")
message("CMAKE_SYSTEM_NAME:           ${CMAKE_SYSTEM_NAME}")
message("CMAKE_SYSTEM_VERSION:        ${CMAKE_SYSTEM_VERSION}")
message("CMAKE_SYSTEM_PROCESSOR:      ${CMAKE_SYSTEM_PROCESSOR}")
message("CMAKE_C_COMPILER:            ${CMAKE_C_COMPILER}")
message("CMAKE_CXX_COMPILER:          ${CMAKE_CXX_COMPILER}")
message("CMAKE_BUILD_TYPE:            ${CMAKE_BUILD_TYPE}")
message("")

if(WIN32)
message("CMAKE_PREFIX_PATH:           ${CMAKE_PREFIX_PATH}")
message("CMAKE_FIND_LIBRARY_PREFIXES: ${CMAKE_FIND_LIBRARY_PREFIXES}")
message("CMAKE_FIND_LIBRARY_SUFFIXES: ${CMAKE_FIND_LIBRARY_SUFFIXES}")
message("")
endif()

# dependency info
message("ENABLE_LCMS:                 ${ENABLE_LCMS}")
message("WITH_POPPLER:                ${WITH_POPPLER}")
message("WITH_CAPYPDF:                ${WITH_CAPYPDF}")
message("ENABLE_POPPLER_CAIRO:        ${ENABLE_POPPLER_CAIRO}")
message("WITH_GNU_READLINE:           ${WITH_GNU_READLINE}")
message("WITH_LIBSPELLING:            ${WITH_LIBSPELLING}")
message("WITH_GSOURCEVIEW:            ${WITH_GSOURCEVIEW}")
message("WITH_IMAGE_MAGICK:           ${WITH_IMAGE_MAGICK}")
message("WITH_GRAPHICS_MAGICK:        ${WITH_GRAPHICS_MAGICK}")
message("WITH_LIBCDR:                 ${WITH_LIBCDR}")
message("WITH_VACARDS_NESTING:        ${WITH_VACARDS_NESTING}")
message("VACARDS_REQUIRE_MODERN_CDR:  ${VACARDS_REQUIRE_MODERN_CDR}")
message("VACARDS_REQUIRE_PATCHED_LIBREVENGE: ${VACARDS_REQUIRE_PATCHED_LIBREVENGE}")
message("VACARDS_LIBCDR_VERSION:      ${VACARDS_RESOLVED_LIBCDR_VERSION}")
message("VACARDS_LIBCDR_PREFIX:       ${VACARDS_RESOLVED_LIBCDR_PREFIX}")
message("VACARDS_LIBREVENGE_VERSION:  ${VACARDS_RESOLVED_LIBREVENGE_VERSION}")
message("VACARDS_LIBREVENGE_PREFIX:   ${VACARDS_RESOLVED_LIBREVENGE_PREFIX}")
message("VACARDS_LIBREVENGE_PATCHED:  ${VACARDS_LIBREVENGE_PATCHED}")
message("VACARDS_REQUIRE_PATCHED_CAIRO: ${VACARDS_REQUIRE_PATCHED_CAIRO}")
message("VACARDS_CAIRO_VERSION:       ${VACARDS_RESOLVED_CAIRO_VERSION}")
message("VACARDS_CAIRO_PREFIX:        ${VACARDS_RESOLVED_CAIRO_PREFIX}")
message("WITH_LIBVISIO:               ${WITH_LIBVISIO}")
message("WITH_LIBWPG:                 ${WITH_LIBWPG}")
message("WITH_NLS:                    ${WITH_NLS}")
message("WITH_JEMALLOC:               ${WITH_JEMALLOC}")
message("WITH_ASAN:                   ${WITH_ASAN}")
message("WITH_INTERNAL_2GEOM:         ${WITH_INTERNAL_2GEOM}")
message("WITH_INTERNAL_DEPIXELIZE:    ${WITH_INTERNAL_DEPIXELIZE}")
message("WITH_INTERNAL_AUTOTRACE:     ${WITH_INTERNAL_AUTOTRACE}")
message("WITH_INTERNAL_ADAPTAGRAMS:   ${WITH_INTERNAL_ADAPTAGRAMS}")
message("WITH_INTERNAL_CAPYPDF:       ${WITH_INTERNAL_CAPYPDF}")

message("WITH_PROFILING:              ${WITH_PROFILING}")
message("BUILD_TESTING:               ${BUILD_TESTING}")
message("TESTS_WITH_ASAN:             ${TESTS_WITH_ASAN}")

if(WIN32)
message("")
message("HAVE_MINGW64:                ${HAVE_MINGW64}")
message("MINGW_PATH:                  ${MINGW_PATH}")
message("MINGW_ARCH:                  ${MINGW_ARCH}")
endif()

message("------------------------------------------------------------------------")

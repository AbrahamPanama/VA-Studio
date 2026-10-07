if(WITH_INTERNAL_2GEOM)
    set(2GEOM_BUILD_SHARED ${BUILD_SHARED_LIBS} CACHE BOOL "")
    include(FetchContent)
    FetchContent_Declare(2Geom SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/2geom" OVERRIDE_FIND_PACKAGE)
    FetchContent_MakeAvailable(2Geom)

    # The clipping recursion counters are mutable function statics reached by parallel geometry work.
    # Compile a generated copy with thread_local counters; each top-level call still resets its counter.
    # Thus serial semantics are unchanged, while the upstream submodule remains untouched.
    set(_va_clipping_source "${CMAKE_CURRENT_SOURCE_DIR}/2geom/src/2geom/bezier-clipping.cpp")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_va_clipping_source}")
    file(READ "${_va_clipping_source}" _va_clipping_contents)
    string(REGEX MATCHALL "static size_t counter = 0" _va_counter_matches "${_va_clipping_contents}")
    list(LENGTH _va_counter_matches _va_counter_count)
    if(NOT _va_counter_count EQUAL 2)
        message(FATAL_ERROR "Expected exactly 2 clipping counters in ${_va_clipping_source}; found ${_va_counter_count}")
    endif()
    message(STATUS "Thread-local clipping copy: exactly ${_va_counter_count} replacements in ${_va_clipping_source}")
    string(REPLACE "static size_t counter = 0;" "static thread_local size_t counter = 0;"
           _va_clipping_generated "${_va_clipping_contents}")
    set(_va_clipping_generated_path "${CMAKE_CURRENT_BINARY_DIR}/2geom-va/bezier-clipping.cpp")
    if(NOT EXISTS "${_va_clipping_generated_path}")
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/2geom-va")
        file(WRITE "${_va_clipping_generated_path}" "${_va_clipping_generated}")
    else()
        file(READ "${_va_clipping_generated_path}" _va_clipping_existing)
        if(NOT _va_clipping_existing STREQUAL _va_clipping_generated)
            file(WRITE "${_va_clipping_generated_path}" "${_va_clipping_generated}")
        endif()
    endif()
    get_target_property(_va_2geom_sources 2geom SOURCES)
    set(_va_2geom_replaced_sources)
    set(_va_2geom_clipping_matches 0)
    foreach(_va_source IN LISTS _va_2geom_sources)
        get_filename_component(_va_source_name "${_va_source}" NAME)
        if(_va_source_name STREQUAL "bezier-clipping.cpp")
            math(EXPR _va_2geom_clipping_matches "${_va_2geom_clipping_matches} + 1")
            list(APPEND _va_2geom_replaced_sources "${_va_clipping_generated_path}")
        else()
            list(APPEND _va_2geom_replaced_sources "${_va_source}")
        endif()
    endforeach()
    if(NOT _va_2geom_clipping_matches EQUAL 1)
        message(FATAL_ERROR "Expected exactly 1 bezier-clipping.cpp source in target 2geom; found ${_va_2geom_clipping_matches}")
    endif()
    set_property(TARGET 2geom PROPERTY SOURCES "${_va_2geom_replaced_sources}")
    target_compile_definitions(inkscape_base PRIVATE VA_2GEOM_THREAD_LOCAL_CLIPPING=1)
endif()

add_subdirectory(libcroco)

if(WITH_INTERNAL_DEPIXELIZE)
    add_subdirectory(libdepixelize)
endif()

if(WITH_INTERNAL_ADAPTAGRAMS)
    add_subdirectory(adaptagrams)
endif()

if(WITH_INTERNAL_AUTOTRACE)
    add_subdirectory(autotrace)
endif()

if(WITH_VACARDS_NESTING)
    add_subdirectory(vacards-nesting-rs)
endif()

if(WITH_CAPYPDF AND WITH_INTERNAL_CAPYPDF)
    add_library(capypdf_LIB STATIC)
    set_property(TARGET capypdf_LIB PROPERTY CXX_STANDARD 23)

    find_package(PNG REQUIRED)
    find_package(JPEG REQUIRED)
    find_package(ZLIB REQUIRED)
    find_package(TIFF REQUIRED)
    find_package(Freetype REQUIRED)
    pkg_check_modules(CAPY_DEPS REQUIRED IMPORTED_TARGET lcms2)
    target_link_libraries(capypdf_LIB PUBLIC PNG::PNG JPEG::JPEG ZLIB::ZLIB TIFF::TIFF Freetype::Freetype PkgConfig::CAPY_DEPS)

    set(SRC capypdf/src)
    target_sources(capypdf_LIB PRIVATE
        ${SRC}/pdfcommon.cpp
        ${SRC}/generator.cpp
        ${SRC}/drawcontext.cpp
        ${SRC}/document.cpp
        ${SRC}/imagefileops.cpp
        ${SRC}/utils.cpp
        ${SRC}/colorconverter.cpp
        ${SRC}/fontsubsetter.cpp
        ${SRC}/ft_subsetter.cpp
        ${SRC}/cffsubsetter.cpp
        ${SRC}/bitfiddling.cpp
        ${SRC}/capi.cpp
        ${SRC}/errorhandling.cpp
        ${SRC}/pdfwriter.cpp
        ${SRC}/objectformatter.cpp
        ${SRC}/commandstreamformatter.cpp
        ${SRC}/mmapper.cpp
        ${SRC}/xmlformatter.cpp
    )

    set(CAPYPDF_VERSION_MAJOR 0)
    set(CAPYPDF_VERSION_MINOR 21)
    set(CAPYPDF_VERSION_MICRO 0)

    string(CONCAT CAPYPDF_HEADER_REGEX
        "s/^#mesondefine CAPYPDF_VERSION_STR$$/#define CAPYPDF_VERSION_STR \\\"${CAPYPDF_VERSION_MAJOR}.${CAPYPDF_VERSION_MINOR}.${CAPYPDF_VERSION_MICRO}\\\"/;"
        "s/^#mesondefine CAPYPDF_VERSION_MAJOR$$/#define CAPYPDF_VERSION_MAJOR ${CAPYPDF_VERSION_MAJOR}/;"
        "s/^#mesondefine CAPYPDF_VERSION_MINOR$$/#define CAPYPDF_VERSION_MINOR ${CAPYPDF_VERSION_MINOR}/;"
        "s/^#mesondefine CAPYPDF_VERSION_MICRO$$/#define CAPYPDF_VERSION_MICRO ${CAPYPDF_VERSION_MICRO}/;"
        "s/^#mesondefine CAPYPDF_STATIC$$/#define CAPYPDF_STATIC 1/"
    )

    add_custom_target(capypdf_HEADER
        COMMAND sed "'${CAPYPDF_HEADER_REGEX}'" "${CMAKE_CURRENT_SOURCE_DIR}/capypdf/include/capypdf.h.in" > "${CMAKE_CURRENT_BINARY_DIR}/capypdf.h"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/capypdf/include/capypdf.h.in"
    )

    add_dependencies(capypdf_LIB capypdf_HEADER)

    target_include_directories(capypdf_LIB
        PRIVATE
            capypdf/src
        PUBLIC
            capypdf/include # for capypdf.hpp
            ${CMAKE_CURRENT_BINARY_DIR} # for generated capypdf.h
    )

    add_library(Inkscape::CapyPDF ALIAS capypdf_LIB)
endif()

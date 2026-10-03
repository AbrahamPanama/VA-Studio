cmake_minimum_required(VERSION 3.12)

project(libcroco LANGUAGES C)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(LIBCROCO_STANDALONE TRUE)
endif()

set(LIBCROCO_MAJOR_VERSION 0)
set(LIBCROCO_MINOR_VERSION 6)
set(LIBCROCO_MICRO_VERSION 99)
set(LIBCROCO_VERSION ${LIBCROCO_MAJOR_VERSION}.${LIBCROCO_MINOR_VERSION}.${LIBCROCO_MICRO_VERSION})
math(EXPR LIBCROCO_VERSION_NUMBER "${LIBCROCO_MAJOR_VERSION} * 10000 +
                                   ${LIBCROCO_MINOR_VERSION} * 100 +
                                   ${LIBCROCO_MICRO_VERSION}")

find_package(PkgConfig REQUIRED)

set(GLIB_REQUIRED 2.0)
set(LIBXML_REQUIRED 2.4.23)

pkg_check_modules(GLIB REQUIRED glib-2.0>=${GLIB_REQUIRED})
pkg_check_modules(LIBXML REQUIRED libxml-2.0>=${LIBXML_REQUIRED})

add_subdirectory(src)

option(LIBCROCO_TESTING "Build tests" "${LIBCROCO_STANDALONE}")

if(LIBCROCO_TESTING)
    enable_testing()
    add_subdirectory(csslint)
    add_subdirectory(tests)
endif()

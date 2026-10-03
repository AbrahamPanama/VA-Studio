# SPDX-License-Identifier: GPL-2.0-or-later
#
# VA Studio public identity (owner decisions in
# packaging/public-release/public-release.env).
#
# Empty values are legitimate while the decisions are pending. Consumers then
# get neutral fallbacks, never an upstream or placeholder address:
#   - VACARDS_PUBLIC_SUPPORT_URL (C++, generated vacards-public-identity-config.h):
#     empty, so src/vacards-public-identity.h points at the installed SUPPORT.md;
#   - VACARDS_COPYRIGHT_HOLDER_DISPLAY (Windows version resource): "the VA Studio
#     authors";
#   - VA_<KEY> (documents installed by share/doc): "(not yet published)".
# check-public-release.py, not the build, blocks a public release while a value
# is missing. Invalid characters fail the configure step.

set(VACARDS_PUBLIC_RELEASE_ENV "${CMAKE_SOURCE_DIR}/packaging/public-release/public-release.env")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${VACARDS_PUBLIC_RELEASE_ENV}")

function(_vacards_public_value key pattern output)
    set(value "")
    if(EXISTS "${VACARDS_PUBLIC_RELEASE_ENV}")
        file(STRINGS "${VACARDS_PUBLIC_RELEASE_ENV}" lines REGEX "^${key}=")
        list(LENGTH lines count)
        if(count GREATER 1)
            message(FATAL_ERROR "public-release.env defines ${key} more than once")
        elseif(count EQUAL 1)
            string(REGEX REPLACE "^${key}=" "" value "${lines}")
            string(STRIP "${value}" value)
        endif()
    endif()
    if(NOT value STREQUAL "" AND NOT value MATCHES "${pattern}")
        message(FATAL_ERROR "public-release.env: ${key} has characters that are not allowed: '${value}'")
    endif()
    set(${output} "${value}" PARENT_SCOPE)
endfunction()

set(_vacards_url_pattern "^https://[A-Za-z0-9.-]+(:[0-9]+)?(/[A-Za-z0-9._~%/+-]*)?$")
_vacards_public_value(VA_PUBLIC_PRODUCT_NAME "^[A-Za-z0-9][A-Za-z0-9 .+-]*$" _vacards_product)
_vacards_public_value(VA_COPYRIGHT_HOLDER "^[A-Za-z0-9][A-Za-z0-9 .,&()+'-]*$" _vacards_holder)
_vacards_public_value(VA_SUPPORT_URL "${_vacards_url_pattern}" _vacards_support)
_vacards_public_value(VA_SOURCE_URL "${_vacards_url_pattern}" _vacards_source)
_vacards_public_value(VA_PUBLIC_REPO_URL "${_vacards_url_pattern}" _vacards_repo)
_vacards_public_value(VA_FORKS_BASE_URL "${_vacards_url_pattern}" _vacards_forks)
string(LENGTH "${_vacards_holder}" _vacards_holder_length)
if(_vacards_holder_length GREATER 100)
    message(FATAL_ERROR "public-release.env: VA_COPYRIGHT_HOLDER is longer than 100 characters")
endif()

# C++: the single "report a problem" destination (src/vacards-public-identity.h).
set(VACARDS_PUBLIC_SUPPORT_URL "${_vacards_support}")
configure_file("${CMAKE_SOURCE_DIR}/src/vacards-public-identity-config.h.in"
               "${CMAKE_BINARY_DIR}/include/vacards-public-identity-config.h" @ONLY)

# Windows version resource (src/inkscape.rc): ASCII only, validated above.
if(_vacards_holder STREQUAL "")
    set(VACARDS_COPYRIGHT_HOLDER_DISPLAY "the VA Studio authors")
else()
    set(VACARDS_COPYRIGHT_HOLDER_DISPLAY "${_vacards_holder}")
endif()

# Installed documents (share/doc/CMakeLists.txt configures their @VA_<KEY>@).
set(_vacards_unset "(not yet published)")
foreach(_pair IN ITEMS "VA_PUBLIC_PRODUCT_NAME;_vacards_product" "VA_COPYRIGHT_HOLDER;_vacards_holder"
                       "VA_SUPPORT_URL;_vacards_support" "VA_SOURCE_URL;_vacards_source"
                       "VA_PUBLIC_REPO_URL;_vacards_repo" "VA_FORKS_BASE_URL;_vacards_forks")
    list(GET _pair 0 _name)
    list(GET _pair 1 _variable)
    if("${${_variable}}" STREQUAL "")
        set(${_name} "${_vacards_unset}")
    else()
        set(${_name} "${${_variable}}")
    endif()
endforeach()
if(_vacards_product STREQUAL "")
    set(VA_PUBLIC_PRODUCT_NAME "VA Studio")
endif()

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
#   - documents installed by share/doc: each <!--VA_<KEY>-->...<!--/VA_<KEY>-->
#     span keeps its neutral text (vacards_configure_public_document below).
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

# Installed documents. Owner values appear in the public documents as spans
#   <!--VA_<KEY>-->neutral text<!--/VA_<KEY>-->
# (HTML comments, so the neutral text is what GitHub shows). The installed copy
# gets the owner's value when it is set and the neutral text otherwise; no raw
# span or placeholder is ever installed. The neutral text must not contain "<".
set(VACARDS_PUBLIC_KEYS VA_PUBLIC_PRODUCT_NAME VA_COPYRIGHT_HOLDER VA_SUPPORT_URL VA_SOURCE_URL
    VA_PUBLIC_REPO_URL VA_FORKS_BASE_URL)
set(VACARDS_VALUE_VA_PUBLIC_PRODUCT_NAME "${_vacards_product}")
set(VACARDS_VALUE_VA_COPYRIGHT_HOLDER "${_vacards_holder}")
set(VACARDS_VALUE_VA_SUPPORT_URL "${_vacards_support}")
set(VACARDS_VALUE_VA_SOURCE_URL "${_vacards_source}")
set(VACARDS_VALUE_VA_PUBLIC_REPO_URL "${_vacards_repo}")
set(VACARDS_VALUE_VA_FORKS_BASE_URL "${_vacards_forks}")

function(vacards_configure_public_document input output)
    file(READ "${input}" text)
    foreach(key IN LISTS VACARDS_PUBLIC_KEYS)
        if("${VACARDS_VALUE_${key}}" STREQUAL "")
            string(REGEX REPLACE "<!--${key}-->([^<]*)<!--/${key}-->" "\\1" text "${text}")
        else()
            string(REGEX REPLACE "<!--${key}-->[^<]*<!--/${key}-->" "${VACARDS_VALUE_${key}}" text "${text}")
        endif()
    endforeach()
    if(text MATCHES "<!--/?VA_[A-Z_]+-->" OR text MATCHES "@VA_[A-Z_]+@")
        message(FATAL_ERROR "${input}: unresolved owner placeholder")
    endif()
    set(previous "")
    if(EXISTS "${output}")
        file(READ "${output}" previous)
    endif()
    if(NOT previous STREQUAL text)
        file(WRITE "${output}" "${text}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${input}")
endfunction()

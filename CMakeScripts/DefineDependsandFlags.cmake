set(INKSCAPE_LIBS "")
set(INKSCAPE_CXX_FLAGS "")
set(INKSCAPE_CXX_FLAGS_DEBUG "")

include_directories(
    ${PROJECT_SOURCE_DIR}/src

    # generated includes
    ${CMAKE_BINARY_DIR}/include
)

# NDEBUG implies G_DISABLE_ASSERT
string(TOUPPER ${CMAKE_BUILD_TYPE} CMAKE_BUILD_TYPE_UPPER)
if(CMAKE_CXX_FLAGS_${CMAKE_BUILD_TYPE_UPPER} MATCHES "-DNDEBUG")
    list(APPEND INKSCAPE_CXX_FLAGS "-DG_DISABLE_ASSERT")
endif()

# AddressSanitizer
# Clang's AddressSanitizer can detect more memory errors and is more powerful
# than compiling with _FORTIFY_SOURCE but has a performance impact (approx. 2x
# slower), so it's not suitable for release builds.
if(WITH_ASAN)
    list(APPEND INKSCAPE_CXX_FLAGS "-fsanitize=address -fno-omit-frame-pointer")
    list(APPEND INKSCAPE_LIBS "-fsanitize=address")
elseif(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    # Undefine first, to suppress 'warning: "_FORTIFY_SOURCE" redefined'
    list(APPEND INKSCAPE_CXX_FLAGS "-U_FORTIFY_SOURCE")
    list(APPEND INKSCAPE_CXX_FLAGS "-D_FORTIFY_SOURCE=2")
endif()

# Disable deprecated Gtk and friends
#list(APPEND INKSCAPE_CXX_FLAGS "-DGLIBMM_DISABLE_DEPRECATED")
#list(APPEND INKSCAPE_CXX_FLAGS "-DGTKMM_DISABLE_DEPRECATED")
#list(APPEND INKSCAPE_CXX_FLAGS "-DGDKMM_DISABLE_DEPRECATED")
#list(APPEND INKSCAPE_CXX_FLAGS "-DGTK_DISABLE_DEPRECATED")
#list(APPEND INKSCAPE_CXX_FLAGS "-DGDK_DISABLE_DEPRECATED")

# Disable deprecation warnings for Gtk
list(APPEND INKSCAPE_CXX_FLAGS "-DGDK_DISABLE_DEPRECATION_WARNINGS")

# Errors for common mistakes
list(APPEND INKSCAPE_CXX_FLAGS "-fstack-protector-strong")
list(APPEND INKSCAPE_CXX_FLAGS "-Werror=format")                # e.g.: printf("%s", std::string("foo"))
list(APPEND INKSCAPE_CXX_FLAGS "-Werror=format-security")       # e.g.: printf(variable);
list(APPEND INKSCAPE_CXX_FLAGS "-Werror=ignored-qualifiers")    # e.g.: const int foo();
list(APPEND INKSCAPE_CXX_FLAGS "-Werror=return-type")           # non-void functions that don't return a value
list(APPEND INKSCAPE_CXX_FLAGS "-Werror=vla")                   # variable-length arrays
list(APPEND INKSCAPE_CXX_FLAGS "-Wno-switch")                   # See !849 for discussion
list(APPEND INKSCAPE_CXX_FLAGS "-Wmisleading-indentation")
list(APPEND INKSCAPE_CXX_FLAGS_DEBUG "-Wcomment")
list(APPEND INKSCAPE_CXX_FLAGS_DEBUG "-Wunused-function")
list(APPEND INKSCAPE_CXX_FLAGS_DEBUG "-Wunused-variable")
list(APPEND INKSCAPE_CXX_FLAGS_DEBUG "-D_GLIBCXX_ASSERTIONS")
if (CMAKE_COMPILER_IS_GNUCC)
    list(APPEND INKSCAPE_CXX_FLAGS "-Wstrict-null-sentinel")    # For NULL instead of nullptr
    list(APPEND INKSCAPE_CXX_FLAGS_DEBUG "-fexceptions -grecord-gcc-switches -fasynchronous-unwind-tables")
    if(CXX_COMPILER_VERSION VERSION_GREATER 8.0)
        list(APPEND INKSCAPE_CXX_FLAGS_DEBUG "-fstack-clash-protection -fcf-protection")
    endif()
endif()
if(APPLE)
    list(APPEND INKSCAPE_CXX_FLAGS "-fexperimental-library") # for jthread, stop_token
endif()

# Define the flags for profiling if desired:
if(WITH_PROFILING)
    set(BUILD_SHARED_LIBS off)
    SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -pg")
    SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -pg")
endif()

include(CheckCXXSourceCompiles)
CHECK_CXX_SOURCE_COMPILES("
#include <atomic>
#include <cstdint>
std::atomic<uint64_t> x (0);
int main() {
  uint64_t i = x.load(std::memory_order_relaxed);
  return 0;
}
"
LIBATOMIC_NOT_NEEDED)
IF (NOT LIBATOMIC_NOT_NEEDED)
    message(STATUS "  Adding -latomic to the libs.")
    list(APPEND INKSCAPE_LIBS "-latomic")
ENDIF()


# ----------------------------------------------------------------------------
# Files we include
# ----------------------------------------------------------------------------
if(WIN32)
    # Set the link and include directories
    get_property(dirs DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY INCLUDE_DIRECTORIES)

    list(APPEND INKSCAPE_LIBS "-lmscms")
    list(APPEND INKSCAPE_LIBS "-ldwmapi")

    list(APPEND INKSCAPE_CXX_FLAGS "-mms-bitfields")
    if(${CMAKE_CXX_COMPILER_ID} STREQUAL "GNU")
      list(APPEND INKSCAPE_CXX_FLAGS "-mwindows")
      list(APPEND INKSCAPE_CXX_FLAGS "-mthreads")
    endif()

    list(APPEND INKSCAPE_LIBS "-lwinpthread")

    if(HAVE_MINGW64)
        list(APPEND INKSCAPE_CXX_FLAGS "-m64")
    else()
        list(APPEND INKSCAPE_CXX_FLAGS "-m32")
    endif()

    # Fixes for windows.h and GTK4
    add_definitions(-DNOGDI)
    add_definitions(-D_NO_W32_PSEUDO_MODIFIERS)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(INKSCAPE_DEP REQUIRED IMPORTED_TARGET
                  harfbuzz>=2.6.5
                  pangocairo>=1.50
                  pangoft2
                  fontconfig
                  gmodule-2.0
                  bdw-gc #boehm-demers-weiser gc
                  lcms2)

list(APPEND INKSCAPE_LIBS PkgConfig::INKSCAPE_DEP)

if(WITH_JEMALLOC)
    find_package(JeMalloc)
    if (JEMALLOC_FOUND)
        list(APPEND INKSCAPE_LIBS ${JEMALLOC_LIBRARIES})
    else()
        set(WITH_JEMALLOC OFF)
    endif()
endif()

pkg_search_module(ICU_UC REQUIRED IMPORTED_TARGET icu-uc)
list(APPEND INKSCAPE_LIBS PkgConfig::ICU_UC)

find_package(Iconv REQUIRED)
list(APPEND INKSCAPE_LIBS Iconv::Iconv)

find_package(Intl REQUIRED)
list(APPEND INKSCAPE_LIBS Intl::Intl)

find_package(GSL REQUIRED)
list(APPEND INKSCAPE_LIBS GSL::gsl)

find_package(double-conversion CONFIG REQUIRED)
list(APPEND INKSCAPE_LIBS double-conversion::double-conversion)

# Check for system-wide version of 2geom and fallback to internal copy if not found
if(NOT WITH_INTERNAL_2GEOM)
    find_package(2Geom ${INKSCAPE_VERSION_MAJOR}.${INKSCAPE_VERSION_MINOR} CONFIG QUIET)
    if(NOT TARGET 2Geom::2geom)
        set(WITH_INTERNAL_2GEOM ON CACHE BOOL "Prefer internal copy of lib2geom" FORCE)
        message(STATUS "lib2geom not found, using internal copy in src/3rdparty/2geom")
    endif()
endif()

if(NOT WITH_INTERNAL_DEPIXELIZE)
    find_package(Depixelize 0.1.0 CONFIG QUIET)
    if(NOT TARGET Depixelize::depixelize)
        set(WITH_INTERNAL_DEPIXELIZE ON CACHE BOOL "Prefer internal copy of libdepixelize" FORCE)
    endif()
endif()

if(NOT WITH_INTERNAL_AUTOTRACE)
    pkg_check_modules(AUTOTRACE QUIET IMPORTED_TARGET autotrace>=0.31.10)
    if(AUTOTRACE_FOUND)
        add_library(autotrace_LIB ALIAS PkgConfig::AUTOTRACE)
    else()
        set(WITH_INTERNAL_AUTOTRACE ON CACHE BOOL "Prefer internal copy of autotrace" FORCE)
    endif()
endif()

if(NOT WITH_INTERNAL_ADAPTAGRAMS)
    pkg_check_modules(ADAPTAGRAMS QUIET IMPORTED_TARGET libavoid>=0.1 libcola>=0.1 libvpsc>=0.1)
    if(ADAPTAGRAMS_FOUND)
        add_library(adaptagrams_LIB ALIAS PkgConfig::ADAPTAGRAMS)
    else()
        set(WITH_INTERNAL_ADAPTAGRAMS ON CACHE BOOL "Prefer internal copy of adaptagrams" FORCE)
    endif()
endif()

if(WITH_CAPYPDF)
    if(NOT WITH_INTERNAL_CAPYPDF)
        pkg_check_modules(CAPYPDF QUIET IMPORTED_TARGET capypdf>=0.21)
        if(CAPYPDF_FOUND)
            add_library(Inkscape::CapyPDF ALIAS PkgConfig::CAPYPDF)
        else()
            message(STATUS "CapyPDF not found, using internal copy in src/3rdparty/capypdf")
            set(WITH_INTERNAL_CAPYPDF ON CACHE BOOL "Prefer internal copy of capypdf" FORCE)
        endif()
    endif()
    add_definitions(-DWITH_CAPYPDF)
endif()

if(WITH_POPPLER)
    pkg_check_modules(POPPLER IMPORTED_TARGET poppler>=0.20.0 poppler-glib>=0.20.0)
    if(POPPLER_FOUND)
        list(APPEND INKSCAPE_LIBS PkgConfig::POPPLER)
        set(HAVE_POPPLER_CAIRO ${ENABLE_POPPLER_CAIRO})
        add_definitions(-DWITH_POPPLER)
    else()
        set(WITH_POPPLER OFF)
        set(ENABLE_POPPLER_CAIRO OFF)
    endif()
else()
    set(ENABLE_POPPLER_CAIRO OFF)
endif()

if(WITH_LIBWPG)
    pkg_check_modules(LIBWPG IMPORTED_TARGET libwpg-0.3 librevenge-0.0 librevenge-stream-0.0)
    if(LIBWPG_FOUND)
        list(APPEND INKSCAPE_LIBS PkgConfig::LIBWPG)
    else()
        set(WITH_LIBWPG OFF)
    endif()
endif()

if(WITH_LIBVISIO)
    pkg_check_modules(LIBVISIO IMPORTED_TARGET libvisio-0.1 librevenge-0.0 librevenge-stream-0.0)
    if(LIBVISIO_FOUND)
        list(APPEND INKSCAPE_LIBS PkgConfig::LIBVISIO)
    else()
        set(WITH_LIBVISIO OFF)
    endif()
endif()

if(WITH_LIBCDR)
    set(_vacards_dependency_manifest "${CMAKE_SOURCE_DIR}/VACARDS-DEPENDENCIES.env")
    file(STRINGS "${_vacards_dependency_manifest}" _vacards_libcdr_version_line
         REGEX "^libcdr_pkgconfig_version=[0-9]+\\.[0-9]+\\.[0-9]+$")
    list(LENGTH _vacards_libcdr_version_line _vacards_libcdr_version_count)
    if(NOT _vacards_libcdr_version_count EQUAL 1)
        message(FATAL_ERROR "VACARDS-DEPENDENCIES.env must define one libcdr_pkgconfig_version")
    endif()
    string(REGEX REPLACE "^[^=]+=" "" VACARDS_REQUIRED_LIBCDR_VERSION
           "${_vacards_libcdr_version_line}")

    if(VACARDS_REQUIRE_MODERN_CDR)
        pkg_check_modules(LIBCDR IMPORTED_TARGET
            "libcdr-0.1>=${VACARDS_REQUIRED_LIBCDR_VERSION}"
            librevenge-0.0 librevenge-stream-0.0)
    else()
        pkg_check_modules(LIBCDR IMPORTED_TARGET
            libcdr-0.1 librevenge-0.0 librevenge-stream-0.0)
    endif()
    if(LIBCDR_FOUND)
        pkg_get_variable(VACARDS_RESOLVED_LIBCDR_PREFIX libcdr-0.1 prefix)
        execute_process(
            COMMAND "${PKG_CONFIG_EXECUTABLE}" --modversion libcdr-0.1
            OUTPUT_VARIABLE VACARDS_RESOLVED_LIBCDR_VERSION
            OUTPUT_STRIP_TRAILING_WHITESPACE
            COMMAND_ERROR_IS_FATAL ANY)
        set(VACARDS_RESOLVED_LIBCDR_PREFIX "${VACARDS_RESOLVED_LIBCDR_PREFIX}"
            CACHE PATH "Resolved libcdr prefix used by this build" FORCE)
        set(VACARDS_RESOLVED_LIBCDR_VERSION "${VACARDS_RESOLVED_LIBCDR_VERSION}"
            CACHE STRING "Resolved libcdr version used by this build" FORCE)
        mark_as_advanced(VACARDS_RESOLVED_LIBCDR_PREFIX VACARDS_RESOLVED_LIBCDR_VERSION)
        message(STATUS "VACards libcdr: ${VACARDS_RESOLVED_LIBCDR_VERSION} from ${VACARDS_RESOLVED_LIBCDR_PREFIX}")
        list(APPEND INKSCAPE_LIBS PkgConfig::LIBCDR)

        # BEGIN VACARDS LIBREVENGE CDR-FIDELITY CAPABILITY
        # The versioned CDR crop/text extension lives in the patched librevenge
        # generator (see packaging/dependencies/librevenge-0.0.6). A stock 0.0.6
        # library has the same version and pkg-config name, so version alone is
        # not evidence. Detect the VACards marker and, when the build requires
        # the capability, fail closed through the platform verifier.
        pkg_get_variable(VACARDS_RESOLVED_LIBREVENGE_PREFIX librevenge-0.0 prefix)
        execute_process(
            COMMAND "${PKG_CONFIG_EXECUTABLE}" --modversion librevenge-0.0
            OUTPUT_VARIABLE VACARDS_RESOLVED_LIBREVENGE_VERSION
            OUTPUT_STRIP_TRAILING_WHITESPACE
            COMMAND_ERROR_IS_FATAL ANY)
        set(VACARDS_RESOLVED_LIBREVENGE_PREFIX "${VACARDS_RESOLVED_LIBREVENGE_PREFIX}"
            CACHE PATH "Resolved librevenge prefix used by this build" FORCE)
        set(VACARDS_RESOLVED_LIBREVENGE_VERSION "${VACARDS_RESOLVED_LIBREVENGE_VERSION}"
            CACHE STRING "Resolved librevenge version used by this build" FORCE)
        mark_as_advanced(VACARDS_RESOLVED_LIBREVENGE_PREFIX VACARDS_RESOLVED_LIBREVENGE_VERSION)
        if(EXISTS "${VACARDS_RESOLVED_LIBREVENGE_PREFIX}/VACARDS-LIBREVENGE.env")
            set(VACARDS_LIBREVENGE_PATCHED ON CACHE INTERNAL
                "Resolved librevenge carries the VACards patch marker" FORCE)
        else()
            set(VACARDS_LIBREVENGE_PATCHED OFF CACHE INTERNAL
                "Resolved librevenge carries the VACards patch marker" FORCE)
        endif()
        message(STATUS "VACards librevenge: ${VACARDS_RESOLVED_LIBREVENGE_VERSION} from ${VACARDS_RESOLVED_LIBREVENGE_PREFIX}")
        if(VACARDS_REQUIRE_PATCHED_LIBREVENGE)
            if(NOT VACARDS_LIBREVENGE_PATCHED)
                message(FATAL_ERROR
                    "VACARDS_REQUIRE_PATCHED_LIBREVENGE=ON but the resolved librevenge "
                    "(${VACARDS_RESOLVED_LIBREVENGE_VERSION} at ${VACARDS_RESOLVED_LIBREVENGE_PREFIX}) "
                    "is not the patched VACards prefix. Build it with "
                    "packaging/macos/vacards/build-patched-librevenge.sh (macOS) or "
                    "packaging/windows/vacards/build-patched-librevenge.sh (Windows) and put its "
                    "install/lib/pkgconfig directory first in PKG_CONFIG_PATH. "
                    "A stock librevenge 0.0.6 does not provide the versioned CDR fidelity extension.")
            endif()
            if(WIN32)
                find_program(VACARDS_LIBREVENGE_BASH_EXECUTABLE bash REQUIRED)
                set(_vacards_librevenge_verifier
                    "${CMAKE_SOURCE_DIR}/packaging/windows/vacards/verify-vacards-librevenge.sh")
                set(_vacards_librevenge_shell "${VACARDS_LIBREVENGE_BASH_EXECUTABLE}")
            else()
                find_program(VACARDS_LIBREVENGE_SH_EXECUTABLE sh REQUIRED)
                set(_vacards_librevenge_verifier
                    "${CMAKE_SOURCE_DIR}/packaging/macos/vacards/verify-vacards-librevenge.sh")
                set(_vacards_librevenge_shell "${VACARDS_LIBREVENGE_SH_EXECUTABLE}")
            endif()
            execute_process(
                COMMAND "${CMAKE_COMMAND}" -E env "PKG_CONFIG=${PKG_CONFIG_EXECUTABLE}"
                    "${_vacards_librevenge_shell}" "${_vacards_librevenge_verifier}"
                    "${VACARDS_RESOLVED_LIBREVENGE_PREFIX}"
                RESULT_VARIABLE _vacards_librevenge_status
                OUTPUT_VARIABLE _vacards_librevenge_output
                ERROR_VARIABLE _vacards_librevenge_error)
            if(NOT _vacards_librevenge_status EQUAL 0)
                message(FATAL_ERROR
                    "VACards patched librevenge verification failed: "
                    "${_vacards_librevenge_error}\n${_vacards_librevenge_output}")
            endif()
            message(STATUS "${_vacards_librevenge_output}")
        elseif(NOT VACARDS_LIBREVENGE_PATCHED)
            message(STATUS
                "VACards CDR fidelity capability: NOT ACTIVE. The resolved librevenge "
                "${VACARDS_RESOLVED_LIBREVENGE_VERSION} at ${VACARDS_RESOLVED_LIBREVENGE_PREFIX} "
                "is not the patched VACards prefix, so the versioned CDR crop/text extension "
                "is unavailable. Set VACARDS_REQUIRE_PATCHED_LIBREVENGE=ON after building the "
                "private prefix to make a missing capability a configure error.")
        endif()
        # END VACARDS LIBREVENGE CDR-FIDELITY CAPABILITY
    elseif(VACARDS_REQUIRE_MODERN_CDR)
        message(FATAL_ERROR
            "VACards requires libcdr >= ${VACARDS_REQUIRED_LIBCDR_VERSION}. "
            "Build the pinned dependency with packaging/macos/vacards/build-vacards-libcdr.sh "
            "and add its install/lib/pkgconfig directory to PKG_CONFIG_PATH.")
    else()
        set(WITH_LIBCDR OFF)
    endif()
endif()

find_package(JPEG)
if(JPEG_FOUND)
    list(APPEND INKSCAPE_LIBS JPEG::JPEG)
    set(HAVE_JPEG ON)
endif()

find_package(PNG REQUIRED)
list(APPEND INKSCAPE_LIBS PNG::PNG)

# Native RGB TIFF export is a release feature. Keeping libtiff in the main
# dependency graph also lets the macOS bundler discover and package it.
find_package(TIFF REQUIRED)
list(APPEND INKSCAPE_LIBS TIFF::TIFF)

find_package(Potrace REQUIRED)
list(APPEND INKSCAPE_LIBS Potrace::Potrace)

if(WITH_SVG2)
    add_definitions(-DWITH_MESH -DWITH_CSSBLEND -DWITH_SVG2)
else()
    add_definitions(-UWITH_MESH -UWITH_CSSBLEND -UWITH_SVG2)
endif()

# ----------------------------------------------------------------------------
# CMake's builtin
# ----------------------------------------------------------------------------

# Include dependencies:

pkg_check_modules(
    MM REQUIRED IMPORTED_TARGET
    cairomm-1.16
    pangomm-2.48
    gdk-pixbuf-2.0
    graphene-1.0
)
list(APPEND INKSCAPE_LIBS PkgConfig::MM)

pkg_get_variable(VACARDS_RESOLVED_CAIRO_PREFIX cairo prefix)
execute_process(
    COMMAND "${PKG_CONFIG_EXECUTABLE}" --modversion cairo
    OUTPUT_VARIABLE VACARDS_RESOLVED_CAIRO_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
set(VACARDS_RESOLVED_CAIRO_PREFIX "${VACARDS_RESOLVED_CAIRO_PREFIX}"
    CACHE PATH "Resolved Cairo prefix used by this build" FORCE)
set(VACARDS_RESOLVED_CAIRO_VERSION "${VACARDS_RESOLVED_CAIRO_VERSION}"
    CACHE STRING "Resolved Cairo version used by this build" FORCE)
mark_as_advanced(VACARDS_RESOLVED_CAIRO_PREFIX VACARDS_RESOLVED_CAIRO_VERSION)
message(STATUS "VACards Cairo: ${VACARDS_RESOLVED_CAIRO_VERSION} from ${VACARDS_RESOLVED_CAIRO_PREFIX}")

# BEGIN VACARDS WINDOWS CAIRO PREFIX CHECK
if(WIN32 AND VACARDS_REQUIRE_PATCHED_CAIRO)
    if(NOT MINGW OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "Patched Windows Cairo currently requires x64 MSYS2 UCRT64/MinGW.")
    endif()
    find_program(VACARDS_CAIRO_BASH_EXECUTABLE bash REQUIRED)
    find_program(VACARDS_CAIRO_CYGPATH_EXECUTABLE cygpath REQUIRED)
    # pkg_get_variable can split a prefix containing spaces into a CMake list.
    # Read one literal path and decode pkgconf's optional escaped spaces only.
    # MSYS tools emit UTF-8; CMake's legacy Windows AUTO decoding corrupts
    # non-ASCII prefixes before they reach the verifier.
    execute_process(
        COMMAND "${PKG_CONFIG_EXECUTABLE}" --variable=prefix cairo
        ENCODING UTF-8
        OUTPUT_VARIABLE _vacards_windows_cairo_prefix
        OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    if(_vacards_windows_cairo_prefix STREQUAL "")
        message(FATAL_ERROR "Windows Cairo pkg-config prefix is empty.")
    endif()
    string(REPLACE "\\ " " " _vacards_windows_cairo_prefix "${_vacards_windows_cairo_prefix}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "LC_ALL=C.UTF-8" "${VACARDS_CAIRO_CYGPATH_EXECUTABLE}" -m "${_vacards_windows_cairo_prefix}"
        ENCODING UTF-8
        OUTPUT_VARIABLE VACARDS_RESOLVED_CAIRO_PREFIX
        OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    set(VACARDS_RESOLVED_CAIRO_PREFIX "${VACARDS_RESOLVED_CAIRO_PREFIX}"
        CACHE PATH "Resolved Cairo prefix used by this build" FORCE)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "PKG_CONFIG=${PKG_CONFIG_EXECUTABLE}"
            "${VACARDS_CAIRO_BASH_EXECUTABLE}"
            "${CMAKE_SOURCE_DIR}/packaging/windows/vacards/verify-vacards-cairo-prefix.sh"
            "${VACARDS_RESOLVED_CAIRO_PREFIX}"
        ENCODING UTF-8
        RESULT_VARIABLE _vacards_windows_cairo_status
        OUTPUT_VARIABLE _vacards_windows_cairo_output
        ERROR_VARIABLE _vacards_windows_cairo_error)
    if(NOT _vacards_windows_cairo_status EQUAL 0)
        message(FATAL_ERROR
            "Windows Cairo provenance verification failed: ${_vacards_windows_cairo_error}\n"
            "Build the pinned dependency with packaging/windows/vacards/build-patched-cairo.sh; "
            "put its install/lib/pkgconfig first in PKG_CONFIG_PATH. "
            "Stock Cairo 1.18.4 or VACARDS_REQUIRE_PATCHED_CAIRO=OFF is not fix evidence.")
    endif()
    message(STATUS "${_vacards_windows_cairo_output}")
endif()
# END VACARDS WINDOWS CAIRO PREFIX CHECK

if(APPLE AND VACARDS_REQUIRE_PATCHED_CAIRO)
    set(_vacards_cairo_marker "${VACARDS_RESOLVED_CAIRO_PREFIX}/VACARDS-CAIRO.txt")
    if(NOT EXISTS "${_vacards_cairo_marker}")
        message(FATAL_ERROR
            "The resolved Cairo at ${VACARDS_RESOLVED_CAIRO_PREFIX} is not the patched VACards build. "
            "Build it with packaging/macos/vacards/build-patched-cairo.sh and put its "
            "install/lib/pkgconfig directory first in PKG_CONFIG_PATH.")
    endif()
endif()

# FindPkgConfig resolves every library in a transitive module independently.
# On Homebrew, gtk4/gtkmm can therefore resolve their cairo entries from
# /opt/homebrew even when cairo.pc itself came from the patched VACards prefix.
# Linking both paths into one process defeats the runtime fix and can reproduce
# the macOS zoom/render crash. Rewrite only Cairo entries on the imported
# targets; all other libraries continue to come from their normal providers.
function(vacards_bind_patched_cairo target_name)
    if(NOT APPLE OR NOT VACARDS_REQUIRE_PATCHED_CAIRO OR NOT TARGET "${target_name}")
        return()
    endif()

    get_target_property(_vacards_links "${target_name}" INTERFACE_LINK_LIBRARIES)
    if(NOT _vacards_links OR _vacards_links STREQUAL "_vacards_links-NOTFOUND")
        return()
    endif()

    set(_vacards_rewritten_links "")
    set(_vacards_rewrote_cairo OFF)
    foreach(_vacards_link IN LISTS _vacards_links)
        if(_vacards_link MATCHES "/libcairo-gobject(\\.[0-9]+)*\\.dylib$")
            set(_vacards_link
                "${VACARDS_RESOLVED_CAIRO_PREFIX}/lib/libcairo-gobject.dylib")
            set(_vacards_rewrote_cairo ON)
        elseif(_vacards_link MATCHES "/libcairo(\\.[0-9]+)*\\.dylib$")
            set(_vacards_link "${VACARDS_RESOLVED_CAIRO_PREFIX}/lib/libcairo.dylib")
            set(_vacards_rewrote_cairo ON)
        endif()
        list(APPEND _vacards_rewritten_links "${_vacards_link}")
    endforeach()

    if(_vacards_rewrote_cairo)
        foreach(_vacards_required_library IN ITEMS
                "${VACARDS_RESOLVED_CAIRO_PREFIX}/lib/libcairo.dylib"
                "${VACARDS_RESOLVED_CAIRO_PREFIX}/lib/libcairo-gobject.dylib")
            if(NOT EXISTS "${_vacards_required_library}")
                message(FATAL_ERROR
                    "Patched VACards Cairo library is missing: ${_vacards_required_library}")
            endif()
        endforeach()
        set_property(TARGET "${target_name}" PROPERTY
            INTERFACE_LINK_LIBRARIES "${_vacards_rewritten_links}")
        message(STATUS "Bound ${target_name} to the patched VACards Cairo runtime")
    endif()
endfunction()

# if system's gtk is new enough for gtkmm, pick it, otherwise ignore it and gtkmm build will build it
pkg_check_modules(GTK IMPORTED_TARGET gtk4>=4.14.0)
if(GTK_FOUND)
    list(APPEND INKSCAPE_LIBS PkgConfig::GTK)
endif()

pkg_check_modules(GLIBMM IMPORTED_TARGET glibmm-2.68>=2.78.1 giomm-2.68)
if(GLIBMM_FOUND)
    add_library(GLibmm::GLibmm ALIAS PkgConfig::GLIBMM)
else()
    message("GLIBMM too old, glibmm 2.78.1 will be compiled from source")
    include(ExternalProject)
    ExternalProject_Add(glibmm
        URL https://download.gnome.org/sources/glibmm/2.78/glibmm-2.78.1.tar.xz
        URL_HASH SHA256=f473f2975d26c3409e112ed11ed36406fb3843fa975df575c22d4cb843085f61
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        CONFIGURE_COMMAND meson setup --libdir lib . ../glibmm --prefix=${CMAKE_CURRENT_BINARY_DIR}/deps
        BUILD_COMMAND meson compile
        INSTALL_COMMAND meson install
    )
    add_library(GLibmm_LIB INTERFACE)
    target_include_directories(GLibmm_LIB INTERFACE ${CMAKE_CURRENT_BINARY_DIR}/deps/include/glibmm-2.68/ ${CMAKE_CURRENT_BINARY_DIR}/deps/lib/glibmm-2.68/include ${CMAKE_CURRENT_BINARY_DIR}/deps/include/giomm-2.68/ ${CMAKE_CURRENT_BINARY_DIR}/deps/lib/giomm-2.68/include)
    target_link_directories(GLibmm_LIB INTERFACE ${CMAKE_CURRENT_BINARY_DIR}/deps/lib)
    target_link_libraries(GLibmm_LIB INTERFACE -lglibmm-2.68)
    add_library(GLibmm::GLibmm ALIAS GLibmm_LIB)
endif()

pkg_check_modules(GTKMM IMPORTED_TARGET gtkmm-4.0>=4.13.3)
if(GTKMM_FOUND)
    add_library(GTKmm::GTKmm ALIAS PkgConfig::GTKMM)
else()
    message("GTKMM too old, gtkmm 4.14.0 will be compiled from source")
    include(ExternalProject)
    ExternalProject_Add(gtkmm
        URL https://download.gnome.org/sources/gtkmm/4.14/gtkmm-4.14.0.tar.xz
        URL_HASH SHA256=9350a0444b744ca3dc69586ebd1b6707520922b6d9f4f232103ce603a271ecda
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        CONFIGURE_COMMAND meson setup --libdir lib . ../gtkmm --prefix=${CMAKE_CURRENT_BINARY_DIR}/deps
        BUILD_COMMAND meson compile
        INSTALL_COMMAND meson install
    )
    add_library(GTKmm_LIB INTERFACE)
    target_include_directories(GTKmm_LIB INTERFACE ${CMAKE_CURRENT_BINARY_DIR}/deps/include/gtkmm-4.0 ${CMAKE_CURRENT_BINARY_DIR}/deps/lib/gtkmm-4.0/include ${CMAKE_CURRENT_BINARY_DIR}/deps/include/gtk-4.0/)
    target_link_directories(GTKmm_LIB INTERFACE ${CMAKE_CURRENT_BINARY_DIR}/deps/lib)
    target_link_libraries(GTKmm_LIB INTERFACE -lgtkmm-4.0 PkgConfig::MM)
    add_library(GTKmm::GTKmm ALIAS GTKmm_LIB)
endif()
if(NOT (GTKMM_FOUND AND GLIBMM_FOUND))
    list(APPEND CMAKE_INSTALL_RPATH "${CMAKE_BINARY_DIR}/deps/lib")

    # check we can actually build it
    message("To build gtkmm4, you need the packages glslc, mm-common, and libgstreamer-plugins-bad1.0-dev")

    find_program(glslc glslc REQUIRED)
    find_program(mmcp mm-common-prepare REQUIRED)
    pkg_check_modules(TMP-gtkmm-gstreamer gstreamer-player-1.0 REQUIRED)
endif()
list(APPEND INKSCAPE_LIBS GLibmm::GLibmm GTKmm::GTKmm)

if(WITH_LIBSPELLING)
    pkg_check_modules(LIBSPELLING IMPORTED_TARGET libspelling-1)
    if("${LIBSPELLING_FOUND}")
        message(STATUS "Using libspelling")
        list(APPEND INKSCAPE_LIBS PkgConfig::LIBSPELLING)
    else()
        set(WITH_LIBSPELLING OFF)
    endif()
endif()

if(WITH_GSOURCEVIEW)
    pkg_check_modules(GSOURCEVIEW IMPORTED_TARGET gtksourceview-5)
    if("${GSOURCEVIEW_FOUND}")
        message(STATUS "Using gtksourceview-5")
        list(APPEND INKSCAPE_LIBS PkgConfig::GSOURCEVIEW)
    else()
        set(WITH_GSOURCEVIEW OFF)
    endif()
endif()

if(APPLE AND VACARDS_REQUIRE_PATCHED_CAIRO)
    foreach(_vacards_pkgconfig_target IN ITEMS
            PkgConfig::INKSCAPE_DEP
            PkgConfig::POPPLER
            PkgConfig::MM
            PkgConfig::GTK
            PkgConfig::GTKMM
            PkgConfig::LIBSPELLING
            PkgConfig::GSOURCEVIEW)
        vacards_bind_patched_cairo("${_vacards_pkgconfig_target}")
    endforeach()
endif()

# BEGIN VACARDS WINDOWS CAIRO LINK CHECK
if(WIN32 AND VACARDS_REQUIRE_PATCHED_CAIRO)
    # FindPkgConfig may resolve transitive GTK/Pango Cairo imports from the
    # system prefix even when cairo.pc is custom. Bind every Cairo import to
    # the verified prefix, preserving the unrelated dependency entries.
    foreach(_vacards_windows_target IN ITEMS
            PkgConfig::INKSCAPE_DEP PkgConfig::POPPLER PkgConfig::MM
            PkgConfig::GTK PkgConfig::GTKMM PkgConfig::LIBSPELLING PkgConfig::GSOURCEVIEW)
        if(NOT TARGET "${_vacards_windows_target}")
            continue()
        endif()
        get_target_property(_vacards_windows_links "${_vacards_windows_target}" INTERFACE_LINK_LIBRARIES)
        if(NOT _vacards_windows_links)
            continue()
        endif()
        set(_vacards_windows_rewritten "")
        foreach(_vacards_windows_link IN LISTS _vacards_windows_links)
            set(_vacards_windows_component "")
            if(_vacards_windows_link MATCHES "(^|[/\\\\])lib(cairo(-gobject|-script-interpreter)?)(\\.dll)?\\.a$")
                set(_vacards_windows_component "${CMAKE_MATCH_2}")
            elseif(_vacards_windows_link MATCHES "^(-l)?(cairo(-gobject|-script-interpreter)?)$")
                set(_vacards_windows_component "${CMAKE_MATCH_2}")
            endif()
            if(NOT _vacards_windows_component STREQUAL "")
                set(_vacards_windows_link "${VACARDS_RESOLVED_CAIRO_PREFIX}/lib/lib${_vacards_windows_component}.dll.a")
                if(NOT EXISTS "${_vacards_windows_link}")
                    message(FATAL_ERROR "Missing verified Cairo import: ${_vacards_windows_link}")
                endif()
            endif()
            list(APPEND _vacards_windows_rewritten "${_vacards_windows_link}")
        endforeach()
        set_property(TARGET "${_vacards_windows_target}" PROPERTY INTERFACE_LINK_LIBRARIES "${_vacards_windows_rewritten}")
        # Transitive GTK/Pango targets can also retain stock Cairo headers.
        # Put the verified headers first and remove only Cairo-specific paths.
        get_target_property(_vacards_windows_includes "${_vacards_windows_target}" INTERFACE_INCLUDE_DIRECTORIES)
        set(_vacards_windows_rewritten_includes "${VACARDS_RESOLVED_CAIRO_PREFIX}/include/cairo")
        if(_vacards_windows_includes)
            foreach(_vacards_windows_include IN LISTS _vacards_windows_includes)
                if(NOT _vacards_windows_include MATCHES "(^|[/\\\\])include[/\\\\]cairo[/\\\\]?$")
                    list(APPEND _vacards_windows_rewritten_includes "${_vacards_windows_include}")
                endif()
            endforeach()
        endif()
        list(REMOVE_DUPLICATES _vacards_windows_rewritten_includes)
        set_property(TARGET "${_vacards_windows_target}" PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${_vacards_windows_rewritten_includes}")
    endforeach()
endif()
# END VACARDS WINDOWS CAIRO LINK CHECK

# Stacktrace print on crash; LIB-001 also compiles Boost.JSON from headers.
# Require the precise numeric parsing mode available in Boost 1.83. This does not fetch or upgrade
# dependencies: the selected/pinned toolchain must already satisfy the minimum.
if(WIN32)
    find_package(Boost 1.83.0 REQUIRED COMPONENTS stacktrace_windbg)
    list(APPEND INKSCAPE_LIBS "-lole32")
    list(APPEND INKSCAPE_LIBS "-ldbgeng")
    add_definitions("-DBOOST_STACKTRACE_USE_WINDBG")
elseif(APPLE)
    find_package(Boost 1.83.0 REQUIRED COMPONENTS stacktrace_basic)
    list(APPEND INKSCAPE_CXX_FLAGS "-D_GNU_SOURCE")
else()
    find_package(Boost 1.83.0 REQUIRED)
    # The package stacktrace_backtrace may not be available on all distros.
    find_package(Boost 1.83.0 COMPONENTS stacktrace_backtrace)
    if (BOOST_FOUND)
        list(APPEND INKSCAPE_LIBS "-lbacktrace")
        add_definitions("-DBOOST_STACKTRACE_USE_BACKTRACE")
    else() # fall back to stacktrace_basic
        find_package(Boost 1.83.0 REQUIRED COMPONENTS stacktrace_basic)
        list(APPEND INKSCAPE_CXX_FLAGS "-D_GNU_SOURCE")
    endif()
endif()



if (CMAKE_COMPILER_IS_GNUCC AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER 7 AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    list(APPEND INKSCAPE_LIBS "-lstdc++fs")
endif()

list(APPEND INKSCAPE_LIBS Boost::headers)

find_package(LibXslt REQUIRED)
list(APPEND INKSCAPE_LIBS LibXslt::LibXslt)

find_package(LibXml2 REQUIRED)
list(APPEND INKSCAPE_LIBS LibXml2::LibXml2)

find_package(ZLIB REQUIRED)
list(APPEND INKSCAPE_LIBS ZLIB::ZLIB)

if(WITH_GNU_READLINE)
  pkg_check_modules(Readline IMPORTED_TARGET readline)
  if(Readline_FOUND)
    message(STATUS "Found GNU Readline: ${Readline_LIBRARY}")
    list(APPEND INKSCAPE_LIBS PkgConfig::Readline)
  else()
    message(STATUS "Did not find GNU Readline")
    set(WITH_GNU_READLINE OFF)
  endif()
endif()

if(WITH_IMAGE_MAGICK)
    # we want "<" but pkg_check_modules only offers "<=" for some reason; let's hope nobody actually has 7.0.0
    pkg_check_modules(MAGICK IMPORTED_TARGET ImageMagick++<=7)
    if(MAGICK_FOUND)
        set(WITH_GRAPHICS_MAGICK OFF)  # prefer ImageMagick for now and disable GraphicsMagick if found
    else()
        set(WITH_IMAGE_MAGICK OFF)
    endif()
endif()
if(WITH_GRAPHICS_MAGICK)
    pkg_check_modules(MAGICK IMPORTED_TARGET GraphicsMagick++)
    if(NOT MAGICK_FOUND)
        set(WITH_GRAPHICS_MAGICK OFF)
    endif()
endif()
if(MAGICK_FOUND)
    list(APPEND INKSCAPE_LIBS PkgConfig::MAGICK)
    set(WITH_MAGICK ON) # enable 'Extensions > Raster'
endif()

set(ENABLE_NLS OFF)
if(WITH_NLS)
    find_package(Gettext)
    if(GETTEXT_FOUND)
        message(STATUS "Found gettext + msgfmt to convert language files. Translation enabled")
        set(ENABLE_NLS ON)
    else(GETTEXT_FOUND)
        message(STATUS "Cannot find gettext + msgfmt to convert language file. Translation won't be enabled")
        set(WITH_NLS OFF)
    endif(GETTEXT_FOUND)
    find_program(GETTEXT_XGETTEXT_EXECUTABLE xgettext)
    if(GETTEXT_XGETTEXT_EXECUTABLE)
        message(STATUS "Found xgettext. inkscape.pot will be re-created if missing.")
    else()
        message(STATUS "Did not find xgettext. inkscape.pot can't be re-created.")
    endif()
endif(WITH_NLS)

pkg_check_modules(SIGC++ REQUIRED IMPORTED_TARGET sigc++-3.0>=3.6)
list(APPEND INKSCAPE_LIBS PkgConfig::SIGC++)
list(APPEND INKSCAPE_CXX_FLAGS "-DSIGCXX_DISABLE_DEPRECATED")

pkg_check_modules(EPOXY REQUIRED IMPORTED_TARGET epoxy)
list(APPEND INKSCAPE_LIBS PkgConfig::EPOXY)


# end Dependencies



# Set include directories and CXX flags
# (INKSCAPE_LIBS are set as target_link_libraries for inkscape_base in src/CMakeLists.txt)

foreach(flag ${INKSCAPE_CXX_FLAGS})
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${flag}")
endforeach()
foreach(flag ${INKSCAPE_CXX_FLAGS_DEBUG})
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} ${flag}")
endforeach()

# Add color output to ninja
if ("${CMAKE_GENERATOR}" MATCHES "Ninja")
    add_compile_options (-fdiagnostics-color)
endif ()

list(REMOVE_DUPLICATES INKSCAPE_LIBS)

include(${CMAKE_CURRENT_LIST_DIR}/ConfigChecks.cmake) # TODO: Check if this needs to be "hidden" here

unset(INKSCAPE_CXX_FLAGS)
unset(INKSCAPE_CXX_FLAGS_DEBUG)

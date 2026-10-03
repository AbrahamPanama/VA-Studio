cmake_minimum_required(VERSION 3.15)
project(libUEMF VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_EXTENSIONS OFF)
add_compile_options(-Wall -Wpedantic)

if(PROJECT_IS_TOP_LEVEL)
    set(LIB_TYPE SHARED)
else()
    set(LIB_TYPE STATIC)
endif()

add_library(uemf ${LIB_TYPE})

set(INC include/libuemf)
set(SRC src/libuemf)
target_sources(uemf PRIVATE
    ${INC}/uemf.h
    ${SRC}/uemf.c
    ${INC}/uemf_print.h
    ${SRC}/uemf_print.c
    ${INC}/uemf_endian.h
    ${SRC}/uemf_endian.c
    ${INC}/uemf_safe.h
    ${SRC}/uemf_safe.c
    ${INC}/uemf_utf.h
    ${SRC}/uemf_utf.c
    ${INC}/uwmf.h
    ${SRC}/uwmf.c
    ${INC}/uwmf_print.h
    ${SRC}/uwmf_print.c
    ${INC}/uwmf_endian.h
    ${SRC}/uwmf_endian.c
    ${INC}/upmf.h
    ${SRC}/upmf.c
    ${INC}/upmf_print.h
    ${SRC}/upmf_print.c
)

find_package(Iconv REQUIRED)
target_link_libraries(uemf PRIVATE Iconv::Iconv m)

if(PROJECT_IS_TOP_LEVEL)
    target_include_directories(uemf PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include/libuemf>)
    target_include_directories(uemf SYSTEM PUBLIC $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

    set_target_properties(uemf PROPERTIES VERSION ${PROJECT_VERSION})
    set_target_properties(uemf PROPERTIES SOVERSION ${PROJECT_VERSION_MAJOR})

    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)
    write_basic_package_version_file("UemfConfigVersion.cmake" VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
    install(TARGETS uemf EXPORT uemf_targets DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/include/libuemf/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/libuemf)
    install(EXPORT uemf_targets FILE UemfTargets.cmake NAMESPACE Uemf:: DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Uemf")
    install(FILES "UemfConfig.cmake" "${CMAKE_CURRENT_BINARY_DIR}/UemfConfigVersion.cmake" DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Uemf")

    function(libuemf_add_executable NAME)
        add_executable(${NAME})
        target_sources(${NAME} PRIVATE ${NAME}.c)
        target_link_libraries(${NAME} PRIVATE uemf m)
    endfunction()

    add_subdirectory(src/tools)

    enable_testing()
    add_subdirectory(test)
else()
    target_include_directories(uemf PRIVATE ${CMAKE_CURRENT_LIST_DIR}/include/libuemf)
    target_include_directories(uemf SYSTEM PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)

    add_library(Uemf::uemf ALIAS uemf)
endif()

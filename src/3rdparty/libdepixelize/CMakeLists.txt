cmake_minimum_required(VERSION 3.13)
project(depixelize VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wsequence-point
    -Wnarrowing
)

option(OUTPUT_PROFILE_INFO "Instructs libdepixelize to output profile info to the standard error output" OFF)
if(OUTPUT_PROFILE_INFO)
    add_definitions(-DLIBDEPIXELIZE_WITH_PROFILING)
endif()

find_package(2Geom CONFIG REQUIRED)

if(PROJECT_IS_TOP_LEVEL)
    set(LIB_TYPE SHARED)
else()
    set(LIB_TYPE STATIC)
endif()

add_library(depixelize ${LIB_TYPE})
target_sources(depixelize
    PRIVATE
        include/depixelize/depixelize.h
        include/depixelize/splines.h
        include/depixelize/image.h

        src/depixelize/depixelize.cpp

        src/depixelize/colorspace.h
        src/depixelize/curvature.h
        src/depixelize/homogeneoussplines.h
        src/depixelize/integral.h
        src/depixelize/iterator.h
        src/depixelize/optimization.h
        src/depixelize/pixelgraph.h
        src/depixelize/point.h
        src/depixelize/simplifiedvoronoi.h
        src/depixelize/splines-impl.h
)
target_link_libraries(depixelize PUBLIC 2Geom::2geom)
target_include_directories(depixelize
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include/depixelize
        ${CMAKE_CURRENT_SOURCE_DIR}/src/depixelize
)

if(PROJECT_IS_TOP_LEVEL)
    add_subdirectory(src/tracer)

    set_target_properties(depixelize PROPERTIES VERSION ${PROJECT_VERSION})
    set_target_properties(depixelize PROPERTIES SOVERSION ${PROJECT_VERSION_MAJOR})

    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)
    write_basic_package_version_file("DepixelizeConfigVersion.cmake" VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
    install(TARGETS depixelize EXPORT depixelize_targets)
    install(DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/include/depixelize/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/depixelize)
    install(EXPORT depixelize_targets FILE DepixelizeTargets.cmake NAMESPACE Depixelize:: DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Depixelize")
    install(FILES "DepixelizeConfig.cmake" "${CMAKE_CURRENT_BINARY_DIR}/DepixelizeConfigVersion.cmake" DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Depixelize")
else()
    add_library(Depixelize::depixelize ALIAS depixelize)
endif()

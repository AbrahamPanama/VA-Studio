include(CMakeFindDependencyMacro)
find_dependency(2Geom CONFIG REQUIRED)
include("${CMAKE_CURRENT_LIST_DIR}/DepixelizeTargets.cmake")

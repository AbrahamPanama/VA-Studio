include(CMakeFindDependencyMacro)
find_dependency(Iconv REQUIRED)
include("${CMAKE_CURRENT_LIST_DIR}/UemfTargets.cmake")

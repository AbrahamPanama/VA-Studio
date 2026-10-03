set(csslint_EXE csslint)

add_executable(${csslint_EXE} csslint.c)

target_include_directories(${csslint_EXE}
    PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/src
    ${GLIB_INCLUDE_DIRS}
    ${LIBXML_INCLUDE_DIRS}
)

target_link_libraries(${csslint_EXE}
    PRIVATE
    croco_LIB
)

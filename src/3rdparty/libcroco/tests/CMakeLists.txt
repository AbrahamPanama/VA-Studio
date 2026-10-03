set(testprogs
    test0
    test1
    test2
    test3
    test4
    test5
    test6
    test7
    test8
)

foreach(source ${testprogs})
    add_executable(${source} ${source}-main.c cr-test-utils.c cr-test-utils.h)

    target_include_directories(${source}
        PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_BINARY_DIR}/src
        ${GLIB_INCLUDE_DIRS}
        ${LIBXML_INCLUDE_DIRS}
    )

    target_link_libraries(${source}
        croco_LIB
    )
endforeach()

if(1)
    # Run each test separately

    set(testscripts
        test-custom-prop.sh
        test-functional-notation.sh
        test-lots-of-comments.sh
        test-parsing-location.sh
        test-prop-ident.sh
        test-sel-child-class.sh
        test-several-media.sh
        test-unknown-at-rule2.sh
        test-unknown-at-rule.sh
    )

    add_test(NAME cleantest
        COMMAND ${CMAKE_SOURCE_DIR}/tests/testctl cleanup
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )

    foreach(source ${testprogs} ${testscripts})
        add_test(NAME ${source}
            COMMAND ${CMAKE_SOURCE_DIR}/tests/testctl run --test-and-report ${source}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        )
        set_tests_properties(${source} PROPERTIES DEPENDS "cleantest")
    endforeach()
else()
    # Run all tests at once
    add_test(NAME all-tests
        COMMAND ${CMAKE_SOURCE_DIR}/tests/testctl run
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
endif()

get_property(test_names DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY TESTS)
set_property(TEST ${test_names} PROPERTY ENVIRONMENT
    "TEST_OUT_DIR=${CMAKE_CURRENT_BINARY_DIR}"
    "TEST_SOURCE_DIR=${CMAKE_SOURCE_DIR}/tests"
    "TEST_INPUTS_DIR=${CMAKE_SOURCE_DIR}/tests/test-inputs"
    "CSSLINT=${CMAKE_BINARY_DIR}/csslint/csslint"
)

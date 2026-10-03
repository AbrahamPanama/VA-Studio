libuemf_add_executable(testbed_emf)
libuemf_add_executable(testbed_pmf)
libuemf_add_executable(testbed_wmf)
libuemf_add_executable(test_mapmodes_emf)

# Test all programs against reference.

add_test(NAME test_write_emf    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_write.sh $<TARGET_FILE:testbed_emf> 0 test_libuemf.emf   ${CMAKE_CURRENT_SOURCE_DIR}/data/test_libuemf_ref.emf)
add_test(NAME test_write_emf_30 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_write.sh $<TARGET_FILE:testbed_emf> 4 test_libuemf.emf   ${CMAKE_CURRENT_SOURCE_DIR}/data/test_libuemf_ref30.emf)
add_test(NAME test_write_wmf    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_write.sh $<TARGET_FILE:testbed_wmf> 0 test_libuemf.wmf   ${CMAKE_CURRENT_SOURCE_DIR}/data/test_libuemf_ref.wmf)
add_test(NAME test_write_pmf    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_write.sh $<TARGET_FILE:testbed_pmf> 0 test_libuemf_p.emf ${CMAKE_CURRENT_SOURCE_DIR}/data/test_libuemf_p_ref.emf)

add_test(NAME test_read_emf    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_read.sh $<TARGET_FILE:reademf> ${CMAKE_CURRENT_SOURCE_DIR}/data/test_libuemf_ref.emf   ${CMAKE_CURRENT_SOURCE_DIR}/data/test_libuemf_ref_emf.txt)
add_test(NAME test_read_emf_30 COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_read.sh $<TARGET_FILE:reademf> ${CMAKE_CURRENT_SOURCE_DIR}/data/test_libuemf_ref30.emf ${CMAKE_CURRENT_SOURCE_DIR}/data/test_libuemf_ref30_emf.txt)
add_test(NAME test_read_wmf    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_read.sh $<TARGET_FILE:readwmf> ${CMAKE_CURRENT_SOURCE_DIR}/data/test_libuemf_ref.wmf   ${CMAKE_CURRENT_SOURCE_DIR}/data/test_libuemf_ref_wmf.txt)
add_test(NAME test_read_pmf    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_read.sh $<TARGET_FILE:reademf> ${CMAKE_CURRENT_SOURCE_DIR}/data/test_libuemf_p_ref.emf ${CMAKE_CURRENT_SOURCE_DIR}/data/test_libuemf_p_ref_emf.txt)

add_test(NAME test_mapmodes COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/test_mapmodes.sh $<TARGET_FILE:test_mapmodes_emf> ${CMAKE_CURRENT_SOURCE_DIR}/data)

libuemf_add_executable(cutemf)
libuemf_add_executable(pmfdual2single)
libuemf_add_executable(reademf)
libuemf_add_executable(readwmf)

install(TARGETS cutemf pmfdual2single reademf readwmf DESTINATION ${CMAKE_INSTALL_BINDIR})

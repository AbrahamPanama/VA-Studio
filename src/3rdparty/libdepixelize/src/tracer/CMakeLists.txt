find_package(PkgConfig REQUIRED)
pkg_check_modules(POPT REQUIRED IMPORTED_TARGET popt)
pkg_check_modules(GDKPIXBUF REQUIRED IMPORTED_TARGET gdk-pixbuf-2.0)

add_executable(depixelize-tracer)
set_target_properties(depixelize-tracer PROPERTIES OUTPUT_NAME "depixelize")

target_sources(depixelize-tracer PRIVATE
    main.cpp
)

target_compile_definitions(depixelize-tracer PRIVATE
    DEPIXELIZE_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(depixelize-tracer
    PUBLIC
        depixelize
        PkgConfig::POPT
        PkgConfig::GDKPIXBUF
)

install(TARGETS depixelize-tracer)

add_subdirectory(application)
add_subdirectory(hicolor)
add_subdirectory(multicolor)
add_subdirectory(Dash)

# Shared welcome branding, also on the source icon search path for development.
# Keep the unthemed application asset available on every supported platform.
install(FILES com.vacards.Inkscape.png DESTINATION ${INKSCAPE_SHARE_INSTALL}/icons)

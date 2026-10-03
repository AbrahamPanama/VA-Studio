# SPDX-License-Identifier: GPL-2.0-or-later

target_sources(inkscape_base PRIVATE
  artwork-library-archive.cpp
  artwork-library-catalog.cpp
  artwork-library-document.cpp
  artwork-library-insert.cpp
  artwork-library-lbart.cpp
  artwork-library-lightburn.cpp
  artwork-library-encoding.cpp
  artwork-library-manifest.cpp
  artwork-library-package.cpp
  artwork-library-png-preflight.cpp
  artwork-library-storage.cpp
  artwork-library-svg-preflight.cpp
  dir-util.cpp
  document-file-operation.cpp
  document-file-transaction.cpp
  document-file-transaction-posix.cpp
  document-file-transaction-win32.cpp
  existing-file-replacement.cpp
  existing-file-replacement-win32.cpp
  file.cpp
  file-export-cmd.cpp
  export-destination.cpp
  preview-css-admission.cpp
  preview-svg-resource-policy.cpp
  preview-xml-input.cpp
  recent-files.cpp
  resource.cpp
  resource-list.cpp
  sandbox.cpp
  split-path.cpp
  fix-broken-links.cpp
  query-file-info.cpp
  stream/bufferstream.cpp
  stream/gzipstream.cpp
  stream/inkscapestream.cpp
  stream/stringstream.cpp
  stream/uristream.cpp
  stream/xsltstream.cpp
  sys.cpp
  tiff-export.cpp

  # -------
  # Headers
  artwork-library-archive.h
  artwork-library-catalog.h
  artwork-library-document.h
  artwork-library-insert.h
  artwork-library-lbart.h
  artwork-library-lightburn.h
  artwork-library-encoding.h
  artwork-library-manifest.h
  artwork-library-package.h
  artwork-library-png-preflight.h
  artwork-library-storage.h
  artwork-library-svg-preflight.h
  dir-util.h
  document-file-operation.h
  document-file-transaction.h
  existing-file-replacement.h
  file.h
  file-export-cmd.h
  export-destination.h
  preview-css-admission.h
  preview-svg-resource-policy.h
  preview-xml-input.h
  recent-files.h
  resource.h
  sandbox.h
  split-path.h
  fix-broken-links.h
  query-file-info.h
  stream/bufferstream.h
  stream/gzipstream.h
  stream/inkscapestream.h
  stream/stringstream.h
  stream/uristream.h
  stream/xsltstream.h
  sys.h
  tiff-export.h
)

# Fault injection is available only in local test builds. Keep the definition
# source-local so enabling tests does not recompile the entire application.
if(BUILD_TESTING)
  set_source_files_properties(
    "${CMAKE_CURRENT_SOURCE_DIR}/existing-file-replacement.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/existing-file-replacement-win32.cpp"
    TARGET_DIRECTORY inkscape_base
    PROPERTIES COMPILE_DEFINITIONS VACARDS_FILE_IO_TEST_HOOKS
  )
endif()

#----------------------------------------------------------------
# Generated CMake target import file for configuration "Release".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "GalayHttp::galay-http" for configuration "Release"
set_property(TARGET GalayHttp::galay-http APPEND PROPERTY IMPORTED_CONFIGURATIONS RELEASE)
set_target_properties(GalayHttp::galay-http PROPERTIES
  IMPORTED_LOCATION_RELEASE "${_IMPORT_PREFIX}/lib/libgalay-http.1.0.0.dylib"
  IMPORTED_SONAME_RELEASE "@rpath/libgalay-http.1.dylib"
  )

list(APPEND _cmake_import_check_targets GalayHttp::galay-http )
list(APPEND _cmake_import_check_files_for_GalayHttp::galay-http "${_IMPORT_PREFIX}/lib/libgalay-http.1.0.0.dylib" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)

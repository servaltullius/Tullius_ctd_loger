function(skydiag_add_version_resource target original_filename file_description file_type)
  set(SKYDIAG_VERSION_MAJOR "${CMAKE_PROJECT_VERSION_MAJOR}")
  set(SKYDIAG_VERSION_MINOR "${CMAKE_PROJECT_VERSION_MINOR}")
  set(SKYDIAG_VERSION_PATCH "${CMAKE_PROJECT_VERSION_PATCH}")
  set(SKYDIAG_VERSION_STRING "${CMAKE_PROJECT_VERSION}")
  set(SKYDIAG_ORIGINAL_FILENAME "${original_filename}")
  set(SKYDIAG_FILE_DESCRIPTION "${file_description}")
  set(SKYDIAG_FILE_TYPE "${file_type}")

  set(_version_resource "${CMAKE_CURRENT_BINARY_DIR}/generated/${target}.version.rc")
  configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/SkyrimDiagVersion.rc.in"
    "${_version_resource}"
    @ONLY
  )
  target_sources(${target} PRIVATE "${_version_resource}")
endfunction()

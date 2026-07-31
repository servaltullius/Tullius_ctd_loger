cmake_minimum_required(VERSION 3.24)

project(
  SkyrimDiag
  VERSION 0.2.58
  LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/SkyrimDiagVersion.cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/utf-8)
endif()
if(WIN32)
  enable_language(RC)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

add_library(skydiag_shared INTERFACE)
target_include_directories(skydiag_shared INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/shared")
target_compile_definitions(skydiag_shared INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN)

enable_testing()
add_subdirectory(tests)

option(SKYDIAG_FUZZ "Build fuzzing harnesses (requires clang with libFuzzer)" OFF)
if(SKYDIAG_FUZZ)
  include(CheckIncludeFileCXX)
  check_include_file_cxx("nlohmann/json.hpp" SKYDIAG_HAS_NLOHMANN_JSON_FOR_FUZZ)
  add_subdirectory(fuzz)
endif()

if(NOT WIN32)
  message(STATUS "SkyrimDiag: Windows-only targets (plugin/helper) are skipped on non-Windows hosts.")
else()
  add_subdirectory(plugin)
  add_subdirectory(helper)
  add_subdirectory(dump_tool)

  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_custom_target(skydiag_native_provenance ALL
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/scripts/write_build_provenance.py"
      --repo-root "${CMAKE_CURRENT_SOURCE_DIR}"
      --output "$<TARGET_FILE_DIR:SkyrimDiag>/SkyrimDiagBuildProvenance.json"
      --kind native
      --configuration "$<CONFIG>"
      --artifact "SkyrimDiag.dll=$<TARGET_FILE:SkyrimDiag>"
      --artifact "SkyrimDiagHelper.exe=$<TARGET_FILE:SkyrimDiagHelper>"
      --artifact "SkyrimDiagDumpToolNative.dll=$<TARGET_FILE:SkyrimDiagDumpToolNative>"
      --artifact "SkyrimDiagDumpToolCli.exe=$<TARGET_FILE:SkyrimDiagDumpToolCli>"
      --artifact "SkyrimDiagDumpToolWinUI.exe=$<TARGET_FILE:SkyrimDiagDumpToolWinUILauncher>"
    DEPENDS
      SkyrimDiag
      SkyrimDiagHelper
      SkyrimDiagDumpToolNative
      SkyrimDiagDumpToolCli
      SkyrimDiagDumpToolWinUILauncher
    VERBATIM
  )
endif()

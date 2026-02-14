cmake_minimum_required(VERSION 3.24)

project(
  SkyrimDiag
  VERSION 0.2.29
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/utf-8)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

add_library(skydiag_shared INTERFACE)
target_include_directories(skydiag_shared INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/shared")
target_compile_definitions(skydiag_shared INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN)

enable_testing()
add_subdirectory(tests)

if(NOT WIN32)
  message(STATUS "SkyrimDiag: Windows-only targets (plugin/helper) are skipped on non-Windows hosts.")
else()
  add_subdirectory(plugin)
  add_subdirectory(helper)
  add_subdirectory(dump_tool)
endif()

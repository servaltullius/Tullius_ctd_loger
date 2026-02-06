cmake_minimum_required(VERSION 3.24)

project(SkyrimDiagDumpTool VERSION ${CMAKE_PROJECT_VERSION} LANGUAGES CXX)

find_package(nlohmann_json CONFIG REQUIRED)

add_library(SkyrimDiagDumpToolCore STATIC
  src/Analyzer.cpp
  src/Analyzer.h
  src/EvidenceBuilder.cpp
  src/EvidenceBuilder.h
  src/CrashLogger.cpp
  src/CrashLogger.h
  src/Mo2Index.cpp
  src/Mo2Index.h
  src/NativeApi.h
  src/Utf.cpp
  src/Utf.h
  src/OutputWriter.cpp
  src/OutputWriter.h)

target_link_libraries(SkyrimDiagDumpToolCore
  PUBLIC
    skydiag_shared
    nlohmann_json::nlohmann_json
    Dbghelp
)

target_compile_definitions(SkyrimDiagDumpToolCore
  PRIVATE
    _UNICODE
    UNICODE)

set_property(TARGET SkyrimDiagDumpToolCore PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL$<$<CONFIG:Debug>:Debug>")

add_library(SkyrimDiagDumpToolNative SHARED
  src/NativeApi.cpp
  src/NativeApi.h)

target_link_libraries(SkyrimDiagDumpToolNative
  PRIVATE
    SkyrimDiagDumpToolCore)

target_compile_definitions(SkyrimDiagDumpToolNative
  PRIVATE
    _UNICODE
    UNICODE)

set_property(TARGET SkyrimDiagDumpToolNative PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL$<$<CONFIG:Debug>:Debug>")

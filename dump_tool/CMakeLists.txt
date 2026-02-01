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

add_executable(SkyrimDiagDumpTool WIN32
  resources/SkyrimDiagDumpTool.rc
  src/GuiApp.cpp
  src/GuiApp.h
  src/main.cpp)

target_link_libraries(SkyrimDiagDumpTool
  PRIVATE
    SkyrimDiagDumpToolCore
    Comdlg32
    Comctl32
    Shell32
    Ole32
    User32
    UxTheme
    Dwmapi)

target_compile_definitions(SkyrimDiagDumpTool
  PRIVATE
    _UNICODE
    UNICODE)

set_property(TARGET SkyrimDiagDumpTool PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL$<$<CONFIG:Debug>:Debug>")

if (MSVC)
  # We embed our own RT_MANIFEST via resources/SkyrimDiagDumpTool.rc.
  # Disable the linker's default manifest generation to avoid duplicate manifest resources.
  target_link_options(SkyrimDiagDumpTool PRIVATE /MANIFEST:NO)
endif()

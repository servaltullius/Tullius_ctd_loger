cmake_minimum_required(VERSION 3.24)

project(SkyrimDiagHelper LANGUAGES CXX)

find_package(nlohmann_json CONFIG REQUIRED)

add_executable(${PROJECT_NAME}
  src/CompatibilityPreflight.cpp
  src/Config.cpp
  src/CrashEtwCapture.cpp
  src/CrashCapture.cpp
  src/DumpToolLaunch.cpp
  src/DumpWriter.cpp
  src/EtwCapture.cpp
  src/HangCapture.cpp
  src/HelperCommon.cpp
  src/HelperLog.cpp
  src/HangDetect.cpp
  src/IncidentManifest.cpp
  src/LoadStats.cpp
  src/ManualCapture.cpp
  src/PendingCrashAnalysis.cpp
  src/PluginScanner.cpp
  src/ProcessAttach.cpp
  src/ProcessUtil.cpp
  src/RetentionWorker.cpp
  src/WctCapture.cpp
  src/WindowHeuristics.cpp
  src/main.cpp
  src/CompatibilityPreflight.h
  include/SkyrimDiagHelper/Config.h
  include/SkyrimDiagHelper/DumpWriter.h
  include/SkyrimDiagHelper/HangDetect.h
  include/SkyrimDiagHelper/LoadStats.h
  include/SkyrimDiagHelper/PluginScanner.h
  include/SkyrimDiagHelper/ProcessAttach.h
  src/RetentionWorker.h
  include/SkyrimDiagHelper/WctCapture.h)

target_include_directories(${PROJECT_NAME}
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(${PROJECT_NAME}
  PRIVATE
    skydiag_shared
    nlohmann_json::nlohmann_json
    Advapi32
    User32
    Dbghelp
    Version
    Shell32
    Ole32)

target_compile_definitions(${PROJECT_NAME}
  PRIVATE
    _UNICODE
    UNICODE)

set_property(TARGET ${PROJECT_NAME} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/dist/SkyrimDiagHelper.ini"
    "$<TARGET_FILE_DIR:${PROJECT_NAME}>/SkyrimDiagHelper.ini"
)

cmake_minimum_required(VERSION 3.24)

project(SkyrimDiagHelper LANGUAGES CXX)

find_package(nlohmann_json CONFIG REQUIRED)

add_executable(${PROJECT_NAME}
  src/Config.cpp
  src/DumpWriter.cpp
  src/HangDetect.cpp
  src/LoadStats.cpp
  src/ProcessAttach.cpp
  src/WctCapture.cpp
  src/main.cpp
  include/SkyrimDiagHelper/Config.h
  include/SkyrimDiagHelper/DumpWriter.h
  include/SkyrimDiagHelper/HangDetect.h
  include/SkyrimDiagHelper/LoadStats.h
  include/SkyrimDiagHelper/ProcessAttach.h
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
    Dbghelp)

target_compile_definitions(${PROJECT_NAME}
  PRIVATE
    _UNICODE
    UNICODE)

set_property(TARGET ${PROJECT_NAME} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL$<$<CONFIG:Debug>:Debug>")

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/dist/SkyrimDiagHelper.ini"
    "$<TARGET_FILE_DIR:${PROJECT_NAME}>/SkyrimDiagHelper.ini"
)

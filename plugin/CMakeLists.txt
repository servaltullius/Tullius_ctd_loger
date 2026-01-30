cmake_minimum_required(VERSION 3.24)

project(SkyrimDiagPlugin VERSION ${CMAKE_PROJECT_VERSION} LANGUAGES CXX)

find_package(CommonLibSSE CONFIG REQUIRED)

set(TARGET_NAME SkyrimDiag)

set(headers
  include/SkyrimDiag/Blackbox.h
  include/SkyrimDiag/CrashHandler.h
  include/SkyrimDiag/EventSinks.h
  include/SkyrimDiag/Hash.h
  include/SkyrimDiag/Heartbeat.h
  include/SkyrimDiag/ResourceLog.h
  include/SkyrimDiag/SharedMemory.h)

set(sources
  src/Blackbox.cpp
  src/CrashHandler.cpp
  src/EventSinks.cpp
  src/Heartbeat.cpp
  src/PluginInfo.cpp
  src/PluginMain.cpp
  src/ResourceHooks.cpp
  src/ResourceLog.cpp
  src/SharedMemory.cpp)

add_library(${TARGET_NAME} SHARED ${headers} ${sources})

target_include_directories(${TARGET_NAME}
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(${TARGET_NAME}
  PRIVATE
    skydiag_shared
    CommonLibSSE::CommonLibSSE)

target_compile_definitions(${TARGET_NAME}
  PRIVATE
    _UNICODE
    UNICODE)

set_property(TARGET ${TARGET_NAME} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreadedDLL$<$<CONFIG:Debug>:Debug>")

if(DEFINED ENV{SKYRIM_FOLDER})
  set(_skyrim_plugins "$ENV{SKYRIM_FOLDER}/Data/SKSE/Plugins")
  add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_skyrim_plugins}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${TARGET_NAME}>" "${_skyrim_plugins}/SkyrimDiag.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_PDB_FILE:${TARGET_NAME}>" "${_skyrim_plugins}/SkyrimDiag.pdb"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/dist/SkyrimDiag.ini" "${_skyrim_plugins}/SkyrimDiag.ini"
  )
elseif(DEFINED ENV{SKYRIM_MODS_FOLDER})
  set(_mods_plugins "$ENV{SKYRIM_MODS_FOLDER}/SkyrimDiag/SKSE/Plugins")
  add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${_mods_plugins}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${TARGET_NAME}>" "${_mods_plugins}/SkyrimDiag.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_PDB_FILE:${TARGET_NAME}>" "${_mods_plugins}/SkyrimDiag.pdb"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/dist/SkyrimDiag.ini" "${_mods_plugins}/SkyrimDiag.ini"
  )
endif()

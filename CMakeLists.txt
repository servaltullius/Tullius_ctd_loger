cmake_minimum_required(VERSION 3.24)

# Static vcpkg libraries with the DLL CRT: SKSE loads SkyrimDiag.dll from
# Data/SKSE/Plugins, where nothing would provide spdlog.dll or fmt.dll (the
# default x64-windows triplet builds them as DLLs; load error 126). The vcpkg
# toolchain reads it when the project is declared, so it has to come first.
if(NOT DEFINED VCPKG_TARGET_TRIPLET)
  set(VCPKG_TARGET_TRIPLET "x64-windows-static-md" CACHE STRING "")
endif()
# Same-named overlay of that triplet: the static vcpkg libraries linked into
# the plugin and helper (spdlog, fmt) must not use the constexpr std::mutex
# constructor either (see _DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR below).
# Appended, not only set when undefined: the vcpkg toolchain caches the
# variable (empty when nothing set it), so a build folder configured before
# this overlay existed would otherwise never get it.
set(SKYRIMDIAG_VCPKG_TRIPLET_OVERLAY "${CMAKE_CURRENT_SOURCE_DIR}/cmake/triplets")
if(NOT SKYRIMDIAG_VCPKG_TRIPLET_OVERLAY IN_LIST VCPKG_OVERLAY_TRIPLETS)
  list(APPEND VCPKG_OVERLAY_TRIPLETS "${SKYRIMDIAG_VCPKG_TRIPLET_OVERLAY}")
  set(VCPKG_OVERLAY_TRIPLETS "${VCPKG_OVERLAY_TRIPLETS}" CACHE STRING "" FORCE)
endif()

project(
  SkyrimDiag
  VERSION 0.2.69
  LANGUAGES CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/SkyrimDiagVersion.cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/utf-8)
  # MSVC 14.40+ makes std::mutex's constructor constexpr. Binaries built that
  # way crash on their first lock (for example SKSE::Init) when the process
  # loads an msvcp140.dll older than 14.40, which an outdated VC++ runtime or
  # a copy shipped next to the game still provides. Every target, the
  # CommonLibSSE-NG subdirectory and the vcpkg ports (cmake/triplets) keep the
  # runtime-initialized constructor that works with old and new runtimes.
  add_compile_definitions(_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR)
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
  # CommonLibSSE-NG is a git submodule pinned to a release tag (GPL-3.0-or-later
  # with the modding exceptions; see extern/CommonLibSSE-NG/EXCEPTIONS.md). SE
  # and AE only; VR would need the openvr submodule. Building from source also
  # keeps it from downloading a prebuilt library during configure.
  set(ENABLE_SKYRIM_SE ON CACHE BOOL "" FORCE)
  set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
  set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)
  set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
  add_subdirectory(extern/CommonLibSSE-NG EXCLUDE_FROM_ALL)

  add_subdirectory(plugin)
  add_subdirectory(helper)
  add_subdirectory(dump_tool)

  option(SKYDIAG_CRASH_TESTER "Build the development-only crash tester SKSE plugins (never packaged)" OFF)
  if(SKYDIAG_CRASH_TESTER)
    add_subdirectory(tools/crash_tester)
  endif()

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

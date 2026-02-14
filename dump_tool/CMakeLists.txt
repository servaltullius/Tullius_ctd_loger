cmake_minimum_required(VERSION 3.24)

project(SkyrimDiagDumpTool VERSION ${CMAKE_PROJECT_VERSION} LANGUAGES CXX)

find_package(nlohmann_json CONFIG REQUIRED)

add_library(SkyrimDiagDumpToolCore STATIC
  src/Analyzer.cpp
  src/Analyzer.h
  src/AnalyzerInternals.cpp
  src/AnalyzerInternals.h
  src/AnalyzerInternalsCrashBucket.cpp
  src/AnalyzerInternalsStackScan.cpp
  src/AnalyzerInternalsWct.cpp
  src/AnalyzerInternalsStackwalk.cpp
  src/AnalyzerInternalsStackwalkFormat.cpp
  src/AnalyzerInternalsStackwalkScoring.cpp
  src/AnalyzerInternalsStackwalkMemory.cpp
  src/AnalyzerInternalsStackwalkPriv.h
  src/AnalyzerInternalsStackwalkSymbols.cpp
  src/EvidenceBuilder.cpp
  src/EvidenceBuilder.h
  src/EvidenceBuilderInternals.cpp
  src/EvidenceBuilderInternals.h
  src/EvidenceBuilderInternalsEvidence.cpp
  src/EvidenceBuilderInternalsPriv.h
  src/EvidenceBuilderInternalsRecommendations.cpp
  src/EvidenceBuilderInternalsSummary.cpp
  src/EvidenceBuilderInternalsUtil.cpp
  src/CrashLogger.cpp
  src/CrashLogger.h
  src/Mo2Index.cpp
  src/Mo2Index.h
  src/MinidumpUtil.cpp
  src/MinidumpUtil.h
  src/SignatureDatabase.cpp
  src/SignatureDatabase.h
  src/AddressResolver.cpp
  src/AddressResolver.h
  src/CrashHistory.cpp
  src/CrashHistory.h
  src/NativeApi.h
  src/Utf.cpp
  src/Utf.h
  src/OutputWriter.cpp
  src/OutputWriter.h
  src/OutputWriterInternals.cpp
  src/OutputWriterInternals.h)

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

set_property(TARGET SkyrimDiagDumpToolCore PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

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

set_property(TARGET SkyrimDiagDumpToolNative PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

add_executable(SkyrimDiagDumpToolCli
  src/DumpToolCliMain.cpp
)

target_link_libraries(SkyrimDiagDumpToolCli
  PRIVATE
    SkyrimDiagDumpToolCore
)

target_compile_definitions(SkyrimDiagDumpToolCli
  PRIVATE
    _UNICODE
    UNICODE
)

set_property(TARGET SkyrimDiagDumpToolCli PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

set(SKYDIAG_DUMP_TOOL_DATA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/data")
set(SKYDIAG_DUMP_TOOL_DATA_FILES
  "hook_frameworks.json"
  "crash_signatures.json"
  "address_db/skyrimse_functions.json"
)

foreach(_target IN ITEMS SkyrimDiagDumpToolNative SkyrimDiagDumpToolCli)
  foreach(_rel IN LISTS SKYDIAG_DUMP_TOOL_DATA_FILES)
    if(EXISTS "${SKYDIAG_DUMP_TOOL_DATA_ROOT}/${_rel}")
      get_filename_component(_rel_dir "${_rel}" DIRECTORY)
      get_filename_component(_rel_name "${_rel}" NAME)
      if(_rel_dir)
        set(_dst_dir "$<TARGET_FILE_DIR:${_target}>/data/${_rel_dir}")
      else()
        set(_dst_dir "$<TARGET_FILE_DIR:${_target}>/data")
      endif()
      add_custom_command(TARGET ${_target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_dst_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${SKYDIAG_DUMP_TOOL_DATA_ROOT}/${_rel}"
          "${_dst_dir}/${_rel_name}"
        VERBATIM
      )
    endif()
  endforeach()
endforeach()

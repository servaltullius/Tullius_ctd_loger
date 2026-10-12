# Development-only SKSE plugins for checking the analysis against crashes and
# freezes of known cause in the real game (tools/crash_tester/README.md).
# Built only with -DSKYDIAG_CRASH_TESTER=ON and never packaged.

find_package(CommonLibSSE CONFIG REQUIRED)

foreach(_target IN ITEMS CrashTester SkyrimCrashGuardStub)
  add_library(${_target} SHARED
    CrashTester.cpp
    "${CMAKE_SOURCE_DIR}/plugin/src/MsvcRegexStub.cpp")
  target_link_libraries(${_target} PRIVATE CommonLibSSE::CommonLibSSE)
  target_compile_definitions(${_target} PRIVATE _UNICODE UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
  set_property(TARGET ${_target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
  set_target_properties(${_target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/crash-tester")
endforeach()

target_compile_definitions(SkyrimCrashGuardStub PRIVATE CRASH_TESTER_GUARD_STUB)
set_target_properties(SkyrimCrashGuardStub PROPERTIES OUTPUT_NAME SkyrimCrashGuard)

# Fuzzing harnesses — build only when SKYDIAG_FUZZ is ON
#
# Usage:
#   cmake -S . -B build-fuzz -DSKYDIAG_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++
#   cmake --build build-fuzz --target fuzz_crashlogger_parser fuzz_wct_parser
#   ./build-fuzz/bin/fuzz_crashlogger_parser fuzz/corpus/crashlogger -max_total_time=60
#
# Sanitizer flags are attached per target rather than through CMAKE_CXX_FLAGS:
# -fsanitize=fuzzer supplies its own main(), which collides with the unit-test
# executables that the same configure step also builds.

set(SKYDIAG_FUZZ_FLAGS -fsanitize=fuzzer,address -fno-omit-frame-pointer -g -O1)

function(skydiag_configure_fuzz_target target)
  target_compile_options(${target} PRIVATE ${SKYDIAG_FUZZ_FLAGS})
  target_link_options(${target} PRIVATE ${SKYDIAG_FUZZ_FLAGS})
  set_target_properties(${target} PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
  )
endfunction()

add_executable(fuzz_crashlogger_parser
  fuzz_crashlogger_parser.cpp
  "${CMAKE_CURRENT_SOURCE_DIR}/../dump_tool/src/CrashLoggerParseCore.cpp"
)

target_include_directories(fuzz_crashlogger_parser PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../dump_tool/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/../shared"
)

skydiag_configure_fuzz_target(fuzz_crashlogger_parser)

if(SKYDIAG_HAS_NLOHMANN_JSON_FOR_FUZZ)
  add_executable(fuzz_wct_parser
    fuzz_wct_parser.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../dump_tool/src/AnalyzerInternalsWct.cpp"
  )

  target_include_directories(fuzz_wct_parser PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../dump_tool/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../shared"
  )

  skydiag_configure_fuzz_target(fuzz_wct_parser)
endif()

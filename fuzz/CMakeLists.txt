# Fuzzing harnesses — build only when SKYDIAG_FUZZ is ON
# Usage:
#   cmake -S . -B build-fuzz -DSKYDIAG_FUZZ=ON \
#     -DCMAKE_CXX_COMPILER=clang++ \
#     -DCMAKE_CXX_FLAGS="-fsanitize=fuzzer,address -g -O1"
#   cmake --build build-fuzz --target fuzz_crashlogger_parser fuzz_wct_parser

add_executable(fuzz_crashlogger_parser
  fuzz_crashlogger_parser.cpp
  "${CMAKE_CURRENT_SOURCE_DIR}/../dump_tool/src/CrashLoggerParseCore.cpp"
)

target_include_directories(fuzz_crashlogger_parser PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../dump_tool/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/../shared"
)

set_target_properties(fuzz_crashlogger_parser PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

if(SKYDIAG_HAS_NLOHMANN_JSON_FOR_FUZZ)
  add_executable(fuzz_wct_parser
    fuzz_wct_parser.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../dump_tool/src/AnalyzerInternalsWct.cpp"
  )

  target_include_directories(fuzz_wct_parser PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../dump_tool/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../shared"
  )

  set_target_properties(fuzz_wct_parser PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
  )
endif()

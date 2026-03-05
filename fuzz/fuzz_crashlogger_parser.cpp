// Fuzz harness for CrashLogger log parser.
// Build with: clang++ -g -O1 -fsanitize=fuzzer,address \
//   -I ../dump_tool/src -I ../shared \
//   fuzz_crashlogger_parser.cpp ../dump_tool/src/CrashLoggerParseCore.cpp \
//   -o fuzz_crashlogger_parser
//
// Run: ./fuzz_crashlogger_parser corpus/ -max_len=8192

#include "CrashLoggerParseCore.h"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

using namespace skydiag::dump_tool::crashlogger_core;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  const std::string_view input(reinterpret_cast<const char*>(data), size);
  const std::string str(input);

  // Exercise version parser
  (void)ParseCrashLoggerVersionAscii(str);

  // Exercise C++ exception details parser
  (void)ParseCrashLoggerCppExceptionDetailsAscii(str);

  // Exercise object refs parser (most complex, most likely to have edge cases)
  auto refs = ParseCrashLoggerObjectRefsAscii(str);
  (void)AggregateCrashLoggerObjectRefs(refs);

  // Exercise ESP name extraction
  (void)ExtractEspNamesFromLine(str);

  return 0;
}

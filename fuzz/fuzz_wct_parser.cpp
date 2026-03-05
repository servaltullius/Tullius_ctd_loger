// Fuzz harness for WCT JSON parser.
// Build with: clang++ -g -O1 -fsanitize=fuzzer,address \
//   -I ../dump_tool/src -I ../shared \
//   fuzz_wct_parser.cpp ../dump_tool/src/AnalyzerInternalsWct.cpp \
//   -o fuzz_wct_parser
//
// Run: ./fuzz_wct_parser corpus/ -max_len=4096

#include "WctTypes.h"

#include <cstdint>
#include <cstdlib>
#include <string_view>

using namespace skydiag::dump_tool::internal;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  const std::string_view input(reinterpret_cast<const char*>(data), size);

  (void)ExtractWctCandidateThreadIds(input, 8);
  (void)TryParseWctCaptureDecision(input);

  return 0;
}

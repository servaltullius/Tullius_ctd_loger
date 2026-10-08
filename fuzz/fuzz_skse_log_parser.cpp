// Fuzz harness for the skse64.log parser.
// Build with: clang++ -g -O1 -fsanitize=fuzzer,address \
//   -I ../helper/include \
//   fuzz_skse_log_parser.cpp ../helper/src/SkseLogParser.cpp \
//   -o fuzz_skse_log_parser
//
// Run: ./fuzz_skse_log_parser corpus/skse_log -max_len=4096

#include "SkyrimDiagHelper/SkseLogParser.h"

#include <cstdint>
#include <cstdlib>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  const std::string_view input(reinterpret_cast<const char*>(data), size);

  const auto summary = skydiag::helper::ParseSkseLog(input);
  if (summary.loaded_count + summary.issues.size() > size) {
    std::abort();  // every counted plugin needs at least one byte of input
  }

  return 0;
}

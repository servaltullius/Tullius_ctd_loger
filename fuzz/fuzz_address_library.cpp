// Fuzz harness for the Address Library parser (ADR-0027): the bytes come from
// a user stream of a dump, which can be cut short or damaged.
// Build with: clang++ -g -O1 -fsanitize=fuzzer,address \
//   -I ../dump_tool/src \
//   fuzz_address_library.cpp ../dump_tool/src/AddressLibrary.cpp \
//   -o fuzz_address_library
//
// Run: ./fuzz_address_library corpus/address_library -max_len=4096

#include "AddressLibrary.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  const std::string_view input(reinterpret_cast<const char*>(data), size);

  skydiag::dump_tool::AddressLibrary library;
  if (library.Parse(input)) {
    // A parsed file has entries, and every lookup stays inside them.
    if (library.Size() == 0u) {
      std::abort();
    }
    (void)library.IdAt(0);
    (void)library.IdAt(0x1000);
    (void)library.IdAt(~std::uint64_t{ 0 });
  } else if (library.Size() != 0u) {
    // A refused file leaves nothing behind.
    std::abort();
  }

  return 0;
}

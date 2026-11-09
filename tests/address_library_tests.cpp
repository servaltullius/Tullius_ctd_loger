// ADR-0027: the Address Library the helper puts in a dump, read back into
// offset -> ID (dump_tool/src/AddressLibrary.cpp).
#include "AddressLibrary.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

using skydiag::dump_tool::AddressLibrary;

void Require(bool condition, const char* message)
{
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

template <class T>
void Put(std::string& out, T value)
{
  char bytes[sizeof(T)];
  std::memcpy(bytes, &value, sizeof(T));
  out.append(bytes, sizeof(T));
}

// Formats 1 and 2: format, version[4], name length + name, pointer size, count.
std::string DeltaHeader(std::int32_t format, std::int32_t count)
{
  std::string out;
  Put<std::int32_t>(out, format);
  for (const std::uint32_t v : { 1u, 6u, 1170u, 0u }) {
    Put<std::uint32_t>(out, v);
  }
  Put<std::int32_t>(out, 4);
  out += "name";
  Put<std::int32_t>(out, 8);
  Put<std::int32_t>(out, count);
  return out;
}

void TestDeltaEncodedFormat()
{
  std::string bytes = DeltaHeader(2, 3);
  // ID 100 at 0x1000, both as full 64-bit values.
  bytes.push_back(static_cast<char>(0x00));
  Put<std::uint64_t>(bytes, 100);
  Put<std::uint64_t>(bytes, 0x1000);
  // ID + 1, offset + a byte: ID 101 at 0x1010.
  bytes.push_back(static_cast<char>(0x21));
  bytes.push_back(static_cast<char>(0x10));
  // ID + 1, offset in pointer units + a byte: (0x1010 / 8 + 2) * 8 = 0x1020.
  bytes.push_back(static_cast<char>(0xA1));
  bytes.push_back(static_cast<char>(0x02));

  AddressLibrary library;
  Require(library.Parse(bytes), "format 2 parses");
  Require(library.Format() == 2 && library.Size() == 3u, "three entries");
  Require(library.IdAt(0x1000) == 100u, "a full entry");
  Require(library.IdAt(0x1010) == 101u, "an offset relative to the previous one");
  Require(library.IdAt(0x1020) == 102u, "an offset in pointer units");
  Require(!library.IdAt(0x1008).has_value(), "only exact offsets map to an ID");

  std::string sse = bytes;
  sse[0] = 1;  // 1.5.x files are format 1, the same encoding
  Require(library.Parse(sse) && library.IdAt(0x1020) == 102u, "format 1 parses the same");

  Require(!library.Parse(bytes.substr(0, bytes.size() - 1)), "a file cut short is refused");
  Require(library.Size() == 0u && !library.IdAt(0x1000).has_value(), "and leaves nothing behind");
}

void TestDenseFormat()
{
  std::string bytes;
  Put<std::int32_t>(bytes, 5);
  for (const std::uint32_t v : { 1u, 7u, 104u, 0u }) {
    Put<std::uint32_t>(bytes, v);
  }
  bytes.append(64, '\0');
  Put<std::int32_t>(bytes, 8);
  Put<std::int32_t>(bytes, 0);
  Put<std::int32_t>(bytes, 4);
  for (const std::uint32_t offset : { 0u, 0x2000u, 0u, 0x1000u }) {
    Put<std::uint32_t>(bytes, offset);
  }

  AddressLibrary library;
  Require(library.Parse(bytes), "format 5 parses");
  Require(library.Format() == 5 && library.Size() == 2u, "IDs without an address are left out");
  Require(library.IdAt(0x2000) == 1u && library.IdAt(0x1000) == 3u, "the ID is the index");
  Require(!library.IdAt(0).has_value(), "0 is no address");
  Require(!library.Parse(bytes.substr(0, bytes.size() - 2)), "a table cut short is refused");
}

void TestUnknownInput()
{
  AddressLibrary library;
  Require(!library.Parse(""), "nothing");
  std::string unknown;
  Put<std::int32_t>(unknown, 3);
  unknown.append(64, '\0');
  Require(!library.Parse(unknown), "an unknown format");
  std::string huge = DeltaHeader(2, 0x7FFFFFFF);
  Require(!library.Parse(huge), "a count no real file has");
}

}  // namespace

// By hand, against a real file: address_library_tests <file> <offset hex> <id>
// (for example versionlib-1-6-1170-0.bin 4A05E0 29924, TESConditionItem::IsTrue).
int CheckRealFile(const char* path, const char* offsetHex, const char* id)
{
  std::FILE* f = std::fopen(path, "rb");
  Require(f != nullptr, "the file opens");
  std::string bytes;
  char buf[65536];
  for (std::size_t n = 0; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) {
    bytes.append(buf, n);
  }
  std::fclose(f);
  AddressLibrary library;
  Require(library.Parse(bytes), "the file parses");
  const auto found = library.IdAt(std::strtoull(offsetHex, nullptr, 16));
  std::printf("format %d, %zu IDs, ID at 0x%s: %s\n", library.Format(), library.Size(), offsetHex,
    found ? std::to_string(*found).c_str() : "none");
  Require(found == std::strtoull(id, nullptr, 10), "the expected ID");
  return 0;
}

int main(int argc, char** argv)
{
  if (argc == 4) {
    return CheckRealFile(argv[1], argv[2], argv[3]);
  }
  TestDeltaEncodedFormat();
  TestDenseFormat();
  TestUnknownInput();
  std::puts("address library tests passed");
  return 0;
}

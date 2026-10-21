// ThreadNamesStream reader on hand-built minidump bytes: names come back, and
// every count, RVA and length that points outside the file is refused.

#include "MinidumpThreadNames.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

using skydiag::dump_tool::minidump::LoadThreadNames;

namespace {

struct Name
{
  std::uint32_t tid = 0;
  std::u16string text;
};

void PutU32(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint32_t value)
{
  std::memcpy(bytes.data() + at, &value, sizeof(value));
}

void PutU64(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint64_t value)
{
  std::memcpy(bytes.data() + at, &value, sizeof(value));
}

// Header (32 bytes), a directory with one ThreadNamesStream entry, the name
// list, then each MINIDUMP_STRING.
struct Layout
{
  std::vector<std::uint8_t> bytes;
  std::size_t directory = 0;
  std::size_t stream = 0;
  std::vector<std::size_t> name_entries;  // offset of each MINIDUMP_THREAD_NAME
};

Layout BuildDump(const std::vector<Name>& names, std::uint32_t streamType = 24u)
{
  Layout layout;
  layout.directory = 32u;
  layout.stream = layout.directory + 12u;
  const std::size_t listSize = 4u + names.size() * 12u;
  std::size_t stringsAt = layout.stream + listSize;
  std::size_t total = stringsAt;
  for (const auto& name : names) {
    total += 4u + name.text.size() * 2u;
  }
  layout.bytes.assign(total, 0u);
  PutU32(layout.bytes, 0, 0x504D444Du);  // "MDMP"
  PutU32(layout.bytes, 8, 1u);           // NumberOfStreams
  PutU32(layout.bytes, 12, static_cast<std::uint32_t>(layout.directory));
  PutU32(layout.bytes, layout.directory, streamType);
  PutU32(layout.bytes, layout.directory + 4u, static_cast<std::uint32_t>(listSize));
  PutU32(layout.bytes, layout.directory + 8u, static_cast<std::uint32_t>(layout.stream));
  PutU32(layout.bytes, layout.stream, static_cast<std::uint32_t>(names.size()));
  for (std::size_t i = 0; i < names.size(); ++i) {
    const std::size_t entry = layout.stream + 4u + i * 12u;
    layout.name_entries.push_back(entry);
    PutU32(layout.bytes, entry, names[i].tid);
    PutU64(layout.bytes, entry + 4u, stringsAt);
    PutU32(layout.bytes, stringsAt, static_cast<std::uint32_t>(names[i].text.size() * 2u));
    std::memcpy(layout.bytes.data() + stringsAt + 4u, names[i].text.data(), names[i].text.size() * 2u);
    stringsAt += 4u + names[i].text.size() * 2u;
  }
  return layout;
}

std::vector<std::pair<std::uint32_t, std::u16string>> Load(const std::vector<std::uint8_t>& bytes)
{
  return LoadThreadNames(bytes.data(), bytes.size());
}

void TestNamesAreRead()
{
  const auto layout = BuildDump({ { 51944u, u"nv.sl.dlss_g.thread.dlssg" }, { 30100u, u"Main" } });
  const auto names = Load(layout.bytes);
  assert(names.size() == 2u);
  assert(names[0].first == 51944u && names[0].second == u"nv.sl.dlss_g.thread.dlssg");
  assert(names[1].first == 30100u && names[1].second == u"Main");
}

void TestMissingStreamOrHeader()
{
  const auto otherStream = BuildDump({ { 1u, u"a" } }, /*streamType=*/3u);
  assert(Load(otherStream.bytes).empty());
  auto badSignature = BuildDump({ { 1u, u"a" } });
  PutU32(badSignature.bytes, 0, 0u);
  assert(Load(badSignature.bytes).empty());
  assert(LoadThreadNames(nullptr, 100u).empty());
  const std::vector<std::uint8_t> tiny(8u, 0u);
  assert(Load(tiny).empty());
}

void TestOutOfBoundsDirectoryAndStream()
{
  // Directory beyond the file, or more streams than fit.
  auto layout = BuildDump({ { 1u, u"a" } });
  PutU32(layout.bytes, 12, static_cast<std::uint32_t>(layout.bytes.size()));
  assert(Load(layout.bytes).empty());
  layout = BuildDump({ { 1u, u"a" } });
  PutU32(layout.bytes, 8, 0x10000000u);
  assert(Load(layout.bytes).empty());

  // Stream RVA or size beyond the file.
  layout = BuildDump({ { 1u, u"a" } });
  PutU32(layout.bytes, layout.directory + 8u, 0xFFFFFFF0u);
  assert(Load(layout.bytes).empty());
  layout = BuildDump({ { 1u, u"a" } });
  PutU32(layout.bytes, layout.directory + 4u, static_cast<std::uint32_t>(layout.bytes.size()));
  assert(Load(layout.bytes).empty());

  // More names than the stream holds.
  layout = BuildDump({ { 1u, u"a" } });
  PutU32(layout.bytes, layout.stream, 2u);
  assert(Load(layout.bytes).empty());
  layout = BuildDump({ { 1u, u"a" } });
  PutU32(layout.bytes, layout.stream, 0xFFFFFFFFu);
  assert(Load(layout.bytes).empty());
}

void TestBadNameEntriesAreSkipped()
{
  // Name RVA past the end (also near 2^64), odd byte length, length past the
  // end: that entry is skipped, the good one stays.
  auto layout = BuildDump({ { 1u, u"bad" }, { 2u, u"good" } });
  PutU64(layout.bytes, layout.name_entries[0] + 4u, layout.bytes.size());
  auto names = Load(layout.bytes);
  assert(names.size() == 1u && names[0].first == 2u && names[0].second == u"good");

  layout = BuildDump({ { 1u, u"bad" }, { 2u, u"good" } });
  PutU64(layout.bytes, layout.name_entries[0] + 4u, ~std::uint64_t{ 0 } - 2u);
  names = Load(layout.bytes);
  assert(names.size() == 1u && names[0].first == 2u);

  layout = BuildDump({ { 1u, u"bad" }, { 2u, u"good" } });
  std::uint64_t firstString = 0;
  std::memcpy(&firstString, layout.bytes.data() + layout.name_entries[0] + 4u, sizeof(firstString));
  PutU32(layout.bytes, static_cast<std::size_t>(firstString), 5u);
  names = Load(layout.bytes);
  assert(names.size() == 1u && names[0].first == 2u);

  layout = BuildDump({ { 1u, u"good" }, { 2u, u"bad" } });
  std::uint64_t lastString = 0;
  std::memcpy(&lastString, layout.bytes.data() + layout.name_entries[1] + 4u, sizeof(lastString));
  PutU32(layout.bytes, static_cast<std::size_t>(lastString), 0x1000u);
  names = Load(layout.bytes);
  assert(names.size() == 1u && names[0].first == 1u && names[0].second == u"good");

  // An empty name and thread id 0 are no entries.
  layout = BuildDump({ { 1u, u"" }, { 0u, u"zero" }, { 3u, u"ok" } });
  names = Load(layout.bytes);
  assert(names.size() == 1u && names[0].first == 3u);
}

}  // namespace

int main()
{
  TestNamesAreRead();
  TestMissingStreamOrHeader();
  TestOutOfBoundsDirectoryAndStream();
  TestBadNameEntriesAreSkipped();
  return 0;
}

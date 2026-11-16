// ADR-0033: the game's FormID table lookup the helper runs at a crash and the
// analyzer runs again over the dump (shared/SkyrimDiagFormMap.h), over a
// table laid out in fake memory as the game lays it out.
#include "SkyrimDiagFormMap.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <vector>

namespace {

using skydiag::form_map::FindForm;
using skydiag::form_map::FormIdHash;

void Require(bool condition, const char* message)
{
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

// Blocks of bytes at addresses; a read must fall inside one block.
class FakeMemory
{
public:
  std::vector<std::uint8_t>& Block(std::uint64_t base, std::size_t size)
  {
    auto& block = blocks_[base];
    block.assign(size, 0);
    return block;
  }

  template <class T>
  void Put(std::uint64_t address, T value)
  {
    auto it = blocks_.upper_bound(address);
    Require(it != blocks_.begin(), "a write inside a block");
    --it;
    Require(address - it->first + sizeof(T) <= it->second.size(), "a write inside a block");
    std::memcpy(it->second.data() + (address - it->first), &value, sizeof(T));
  }

  void Forget(std::uint64_t base) { blocks_.erase(base); }

  bool operator()(std::uint64_t address, void* destination, std::size_t size) const
  {
    auto it = blocks_.upper_bound(address);
    if (it == blocks_.begin()) {
      return false;
    }
    --it;
    if (address - it->first + size > it->second.size()) {
      return false;
    }
    std::memcpy(destination, it->second.data() + (address - it->first), size);
    return true;
  }

private:
  std::map<std::uint64_t, std::vector<std::uint8_t>> blocks_;
};

constexpr std::uint64_t kSlot = 0x140001000ull;     // TESForm::allForms in the game's data
constexpr std::uint64_t kSentinel = 0x140002000ull;  // the game's static sentinel
constexpr std::uint64_t kMap = 0x200000000ull;
constexpr std::uint64_t kEntries = 0x300000000ull;
constexpr std::uint64_t kForms = 0x400000000ull;
constexpr std::uint32_t kCapacity = 8;

std::uint64_t EntryAt(std::uint32_t index)
{
  return kEntries + std::uint64_t{ index } * skydiag::form_map::kEntryBytes;
}

// The table with these forms, inserted as the game inserts them: into the
// key's own slot when it is free, else into a free slot linked in after it.
struct Table
{
  FakeMemory memory;
  std::map<std::uint32_t, std::uint64_t> forms;  // FormID -> form address

  explicit Table(const std::vector<std::uint32_t>& ids)
  {
    memory.Block(kSlot, 8);
    memory.Put<std::uint64_t>(kSlot, kMap);
    memory.Block(kMap, 0x30);
    memory.Put<std::uint32_t>(kMap + 0x0C, kCapacity);
    memory.Put<std::uint32_t>(kMap + 0x10, kCapacity - static_cast<std::uint32_t>(ids.size()));
    memory.Put<std::uint64_t>(kMap + 0x18, kSentinel);
    memory.Put<std::uint64_t>(kMap + 0x28, kEntries);
    memory.Block(kEntries, kCapacity * skydiag::form_map::kEntryBytes);
    std::vector<bool> used(kCapacity, false);
    std::uint64_t nextForm = kForms;
    for (const auto id : ids) {
      const std::uint64_t form = nextForm;
      nextForm += 0x100;
      memory.Block(form, 0x40);
      memory.Put<std::uint32_t>(form + 0x14, id);
      forms[id] = form;
      const std::uint32_t home = FormIdHash(id) & (kCapacity - 1u);
      std::uint32_t at = home;
      std::uint64_t next = kSentinel;
      if (used[home]) {
        at = kCapacity - 1u;
        while (used[at]) {
          --at;
        }
        std::uint64_t headNext = 0;
        Require(memory(EntryAt(home) + 0x10, &headNext, 8), "the head's link");
        next = headNext;
        memory.Put<std::uint64_t>(EntryAt(home) + 0x10, EntryAt(at));
      }
      used[at] = true;
      memory.Put<std::uint32_t>(EntryAt(at), id);
      memory.Put<std::uint64_t>(EntryAt(at) + 0x08, form);
      memory.Put<std::uint64_t>(EntryAt(at) + 0x10, next);
    }
  }
};

// Two FormIDs that hash to the same slot of an 8-slot table, and one whose
// slot neither takes.
struct Ids
{
  std::uint32_t first = 0;
  std::uint32_t second = 0;
  std::uint32_t emptySlot = 0;
};

Ids PickIds()
{
  Ids ids{};
  ids.first = 0x00012EB6u;
  const std::uint32_t slot = FormIdHash(ids.first) & (kCapacity - 1u);
  for (std::uint32_t id = 0x00010000u; ids.second == 0u || ids.emptySlot == 0u; ++id) {
    const std::uint32_t at = FormIdHash(id) & (kCapacity - 1u);
    if (id != ids.first && at == slot && ids.second == 0u) {
      ids.second = id;
    } else if (at != slot && ids.emptySlot == 0u) {
      ids.emptySlot = id;
    }
  }
  return ids;
}

void TestHash()
{
  // CommonLibSSE-NG's table (RE/C/CRC.h) starts and ends with these.
  Require(skydiag::form_map::detail::kCrcTable[1] == 0x77073096u, "the CRC table's second entry");
  Require(skydiag::form_map::detail::kCrcTable[255] == 0x2D02EF8Du, "the CRC table's last entry");
  // From zlib: CRC-32 of the four bytes, started at 0, not inverted at the end.
  Require(FormIdHash(0x00012EB6u) == 0x13D14BEDu, "the hash of ArmorIronShield's FormID");
  Require(FormIdHash(0x00000014u) == 0xDF7BC0C8u, "the hash of the player's FormID");
  Require(FormIdHash(0xFE000800u) == 0x54168EA3u, "the hash of a light plugin's FormID");
  Require(FormIdHash(0u) == 0u, "0 hashes to 0");
}

void TestIdsAndCandidates()
{
  Require(skydiag::form_map::AllFormsId(1) == 514351u, "SE 1.5.x");
  Require(skydiag::form_map::AllFormsId(2) == 400507u, "AE 1.6.x");
  Require(skydiag::form_map::AllFormsId(5) == 400507u, "1.7.x keeps the AE ID");
  Require(!skydiag::form_map::IsFormIdCandidate(0xFFFFu), "the null page says enough");
  Require(skydiag::form_map::IsFormIdCandidate(0x10000u), "the first value above it");
  Require(skydiag::form_map::IsFormIdCandidate(0xFFFFFFFFu), "the widest FormID");
  Require(!skydiag::form_map::IsFormIdCandidate(0x100000000ull), "wider than a FormID");
}

void TestLookup()
{
  const Ids ids = PickIds();
  Table table({ ids.first, ids.second });
  Require(FindForm(table.memory, kSlot, ids.first) == table.forms[ids.first], "the form in its own slot");
  Require(FindForm(table.memory, kSlot, ids.second) == table.forms[ids.second], "the form further down the chain");
  Require(!FindForm(table.memory, kSlot, ids.emptySlot).has_value(), "an empty slot");
  // Hashes to the taken slot, but is in no entry: the chain ends.
  std::uint32_t absent = ids.second + 1u;
  while ((FormIdHash(absent) & (kCapacity - 1u)) != (FormIdHash(ids.first) & (kCapacity - 1u))) {
    ++absent;
  }
  Require(!FindForm(table.memory, kSlot, absent).has_value(), "a chain without the key");
}

void TestDamagedTables()
{
  const Ids ids = PickIds();
  const std::uint32_t home = FormIdHash(ids.first) & (kCapacity - 1u);
  {
    Table table({ ids.first });
    table.memory.Put<std::uint32_t>(table.forms[ids.first] + 0x14, ids.first + 1u);
    Require(!FindForm(table.memory, kSlot, ids.first).has_value(), "a form whose own FormID differs");
  }
  {
    Table table({ ids.first });
    table.memory.Put<std::uint32_t>(kMap + 0x0C, 6u);
    Require(!FindForm(table.memory, kSlot, ids.first).has_value(), "a capacity that is no power of two");
    table.memory.Put<std::uint32_t>(kMap + 0x0C, 0u);
    Require(!FindForm(table.memory, kSlot, ids.first).has_value(), "no capacity");
    // 24 slots would still reach the form's entry (hash & 23 == hash & 7 for
    // this FormID): only the power-of-two check refuses it.
    Require((FormIdHash(ids.first) & 23u) == home, "the FormID reaches its entry with 24 slots too");
    table.memory.Put<std::uint32_t>(kMap + 0x0C, 24u);
    Require(!FindForm(table.memory, kSlot, ids.first).has_value(), "a capacity that is no power of two, entry reachable");
  }
  {
    Table table({ ids.first });
    table.memory.Forget(kSlot);
    Require(!FindForm(table.memory, kSlot, ids.first).has_value(), "the slot is not in the dump");
  }
  {
    Table table({ ids.first });
    table.memory.Put<std::uint64_t>(kSlot, 0x1234u);
    Require(!FindForm(table.memory, kSlot, ids.first).has_value(), "the slot holds no pointer");
  }
  {
    Table table({ ids.first, ids.second });
    // The second key's entry, reached from the head, now points outside the array.
    std::uint64_t chained = 0;
    Require(table.memory(EntryAt(home) + 0x10, &chained, 8), "the head's link");
    // Outside the array, readable, and holding an entry that would match.
    constexpr std::uint64_t kStray = 0x500000000ull;
    table.memory.Block(kStray, skydiag::form_map::kEntryBytes);
    table.memory.Put<std::uint32_t>(kStray, ids.second);
    table.memory.Put<std::uint64_t>(kStray + 0x08, table.forms[ids.second]);
    table.memory.Put<std::uint64_t>(kStray + 0x10, kSentinel);
    table.memory.Put<std::uint32_t>(chained, ids.second + 7u);
    table.memory.Put<std::uint64_t>(chained + 0x10, kStray);
    Require(!FindForm(table.memory, kSlot, ids.second).has_value(), "a link outside the entry array");
    // A link into the middle of an entry.
    table.memory.Put<std::uint64_t>(chained + 0x10, kEntries + 4u);
    Require(!FindForm(table.memory, kSlot, ids.second).has_value(), "a link into the middle of an entry");
    // A chain that loops back on itself ends after kMaxChain steps.
    table.memory.Put<std::uint64_t>(chained + 0x10, chained);
    Require(!FindForm(table.memory, kSlot, ids.second).has_value(), "a chain that loops");
  }
}

}  // namespace

int main()
{
  TestHash();
  TestIdsAndCandidates();
  TestLookup();
  TestDamagedTables();
  std::puts("form map tests passed");
  return 0;
}

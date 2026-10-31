#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace skydiag::dump_tool {

// ADR-0024: the hook stubs the helper read in the game's executable private
// memory while it ran (kMinidumpUserStream_HookPages).
struct RecordedHookStubInfo
{
  std::uint32_t offset = 0;
  std::uint32_t length = 0;
  std::string kind;
  std::uint64_t target = 0;
  std::wstring module;  // empty: the target lay outside every module
  std::uint64_t module_offset = 0;
};

struct RecordedHookPageInfo
{
  std::uint64_t base = 0;
  std::uint64_t size = 0;
  std::uint64_t last_seen_ms_ago = 0;
  std::vector<RecordedHookStubInfo> stubs;
};

struct HookPageRecordInfo
{
  bool present = false;
  std::uint32_t scans = 0;
  std::vector<RecordedHookPageInfo> pages;
};

// Fields of another type are skipped, not fatal; false when it is no record.
bool ParseHookPageRecordJson(std::string_view json, HookPageRecordInfo* out);

// Whose hook the freed code a crash jumped into was.
struct FreedHookInfo
{
  bool detected = false;  // the address lay in a recorded page
  bool exact = false;     // a recorded stub started at the address and named its owner
  std::uint64_t address = 0;
  std::uint64_t page_base = 0;
  std::uint64_t last_seen_ms_ago = 0;  // the page's last scan before the dump
  std::string stub_kind;               // exact only
  std::wstring owner_module;           // the stub's target module, or the page's main one
  std::uint64_t owner_offset = 0;      // exact only: where in owner_module the stub jumped
  std::wstring owner_module_path;      // from the dump's modules, when it still lists it
  std::wstring owner_mod_name;         // MO2 mod name, when known
  // Stubs per target module in the page, the most first.
  std::vector<std::pair<std::wstring, std::uint32_t>> page_modules;
};

// notOwner: a module no hook stub belongs to (the game EXE, Windows), whose
// stubs in the page do not name the owner.
FreedHookInfo ResolveFreedHook(
  const HookPageRecordInfo& record,
  std::uint64_t address,
  const std::function<bool(const std::wstring&)>& notOwner);

}  // namespace skydiag::dump_tool

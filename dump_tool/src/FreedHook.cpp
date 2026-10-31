#include "FreedHook.h"

#include "JsonValue.h"
#include "Utf.h"

#include <algorithm>
#include <cwctype>
#include <optional>

#include <nlohmann/json.hpp>

namespace skydiag::dump_tool {
namespace {

// "0x7FF6848B0000" as the helper writes addresses.
std::optional<std::uint64_t> ParseHex(const std::string& text)
{
  if (text.size() < 3u || text.size() > 18u || text[0] != '0' || (text[1] != 'x' && text[1] != 'X')) {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  for (std::size_t i = 2; i < text.size(); ++i) {
    const char c = text[i];
    std::uint64_t digit = 0;
    if (c >= '0' && c <= '9') {
      digit = static_cast<std::uint64_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = static_cast<std::uint64_t>(c - 'a') + 10u;
    } else if (c >= 'A' && c <= 'F') {
      digit = static_cast<std::uint64_t>(c - 'A') + 10u;
    } else {
      return std::nullopt;
    }
    value = (value << 4u) | digit;
  }
  return value;
}

bool SameModule(const std::wstring& a, const std::wstring& b)
{
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](wchar_t x, wchar_t y) {
    return std::towlower(x) == std::towlower(y);
  });
}

// The page the address lies in; of a page and one recorded over it later,
// the one seen last.
const RecordedHookPageInfo* PageOf(const HookPageRecordInfo& record, std::uint64_t address)
{
  const RecordedHookPageInfo* best = nullptr;
  for (const auto& page : record.pages) {
    if (address >= page.base && address - page.base < page.size &&
        (!best || page.last_seen_ms_ago < best->last_seen_ms_ago)) {
      best = &page;
    }
  }
  return best;
}

const RecordedHookStubInfo* StubAt(const RecordedHookPageInfo& page, std::uint64_t address)
{
  for (const auto& stub : page.stubs) {
    if (page.base + stub.offset == address) {
      return &stub;
    }
  }
  return nullptr;
}

}  // namespace

bool ParseHookPageRecordJson(std::string_view json, HookPageRecordInfo* out)
{
  if (!out) {
    return false;
  }
  *out = HookPageRecordInfo{};
  const auto j = nlohmann::json::parse(json.begin(), json.end(), nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded() || !j.is_object()) {
    return false;
  }
  out->present = true;
  out->scans = JsonValueOr(j, "scans", 0u);
  const auto pages = j.find("pages");
  if (pages == j.end() || !pages->is_array()) {
    return true;
  }
  for (const auto& p : *pages) {
    if (!p.is_object()) {
      continue;
    }
    const auto base = ParseHex(JsonValueOr(p, "base", std::string{}));
    const auto size = JsonValueOr(p, "size", std::uint64_t{ 0 });
    if (!base || size == 0u) {
      continue;
    }
    RecordedHookPageInfo page{};
    page.base = *base;
    page.size = size;
    page.last_seen_ms_ago = JsonValueOr(p, "last_seen_ms_ago", std::uint64_t{ 0 });
    const auto stubs = p.find("stubs");
    if (stubs != p.end() && stubs->is_array()) {
      for (const auto& s : *stubs) {
        const auto target = s.is_object() ? ParseHex(JsonValueOr(s, "target", std::string{})) : std::nullopt;
        if (!target) {
          continue;
        }
        RecordedHookStubInfo stub{};
        stub.offset = JsonValueOr(s, "offset", 0u);
        stub.length = JsonValueOr(s, "length", 0u);
        stub.kind = JsonValueOr(s, "kind", std::string{});
        stub.target = *target;
        stub.module = Utf8ToWide(JsonValueOr(s, "module", std::string{}));
        stub.module_offset = ParseHex(JsonValueOr(s, "module_offset", std::string{})).value_or(0u);
        page.stubs.push_back(std::move(stub));
      }
    }
    out->pages.push_back(std::move(page));
  }
  return true;
}

FreedHookInfo ResolveFreedHook(
  const HookPageRecordInfo& record,
  std::uint64_t address,
  const std::function<bool(const std::wstring&)>& notOwner)
{
  FreedHookInfo info{};
  const auto* page = PageOf(record, address);
  if (!page) {
    return info;
  }
  info.detected = true;
  info.address = address;
  info.page_base = page->base;
  info.last_seen_ms_ago = page->last_seen_ms_ago;
  const auto owns = [&](const std::wstring& module) { return !module.empty() && !(notOwner && notOwner(module)); };

  for (const auto& stub : page->stubs) {
    if (!owns(stub.module)) {
      continue;
    }
    const auto it = std::find_if(info.page_modules.begin(), info.page_modules.end(), [&](const auto& count) {
      return SameModule(count.first, stub.module);
    });
    if (it == info.page_modules.end()) {
      info.page_modules.emplace_back(stub.module, 1u);
    } else {
      ++it->second;
    }
  }
  std::stable_sort(info.page_modules.begin(), info.page_modules.end(), [](const auto& a, const auto& b) {
    return a.second > b.second;
  });

  // The stub at the address; one that jumps on into another recorded stub (a
  // chain of trampolines) is followed a few times to the module it ends in.
  const RecordedHookStubInfo* stub = StubAt(*page, address);
  for (int hop = 0; stub && stub->module.empty() && hop < 4; ++hop) {
    const auto* next = PageOf(record, stub->target);
    stub = next ? StubAt(*next, stub->target) : nullptr;
  }
  if (stub && owns(stub->module)) {
    info.exact = true;
    info.stub_kind = stub->kind;
    info.owner_module = stub->module;
    info.owner_offset = stub->module_offset;
  } else if (!info.page_modules.empty()) {
    info.owner_module = info.page_modules.front().first;
  }
  return info;
}

}  // namespace skydiag::dump_tool

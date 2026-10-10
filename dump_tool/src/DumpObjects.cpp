#include "DumpObjects.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cwchar>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_map>

#include "AnalyzerInternalsStackwalkPriv.h"
#include "CrashLoggerParseCore.h"
#include "SkyrimDiagRtti.h"
#include "Utf.h"

namespace skydiag::dump_tool {
namespace {

// Same as the helper (helper/src/CrashObjectMemory.h): the analyzer must look
// where the helper looked.
constexpr std::size_t kStackBytes = 0x800;

bool IsValidUtf8(std::string_view s)
{
  return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0) > 0;
}

// TESFile names are in the game's ANSI code page.
std::wstring FileNameToWide(const std::string& name)
{
  if (name.empty() || name.size() > 0x1000) {
    return {};
  }
  if (IsValidUtf8(name)) {
    return Utf8ToWide(name);
  }
  const int len = static_cast<int>(name.size());
  const int wideLen = MultiByteToWideChar(CP_ACP, 0, name.data(), len, nullptr, 0);
  if (wideLen <= 0) {
    return {};
  }
  std::wstring wide(static_cast<std::size_t>(wideLen), wchar_t{});
  MultiByteToWideChar(CP_ACP, 0, name.data(), len, wide.data(), wideLen);
  return wide;
}

std::wstring ModuleNameFor(const std::vector<minidump::ModuleInfo>& modules, std::uint64_t base)
{
  for (const auto& module : modules) {
    if (module.base == base) {
      return module.filename;
    }
  }
  return {};
}

struct Root
{
  std::string location;  // Crash Logger's spelling: "RBX", "RSP+68"
  std::uint64_t value = 0;
};

std::vector<Root> CollectRoots(const internal::stackwalk_internal::MinidumpMemoryView& memory, const CONTEXT& c)
{
  // The helper's order (helper/src/CrashObjectMemory.cpp).
  std::vector<Root> roots = {
    { "RAX", c.Rax }, { "RBX", c.Rbx }, { "RCX", c.Rcx }, { "RDX", c.Rdx }, { "RSI", c.Rsi },
    { "RDI", c.Rdi }, { "RBP", c.Rbp }, { "R8", c.R8 },   { "R9", c.R9 },   { "R10", c.R10 },
    { "R11", c.R11 }, { "R12", c.R12 }, { "R13", c.R13 }, { "R14", c.R14 }, { "R15", c.R15 },
  };
  for (std::size_t offset = 0; offset < kStackBytes; offset += 8) {
    std::uint64_t value = 0;
    std::size_t got = 0;
    if (!memory.Read(c.Rsp + offset, &value, sizeof(value), got) || got != sizeof(value)) {
      break;
    }
    char location[32]{};
    std::snprintf(location, sizeof(location), "RSP+%llX", static_cast<unsigned long long>(offset));
    roots.push_back(Root{ location, value });
  }
  return roots;
}

}  // namespace

std::wstring DescribeDumpObject(const DumpObject& object)
{
  std::wstring text = object.location + L": " + object.type_name;
  if (object.is_form) {
    wchar_t formId[16]{};
    std::swprintf(formId, std::size(formId), L"0x%08X", object.form_id);
    text += L" [" + std::wstring(formId) + L"]";
    // Defining plugin, then the last one that changes it.
    if (!object.source_files.empty()) {
      text += L" " + object.source_files.front();
      if (object.source_files.size() > 1u) {
        text += L" -> " + object.source_files.back();
      }
    }
  } else if (!object.module.empty()) {
    text += L" (" + object.module + L")";
  }
  return text;
}

std::vector<DumpObject> ReadDumpObjects(
  void* dumpBase,
  std::uint64_t dumpSize,
  const std::vector<minidump::ThreadRecord>& threads,
  const std::vector<minidump::ModuleInfo>& modules,
  const CONTEXT& context)
{
  internal::stackwalk_internal::MinidumpMemoryView memory;
  if (!memory.Init(dumpBase, dumpSize, &threads)) {
    return {};
  }
  // Dump memory only: the local module files must not stand in for what the
  // game held at the crash.
  const auto read = [&memory](std::uint64_t address, void* destination, std::size_t size) {
    std::size_t got = 0;
    return memory.Read(address, destination, size, got) && got == size;
  };

  std::vector<DumpObject> objects;
  std::unordered_map<std::uint64_t, std::size_t> indexByObject;
  for (const auto& root : CollectRoots(memory, context)) {
    const auto info = skydiag::rtti::ReadObject(read, root.value);
    if (!info) {
      continue;
    }
    const auto typeName = skydiag::rtti::UndecorateTypeName(info->type_name);
    const std::uint32_t relevance = crashlogger_core::LocationWeight(root.location) + crashlogger_core::TypeWeight(typeName);
    if (const auto it = indexByObject.find(info->complete_object); it != indexByObject.end()) {
      // The same object from another place: keep the most relevant one.
      auto& existing = objects[it->second];
      if (relevance > existing.relevance) {
        existing.location = Utf8ToWide(root.location);
        existing.relevance = relevance;
      }
      continue;
    }
    indexByObject.emplace(info->complete_object, objects.size());
    DumpObject object{};
    object.location = Utf8ToWide(root.location);
    object.address = info->complete_object;
    object.type_name = Utf8ToWide(typeName);
    object.module = ModuleNameFor(modules, info->module_base);
    object.is_form = info->is_form;
    object.form_id = info->form_id;
    object.form_type = info->form_type;
    for (const auto& file : info->source_files) {
      if (auto wide = FileNameToWide(file); !wide.empty()) {
        object.source_files.push_back(std::move(wide));
      }
    }
    object.relevance = relevance;
    objects.push_back(std::move(object));
  }
  std::stable_sort(objects.begin(), objects.end(), [](const DumpObject& a, const DumpObject& b) {
    return a.relevance > b.relevance;
  });
  if (objects.size() > kMaxDumpObjects) {
    objects.resize(kMaxDumpObjects);
  }
  return objects;
}

}  // namespace skydiag::dump_tool

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

// TESFile and form names: UTF-8 when they are valid UTF-8 (localized string
// files), otherwise the game's ANSI code page.
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

std::vector<std::wstring> FileNamesToWide(const std::vector<std::string>& names)
{
  std::vector<std::wstring> out;
  for (const auto& name : names) {
    if (auto wide = FileNameToWide(name); !wide.empty()) {
      out.push_back(std::move(wide));
    }
  }
  return out;
}

std::wstring FormIdText(std::uint32_t formId)
{
  wchar_t text[16]{};
  std::swprintf(text, std::size(text), L"0x%08X", formId);
  return text;
}

// Defining plugin, then the last one that changes it.
std::wstring FilesText(const std::vector<std::wstring>& files)
{
  if (files.empty()) {
    return {};
  }
  std::wstring text = files.front();
  if (files.size() > 1u) {
    text += L" -> " + files.back();
  }
  return text;
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
  if (!object.name.empty()) {
    text += L" \"" + object.name + L"\"";
  }
  if (object.is_form) {
    text += L" [" + FormIdText(object.form_id) + L"]";
    if (const auto files = FilesText(object.source_files); !files.empty()) {
      text += L" " + files;
    }
    if (object.has_base_object) {
      text += L" (base " + object.base_type_name + L" [" + FormIdText(object.base_form_id) + L"]";
      if (const auto files = FilesText(object.base_source_files); !files.empty()) {
        text += L" " + files;
      }
      text += L")";
    }
  } else if (!object.module.empty()) {
    text += L" (" + object.module + L")";
  }
  return text;
}

const DumpObject* FirstFormObject(const std::vector<DumpObject>& objects)
{
  const auto it = std::find_if(objects.begin(), objects.end(), [](const DumpObject& o) { return o.is_form; });
  return it != objects.end() ? &*it : nullptr;
}

std::wstring DescribeDumpObjectTarget(const DumpObject& object)
{
  std::wstring text = object.type_name;
  if (!object.name.empty()) {
    text += L" \"" + object.name + L"\"";
  }
  if (!object.is_form) {
    return text;
  }
  text += L" (FormID " + FormIdText(object.form_id);
  if (const auto files = FilesText(object.source_files); !files.empty()) {
    text += L", " + files;
  } else if (object.has_base_object) {
    text += L", base " + object.base_type_name + L" " + FormIdText(object.base_form_id);
    if (const auto baseFiles = FilesText(object.base_source_files); !baseFiles.empty()) {
      text += L" " + baseFiles;
    }
  }
  return text + L")";
}

std::uint32_t FreezeVerdictThreadId(const AnalysisResult& r)
{
  if (r.engine_spin.nan_detected) {
    return r.engine_spin.tid;
  }
  if (IsLockOwnerFreezeState(r.freeze_analysis.state_id) && r.main_thread_lock_owner.tid != 0u) {
    return r.main_thread_lock_owner.tid;
  }
  return r.main_thread_wait.blocked_thread_tid;
}

const DumpObject* FreezeVerdictForm(const AnalysisResult& r)
{
  const std::uint32_t tid = FreezeVerdictThreadId(r);
  if (tid == 0u) {
    return nullptr;
  }
  const auto it = std::find_if(r.freeze_thread_objects.begin(), r.freeze_thread_objects.end(),
    [tid](const FreezeThreadObjects& t) { return t.tid == tid; });
  return it != r.freeze_thread_objects.end() ? FirstFormObject(it->objects) : nullptr;
}

std::wstring FreezeTargetObjectClue(const AnalysisResult& r, bool en)
{
  const DumpObject* form = FreezeVerdictForm(r);
  if (!form) {
    return {};
  }
  const bool calculation = r.engine_spin.nan_detected;
  const std::wstring label = en
    ? (calculation ? L"The stuck calculation was handling: " : L"The stuck thread was handling: ")
    : (calculation ? L"멈춘 계산이 다루던 대상: " : L"멈춘 스레드가 다루던 대상: ");
  return label + DescribeDumpObjectTarget(*form);
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
    object.source_files = FileNamesToWide(info->source_files);
    object.name = FileNameToWide(info->full_name);
    if (info->has_base_object) {
      object.has_base_object = true;
      object.base_type_name = Utf8ToWide(skydiag::rtti::UndecorateTypeName(info->base_type_name));
      object.base_form_id = info->base_form_id;
      object.base_source_files = FileNamesToWide(info->base_source_files);
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

#include "MinidumpUtil.h"

#include "Mo2Index.h"
#include "Utf.h"

#include <algorithm>
#include <cwctype>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>

#include <nlohmann/json.hpp>

namespace skydiag::dump_tool::minidump {

namespace {

std::wstring LowerCopy(std::wstring_view s)
{
  std::wstring out(s);
  std::transform(out.begin(), out.end(), out.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
  return out;
}

bool StartsWith(std::wstring_view s, std::wstring_view prefix)
{
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool EndsWith(std::wstring_view s, std::wstring_view suffix)
{
  return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool IsSkseModuleLower(std::wstring_view lower)
{
  if (lower == L"skse64_loader.dll" || lower == L"skse64_steam_loader.dll" || lower == L"skse64.dll") {
    return true;
  }

  // SKSE runtime binaries usually follow skse64_<runtime>.dll, e.g. skse64_1_6_1170.dll.
  if (StartsWith(lower, L"skse64_") && EndsWith(lower, L".dll")) {
    const std::size_t runtimePos = std::size_t{ 7 };  // length of "skse64_"
    if (lower.size() > runtimePos + std::size_t{ 4 }) {
      const wchar_t c = lower[runtimePos];
      if (c >= L'0' && c <= L'9') {
        return true;
      }
    }
  }

  return false;
}

bool IsLikelyWindowsSystemModulePathLower(std::wstring_view lowerPath)
{
  if (lowerPath.empty()) {
    return false;
  }

  // Typical module paths in dumps:
  // - C:\Windows\System32\...
  // - \??\C:\Windows\System32\...
  // - \SystemRoot\System32\...
  const bool hasWindowsSystemDir =
    (lowerPath.find(L"\\windows\\system32\\") != std::wstring::npos) ||
    (lowerPath.find(L"\\windows\\syswow64\\") != std::wstring::npos) ||
    (lowerPath.find(L"\\windows\\winsxs\\") != std::wstring::npos) ||
    (lowerPath.find(L"\\systemroot\\system32\\") != std::wstring::npos);
  return hasWindowsSystemDir;
}

std::vector<std::wstring> DefaultHookFrameworkDlls()
{
  return {
    L"enginefixes.dll",
    L"ssedisplaytweaks.dll",
    L"po3_tweaks.dll",
    L"hdtssephysics.dll",
    L"hdtsmp64.dll",
    L"storageutil.dll",
    L"crashlogger.dll",
    L"crashloggersse.dll",
    L"sl.interposer.dll",
    L"skse64.dll",
    L"skse64_loader.dll",
    L"skse64_steam_loader.dll",
  };
}

std::string ModuleVersionString(const VS_FIXEDFILEINFO& info)
{
  if (info.dwSignature != VS_FFI_SIGNATURE) {
    return {};
  }
  const auto major = static_cast<unsigned>(HIWORD(info.dwFileVersionMS));
  const auto minor = static_cast<unsigned>(LOWORD(info.dwFileVersionMS));
  const auto build = static_cast<unsigned>(HIWORD(info.dwFileVersionLS));
  const auto revision = static_cast<unsigned>(LOWORD(info.dwFileVersionLS));
  return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(build) + "." + std::to_string(revision);
}

std::mutex g_hookFrameworksMutex;
std::vector<std::wstring> g_hookFrameworkDlls = DefaultHookFrameworkDlls();

}  // namespace

bool MappedFile::Open(const std::wstring& path, std::wstring* err)
{
  Close();

  file = CreateFileW(
    path.c_str(),
    GENERIC_READ,
    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
    nullptr,
    OPEN_EXISTING,
    FILE_ATTRIBUTE_NORMAL,
    nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    if (err) *err = L"CreateFileW failed: " + std::to_wstring(GetLastError());
    return false;
  }

  LARGE_INTEGER sz{};
  if (!GetFileSizeEx(file, &sz)) {
    const DWORD le = GetLastError();
    CloseHandle(file);
    file = INVALID_HANDLE_VALUE;
    if (err) *err = L"GetFileSizeEx failed: " + std::to_wstring(le);
    return false;
  }
  if (sz.QuadPart <= 0) {
    CloseHandle(file);
    file = INVALID_HANDLE_VALUE;
    if (err) *err = L"File is empty";
    return false;
  }

  size = static_cast<std::uint64_t>(sz.QuadPart);
  mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
  if (!mapping) {
    const DWORD le = GetLastError();
    CloseHandle(file);
    file = INVALID_HANDLE_VALUE;
    if (err) *err = L"CreateFileMappingW failed: " + std::to_wstring(le);
    return false;
  }

  view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
  if (!view) {
    const DWORD le = GetLastError();
    CloseHandle(mapping);
    mapping = nullptr;
    CloseHandle(file);
    file = INVALID_HANDLE_VALUE;
    if (err) *err = L"MapViewOfFile failed: " + std::to_wstring(le);
    return false;
  }

  if (err) err->clear();
  return true;
}

void MappedFile::Close() noexcept
{
  if (view) {
    UnmapViewOfFile(view);
    view = nullptr;
  }
  if (mapping) {
    CloseHandle(mapping);
    mapping = nullptr;
  }
  if (file != INVALID_HANDLE_VALUE) {
    CloseHandle(file);
    file = INVALID_HANDLE_VALUE;
  }
  size = 0;
}

MappedFile::~MappedFile()
{
  Close();
}

bool ReadStreamSized(void* dumpBase, std::uint64_t dumpSize, std::uint32_t streamType, void** outPtr, ULONG* outSize)
{
  if (!dumpBase || !outPtr || !outSize || dumpSize < sizeof(MINIDUMP_HEADER)) {
    return false;
  }

  const auto* hdr = static_cast<const MINIDUMP_HEADER*>(dumpBase);
  if (hdr->Signature != MINIDUMP_SIGNATURE) {
    return false;
  }

  const std::uint64_t dirOff = static_cast<std::uint64_t>(hdr->StreamDirectoryRva);
  const std::uint64_t n = static_cast<std::uint64_t>(hdr->NumberOfStreams);
  if (dirOff > dumpSize) {
    return false;
  }
  const std::uint64_t maxDirs = (dumpSize - dirOff) / sizeof(MINIDUMP_DIRECTORY);
  if (n == 0 || n > maxDirs) {
    return false;
  }

  const auto* base = static_cast<const std::uint8_t*>(dumpBase);
  const auto* dirs = reinterpret_cast<const MINIDUMP_DIRECTORY*>(base + dirOff);

  for (std::uint64_t i = 0; i < n; i++) {
    if (dirs[i].StreamType != streamType) {
      continue;
    }

    const std::uint64_t rva = static_cast<std::uint64_t>(dirs[i].Location.Rva);
    const std::uint64_t sz = static_cast<std::uint64_t>(dirs[i].Location.DataSize);
    if (rva > dumpSize) {
      return false;
    }
    if (sz > (dumpSize - rva)) {
      return false;
    }

    *outPtr = const_cast<std::uint8_t*>(base + rva);
    *outSize = static_cast<ULONG>(sz);
    return true;
  }

  return false;
}

bool ReadMinidumpStringUtf8(void* dumpBase, std::uint64_t dumpSize, RVA rva, std::string& out)
{
  out.clear();
  if (!dumpBase || dumpSize < sizeof(std::uint32_t) || rva == 0) {
    return false;
  }

  const std::uint64_t off = static_cast<std::uint64_t>(rva);
  if (off + sizeof(std::uint32_t) > dumpSize) {
    return false;
  }

  const auto* base = static_cast<const std::uint8_t*>(dumpBase);
  const auto* lenPtr = reinterpret_cast<const std::uint32_t*>(base + off);
  const std::uint32_t lenBytes = *lenPtr;

  if ((lenBytes % 2u) != 0u) {
    return false;
  }
  if (off + sizeof(std::uint32_t) + lenBytes > dumpSize) {
    return false;
  }

  const auto* wbuf = reinterpret_cast<const wchar_t*>(base + off + sizeof(std::uint32_t));
  const std::wstring_view w(wbuf, lenBytes / sizeof(wchar_t));
  out = WideToUtf8(w);
  return true;
}

std::optional<ModuleHit> ModuleForAddress(void* dumpBase, std::uint64_t dumpSize, std::uint64_t addr)
{
  void* ptr = nullptr;
  ULONG size = 0;
  if (!ReadStreamSized(dumpBase, dumpSize, ModuleListStream, &ptr, &size)) {
    return std::nullopt;
  }
  if (size < sizeof(MINIDUMP_MODULE_LIST)) {
    return std::nullopt;
  }

  const auto* list = static_cast<const MINIDUMP_MODULE_LIST*>(ptr);
  const std::uint64_t need =
    static_cast<std::uint64_t>(offsetof(MINIDUMP_MODULE_LIST, Modules)) +
    static_cast<std::uint64_t>(list->NumberOfModules) * static_cast<std::uint64_t>(sizeof(MINIDUMP_MODULE));
  if (need > static_cast<std::uint64_t>(size)) {
    return std::nullopt;
  }

  for (ULONG i = 0; i < list->NumberOfModules; i++) {
    const auto& mod = list->Modules[i];
    const std::uint64_t base = mod.BaseOfImage;
    const std::uint64_t end = base + mod.SizeOfImage;
    if (addr < base || addr >= end) {
      continue;
    }

    std::string utf8;
    if (!ReadMinidumpStringUtf8(dumpBase, dumpSize, mod.ModuleNameRva, utf8)) {
      return std::nullopt;
    }

    const std::wstring wpath = Utf8ToWide(utf8);
    std::filesystem::path p(wpath);
    const auto file = p.filename().wstring();

    const std::uint64_t off = addr - base;
    wchar_t buf[1024]{};
    swprintf_s(buf, L"%s+0x%llx", file.c_str(), static_cast<unsigned long long>(off));

    ModuleHit hit{};
    hit.base = base;
    hit.path = wpath;
    hit.filename = file;
    hit.plusOffset = buf;
    return hit;
  }

  return std::nullopt;
}

std::wstring WideLower(std::wstring_view s)
{
  std::wstring out(s);
  std::transform(out.begin(), out.end(), out.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
  return out;
}

bool IsSystemishModule(std::wstring_view filename)
{
  std::wstring lower(filename);
  std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });

  const wchar_t* k[] = {
    L"kernelbase.dll", L"ntdll.dll",     L"kernel32.dll",  L"ucrtbase.dll",
    L"msvcp140.dll",   L"vcruntime140.dll", L"vcruntime140_1.dll", L"concrt140.dll", L"user32.dll",
    L"gdi32.dll",      L"combase.dll",   L"ole32.dll",     L"ws2_32.dll",
    L"win32u.dll",
  };
  for (const auto* m : k) {
    if (lower == m) {
      return true;
    }
  }
  return false;
}

bool IsLikelyWindowsSystemModulePath(std::wstring_view modulePath)
{
  const std::wstring lower = LowerCopy(modulePath);
  return IsLikelyWindowsSystemModulePathLower(lower);
}

bool IsGameExeModule(std::wstring_view filename)
{
  const std::wstring lower = WideLower(filename);
  return (lower == L"skyrimse.exe" || lower == L"skyrimae.exe" || lower == L"skyrimvr.exe" || lower == L"skyrim.exe");
}

void LoadHookFrameworksFromJson(const std::filesystem::path& jsonPath)
{
  if (jsonPath.empty()) {
    return;
  }

  try {
    std::ifstream f(jsonPath);
    if (!f.is_open()) {
      return;
    }

    const auto j = nlohmann::json::parse(f, nullptr, /*allow_exceptions=*/true);
    if (!j.is_object() || !j.contains("frameworks") || !j["frameworks"].is_array()) {
      return;
    }

    std::vector<std::wstring> loaded;
    loaded.reserve(j["frameworks"].size());
    for (const auto& fw : j["frameworks"]) {
      if (!fw.is_object()) {
        continue;
      }
      const auto it = fw.find("dll");
      if (it == fw.end() || !it->is_string()) {
        continue;
      }
      const std::wstring lower = LowerCopy(Utf8ToWide(it->get<std::string>()));
      if (!lower.empty()) {
        loaded.push_back(lower);
      }
    }

    if (!loaded.empty()) {
      std::lock_guard<std::mutex> lock(g_hookFrameworksMutex);
      g_hookFrameworkDlls = std::move(loaded);
    }
  } catch (...) {
    // Keep defaults on parse/IO errors.
  }
}

bool IsKnownHookFramework(std::wstring_view filename)
{
  const std::wstring lower = LowerCopy(filename);
  if (IsSkseModuleLower(lower)) {
    return true;
  }
  std::lock_guard<std::mutex> lock(g_hookFrameworksMutex);
  for (const auto& m : g_hookFrameworkDlls) {
    if (lower == m) {
      return true;
    }
  }
  return false;
}

bool IsSkseModule(std::wstring_view filename)
{
  return IsSkseModuleLower(LowerCopy(filename));
}

std::vector<ModuleInfo> LoadAllModules(void* dumpBase, std::uint64_t dumpSize)
{
  std::vector<ModuleInfo> out;

  void* ptr = nullptr;
  ULONG size = 0;
  if (!ReadStreamSized(dumpBase, dumpSize, ModuleListStream, &ptr, &size) || !ptr || size < sizeof(MINIDUMP_MODULE_LIST)) {
    return out;
  }

  const auto* list = static_cast<const MINIDUMP_MODULE_LIST*>(ptr);
  const std::uint64_t need =
    static_cast<std::uint64_t>(offsetof(MINIDUMP_MODULE_LIST, Modules)) +
    static_cast<std::uint64_t>(list->NumberOfModules) * static_cast<std::uint64_t>(sizeof(MINIDUMP_MODULE));
  if (need > static_cast<std::uint64_t>(size)) {
    return out;
  }

  out.reserve(list->NumberOfModules);
  for (ULONG i = 0; i < list->NumberOfModules; i++) {
    const auto& mod = list->Modules[i];
    std::string utf8;
    if (!ReadMinidumpStringUtf8(dumpBase, dumpSize, mod.ModuleNameRva, utf8)) {
      continue;
    }

    ModuleInfo mi{};
    mi.base = mod.BaseOfImage;
    mi.end = mi.base + mod.SizeOfImage;
    mi.version = ModuleVersionString(mod.VersionInfo);
    mi.path = Utf8ToWide(utf8);
    mi.filename = std::filesystem::path(mi.path).filename().wstring();
    mi.inferred_mod_name = InferMo2ModNameFromPath(mi.path);
    mi.is_systemish = IsSystemishModule(mi.filename) || IsLikelyWindowsSystemModulePathLower(WideLower(mi.path));
    mi.is_game_exe = IsGameExeModule(mi.filename);
    mi.is_known_hook_framework = IsKnownHookFramework(mi.filename);
    out.push_back(std::move(mi));
  }

  std::sort(out.begin(), out.end(), [](const ModuleInfo& a, const ModuleInfo& b) { return a.base < b.base; });
  return out;
}

std::optional<std::size_t> FindModuleIndexForAddress(const std::vector<ModuleInfo>& mods, std::uint64_t addr)
{
  if (mods.empty()) {
    return std::nullopt;
  }

  // lower_bound by base, then check previous element.
  const auto it = std::upper_bound(mods.begin(), mods.end(), addr, [](std::uint64_t value, const ModuleInfo& m) {
    return value < m.base;
  });

  if (it == mods.begin()) {
    return std::nullopt;
  }
  const auto& cand = *(it - 1);
  if (addr >= cand.base && addr < cand.end) {
    return static_cast<std::size_t>(std::distance(mods.begin(), it - 1));
  }
  return std::nullopt;
}

std::vector<ThreadRecord> LoadThreads(void* dumpBase, std::uint64_t dumpSize)
{
  std::vector<ThreadRecord> out;

  void* ptr = nullptr;
  ULONG size = 0;
  if (!ReadStreamSized(dumpBase, dumpSize, ThreadListStream, &ptr, &size) || !ptr || size < sizeof(MINIDUMP_THREAD_LIST)) {
    return out;
  }

  const auto* list = static_cast<const MINIDUMP_THREAD_LIST*>(ptr);
  const std::uint64_t need =
    static_cast<std::uint64_t>(offsetof(MINIDUMP_THREAD_LIST, Threads)) +
    static_cast<std::uint64_t>(list->NumberOfThreads) * static_cast<std::uint64_t>(sizeof(MINIDUMP_THREAD));
  if (need > static_cast<std::uint64_t>(size)) {
    return out;
  }

  out.reserve(list->NumberOfThreads);
  for (ULONG i = 0; i < list->NumberOfThreads; i++) {
    const auto& th = list->Threads[i];
    ThreadRecord tr{};
    tr.tid = th.ThreadId;
    tr.stack = th.Stack;
    tr.context = th.ThreadContext;
    out.push_back(std::move(tr));
  }
  return out;
}

bool ReadThreadContextWin64(void* dumpBase, std::uint64_t dumpSize, const ThreadRecord& tr, CONTEXT& out)
{
  if (!dumpBase) {
    return false;
  }
  const std::uint64_t rva = static_cast<std::uint64_t>(tr.context.Rva);
  const std::uint64_t sz = static_cast<std::uint64_t>(tr.context.DataSize);
  if (rva == 0 || sz == 0 || rva > dumpSize || sz > (dumpSize - rva)) {
    return false;
  }

  const auto* base = static_cast<const std::uint8_t*>(dumpBase);
  const std::size_t copyN = static_cast<std::size_t>(std::min<std::uint64_t>(sz, sizeof(CONTEXT)));
  std::memset(&out, 0, sizeof(out));
  std::memcpy(&out, base + rva, copyN);
  return true;
}

bool GetThreadStackBytes(
  void* dumpBase,
  std::uint64_t dumpSize,
  const ThreadRecord& tr,
  const std::uint8_t*& outPtr,
  std::size_t& outSize,
  std::uint64_t& outBaseAddr)
{
  outPtr = nullptr;
  outSize = 0;
  outBaseAddr = 0;

  const std::uint64_t rva = static_cast<std::uint64_t>(tr.stack.Memory.Rva);
  const std::uint64_t sz = static_cast<std::uint64_t>(tr.stack.Memory.DataSize);
  if (!dumpBase || rva == 0 || sz == 0 || rva > dumpSize || sz > (dumpSize - rva)) {
    return false;
  }

  const auto* base = static_cast<const std::uint8_t*>(dumpBase);
  outPtr = base + rva;
  outSize = static_cast<std::size_t>(sz);
  outBaseAddr = tr.stack.StartOfMemoryRange;
  return true;
}

}  // namespace skydiag::dump_tool::minidump

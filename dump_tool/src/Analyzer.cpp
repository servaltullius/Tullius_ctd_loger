#include "Analyzer.h"
#include "EvidenceBuilder.h"
#include "CrashLogger.h"
#include "Mo2Index.h"

#include <Windows.h>

#include <DbgHelp.h>

#include <algorithm>
#include <cwctype>
#include <cstddef>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "SkyrimDiagProtocol.h"
#include "SkyrimDiagShared.h"

namespace skydiag::dump_tool {
namespace {

std::wstring ToWide(std::string_view s)
{
  if (s.empty()) {
    return {};
  }

  const int needed = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  if (needed <= 0) {
    return {};
  }

  std::wstring out(static_cast<std::size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed);
  return out;
}

std::string WideToUtf8(std::wstring_view w)
{
  if (w.empty()) {
    return {};
  }
  const int needed =
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  if (needed <= 0) {
    return {};
  }
  std::string out(static_cast<std::size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), needed, nullptr, nullptr);
  return out;
}

struct MappedFile
{
  HANDLE file = INVALID_HANDLE_VALUE;
  HANDLE mapping = nullptr;
  void* view = nullptr;
  std::uint64_t size = 0;

  bool Open(const std::wstring& path, std::wstring* err)
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

  void Close() noexcept
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

  ~MappedFile()
  {
    Close();
  }
};

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

struct ModuleHit
{
  std::wstring path;
  std::wstring filename;
  std::wstring plusOffset;
};

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

    const std::wstring wpath = ToWide(utf8);
    std::filesystem::path p(wpath);
    const auto file = p.filename().wstring();

    const std::uint64_t off = addr - base;
    wchar_t buf[1024]{};
    swprintf_s(buf, L"%s+0x%llx", file.c_str(), static_cast<unsigned long long>(off));

    ModuleHit hit{};
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
  };
  for (const auto* m : k) {
    if (lower == m) {
      return true;
    }
  }
  return false;
}

bool IsGameExeModule(std::wstring_view filename)
{
  const std::wstring lower = WideLower(filename);
  return (lower == L"skyrimse.exe" || lower == L"skyrimae.exe" || lower == L"skyrimvr.exe" || lower == L"skyrim.exe");
}

struct ModuleInfo
{
  std::uint64_t base = 0;
  std::uint64_t end = 0;
  std::wstring path;
  std::wstring filename;
  std::wstring inferred_mod_name;
  bool is_systemish = false;
  bool is_game_exe = false;
};

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
    mi.path = ToWide(utf8);
    mi.filename = std::filesystem::path(mi.path).filename().wstring();
    mi.inferred_mod_name = InferMo2ModNameFromPath(mi.path);
    mi.is_systemish = IsSystemishModule(mi.filename);
    mi.is_game_exe = IsGameExeModule(mi.filename);
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

std::wstring EventTypeName(std::uint16_t t)
{
  using skydiag::EventType;
  switch (static_cast<EventType>(t)) {
    case EventType::kSessionStart: return L"SessionStart";
    case EventType::kHeartbeat: return L"Heartbeat";
    case EventType::kMenuOpen: return L"MenuOpen";
    case EventType::kMenuClose: return L"MenuClose";
    case EventType::kLoadStart: return L"LoadStart";
    case EventType::kLoadEnd: return L"LoadEnd";
    case EventType::kCellChange: return L"CellChange";
    case EventType::kNote: return L"Note";
    case EventType::kPerfHitch: return L"PerfHitch";
    case EventType::kCrash: return L"Crash";
    case EventType::kHangMark: return L"HangMark";
    default: return L"Unknown";
  }
}

std::wstring ConfidenceHigh() { return L"높음"; }
std::wstring ConfidenceMid() { return L"중간"; }
std::wstring ConfidenceLow() { return L"낮음"; }

std::vector<std::uint32_t> ExtractWctCandidateThreadIds(std::string_view wctJsonUtf8, std::size_t maxN)
{
  std::vector<std::uint32_t> tids;
  if (wctJsonUtf8.empty()) {
    return tids;
  }

  try {
    const auto j = nlohmann::json::parse(wctJsonUtf8, nullptr, /*allow_exceptions=*/true);
    if (!j.is_object()) {
      return tids;
    }
    const auto it = j.find("threads");
    if (it == j.end() || !it->is_array()) {
      return tids;
    }
    struct Row
    {
      std::uint32_t tid = 0;
      std::uint64_t waitTime = 0;
    };
    std::vector<Row> nonCycle;

    for (const auto& t : *it) {
      if (!t.is_object()) {
        continue;
      }
      const auto tid = t.value("tid", 0u);
      if (tid == 0u) {
        continue;
      }
      const bool isCycle = t.value("isCycle", false);
      if (isCycle) {
        tids.push_back(tid);
        continue;
      }

      std::uint64_t waitTime = 0;
      const auto nodesIt = t.find("nodes");
      if (nodesIt != t.end() && nodesIt->is_array()) {
        for (const auto& node : *nodesIt) {
          if (!node.is_object()) {
            continue;
          }
          const auto thIt = node.find("thread");
          if (thIt == node.end() || !thIt->is_object()) {
            continue;
          }
          waitTime = std::max<std::uint64_t>(waitTime, thIt->value("waitTime", 0ull));
        }
      }
      nonCycle.push_back(Row{ tid, waitTime });
    }

    // If cycles exist, prioritize them (deadlock likely).
    if (!tids.empty()) {
      return tids;
    }

    // Otherwise pick the longest-waiting threads (best-effort).
    if (maxN == 0) {
      return tids;
    }
    std::sort(nonCycle.begin(), nonCycle.end(), [](const Row& a, const Row& b) { return a.waitTime > b.waitTime; });
    for (const auto& r : nonCycle) {
      if (r.tid == 0u) {
        continue;
      }
      tids.push_back(r.tid);
      if (tids.size() >= maxN) {
        break;
      }
    }
  } catch (...) {
    return tids;
  }
  return tids;
}

struct WctCaptureDecision
{
  bool has = false;
  std::string kind;
  double secondsSinceHeartbeat = 0.0;
  std::uint32_t thresholdSec = 0;
  bool isLoading = false;
};

std::optional<WctCaptureDecision> TryParseWctCaptureDecision(std::string_view wctJsonUtf8)
{
  if (wctJsonUtf8.empty()) {
    return std::nullopt;
  }
  try {
    const auto j = nlohmann::json::parse(wctJsonUtf8, nullptr, /*allow_exceptions=*/true);
    if (!j.is_object()) {
      return std::nullopt;
    }
    const auto it = j.find("capture");
    if (it == j.end() || !it->is_object()) {
      return std::nullopt;
    }

    WctCaptureDecision d{};
    d.has = true;
    d.kind = it->value("kind", std::string{});
    d.secondsSinceHeartbeat = it->value("secondsSinceHeartbeat", 0.0);
    d.thresholdSec = it->value("thresholdSec", 0u);
    d.isLoading = it->value("isLoading", false);
    return d;
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<std::uint32_t> InferMainThreadIdFromEvents(const std::vector<EventRow>& events)
{
  for (auto it = events.rbegin(); it != events.rend(); ++it) {
    if (it->type == static_cast<std::uint16_t>(skydiag::EventType::kHeartbeat) && it->tid != 0) {
      return it->tid;
    }
  }
  return std::nullopt;
}

std::optional<double> InferHeartbeatAgeFromEventsSec(const std::vector<EventRow>& events)
{
  if (events.empty()) {
    return std::nullopt;
  }

  double maxMs = events[0].t_ms;
  double lastHbMs = -1.0;
  for (const auto& e : events) {
    maxMs = std::max(maxMs, e.t_ms);
    if (e.type == static_cast<std::uint16_t>(skydiag::EventType::kHeartbeat)) {
      lastHbMs = std::max(lastHbMs, e.t_ms);
    }
  }
  if (lastHbMs < 0.0) {
    return std::nullopt;
  }

  const double ageMs = std::max(0.0, maxMs - lastHbMs);
  return ageMs / 1000.0;
}

std::wstring ResourceKindFromPath(std::wstring_view path)
{
  const std::filesystem::path p(path);
  const std::wstring ext = WideLower(p.extension().wstring());
  if (ext == L".nif") {
    return L"nif";
  }
  if (ext == L".hkx") {
    return L"hkx";
  }
  if (ext == L".tri") {
    return L"tri";
  }
  if (!ext.empty()) {
    return ext.substr(1);
  }
  return L"(unknown)";
}

struct ThreadRecord
{
  std::uint32_t tid = 0;
  MINIDUMP_MEMORY_DESCRIPTOR stack{};
  MINIDUMP_LOCATION_DESCRIPTOR context{};
};

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

bool GetThreadStackBytes(void* dumpBase, std::uint64_t dumpSize, const ThreadRecord& tr, const std::uint8_t*& outPtr, std::size_t& outSize, std::uint64_t& outBaseAddr)
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

std::wstring ConfidenceForTopSuspect(std::uint32_t topScore, std::uint32_t secondScore)
{
  if (topScore >= 128u || (topScore >= 48u && topScore >= (secondScore * 2u))) {
    return ConfidenceHigh();
  }
  if (topScore >= 20u) {
    return ConfidenceMid();
  }
  return ConfidenceLow();
}

std::vector<SuspectItem> ComputeStackScanSuspects(
  void* dumpBase,
  std::uint64_t dumpSize,
  const std::vector<ModuleInfo>& modules,
  const std::vector<std::uint32_t>& targetTids)
{
  std::vector<SuspectItem> out;
  if (!dumpBase || modules.empty() || targetTids.empty()) {
    return out;
  }

  const auto threads = LoadThreads(dumpBase, dumpSize);
  if (threads.empty()) {
    return out;
  }

  std::unordered_map<std::size_t, std::uint32_t> scoreByModule;

  constexpr std::size_t kMaxScanBytes = 96 * 1024;
  for (const auto tid : targetTids) {
    const auto it = std::find_if(threads.begin(), threads.end(), [&](const ThreadRecord& tr) { return tr.tid == tid; });
    if (it == threads.end()) {
      continue;
    }

    CONTEXT ctx{};
    if (!ReadThreadContextWin64(dumpBase, dumpSize, *it, ctx)) {
      continue;
    }
    const std::uint64_t sp = ctx.Rsp;

    const std::uint8_t* stackBytes = nullptr;
    std::size_t stackSize = 0;
    std::uint64_t stackBase = 0;
    if (!GetThreadStackBytes(dumpBase, dumpSize, *it, stackBytes, stackSize, stackBase)) {
      continue;
    }

    std::size_t startOff = 0;
    if (sp >= stackBase && sp < stackBase + static_cast<std::uint64_t>(stackSize)) {
      startOff = static_cast<std::size_t>(sp - stackBase);
    }
    const std::size_t endOff = std::min<std::size_t>(stackSize, startOff + kMaxScanBytes);

    for (std::size_t off = startOff; off + sizeof(std::uint64_t) <= endOff; off += sizeof(std::uint64_t)) {
      std::uint64_t val = 0;
      std::memcpy(&val, stackBytes + off, sizeof(val));
      auto mi = FindModuleIndexForAddress(modules, val);
      if (!mi) {
        continue;
      }
      scoreByModule[*mi] += 1;
    }
  }

  struct Row
  {
    std::size_t modIndex = 0;
    std::uint32_t score = 0;
  };
  std::vector<Row> rows;
  rows.reserve(scoreByModule.size());
  for (const auto& [idx, score] : scoreByModule) {
    const auto& m = modules[idx];
    if (m.is_systemish || m.is_game_exe) {
      continue;
    }
    rows.push_back(Row{ idx, score });
  }

  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.score > b.score; });

  if (rows.empty()) {
    return out;
  }

  const std::uint32_t topScore = rows[0].score;
  const std::uint32_t secondScore = (rows.size() > 1) ? rows[1].score : 0;
  const std::wstring conf = ConfidenceForTopSuspect(topScore, secondScore);

  const std::size_t n = std::min<std::size_t>(rows.size(), 5);
  out.reserve(n);
  for (std::size_t i = 0; i < n; i++) {
    const auto& row = rows[i];
    const auto& m = modules[row.modIndex];

    SuspectItem si{};
    si.confidence = (i == 0) ? conf : ConfidenceMid();
    si.module_filename = m.filename;
    si.module_path = m.path;
    si.inferred_mod_name = m.inferred_mod_name;
    si.score = row.score;
    si.reason = L"스택 스캔에서 " + std::to_wstring(row.score) + L"회 관측";
    out.push_back(std::move(si));
  }

  return out;
}

struct MinidumpMemoryRange
{
  std::uint64_t start = 0;
  std::uint64_t end = 0;
  const std::uint8_t* bytes = nullptr;  // points into mapped dump file
};

struct MinidumpMemoryView
{
  std::vector<MinidumpMemoryRange> ranges;

  bool Init(void* dumpBase, std::uint64_t dumpSize, const std::vector<ThreadRecord>* threads)
  {
    ranges.clear();
    if (!dumpBase || dumpSize < sizeof(MINIDUMP_HEADER)) {
      return false;
    }

    const auto* base = static_cast<const std::uint8_t*>(dumpBase);

    // Prefer Memory64ListStream for FullMemory dumps.
    {
      void* ptr = nullptr;
      ULONG size = 0;
      if (ReadStreamSized(dumpBase, dumpSize, Memory64ListStream, &ptr, &size) && ptr && size >= sizeof(MINIDUMP_MEMORY64_LIST)) {
        const auto* list = static_cast<const MINIDUMP_MEMORY64_LIST*>(ptr);
        const std::uint64_t need =
          static_cast<std::uint64_t>(offsetof(MINIDUMP_MEMORY64_LIST, MemoryRanges)) +
          static_cast<std::uint64_t>(list->NumberOfMemoryRanges) * static_cast<std::uint64_t>(sizeof(MINIDUMP_MEMORY_DESCRIPTOR64));
        if (need <= static_cast<std::uint64_t>(size)) {
          std::uint64_t cursor = list->BaseRva;
          ranges.reserve(static_cast<std::size_t>(list->NumberOfMemoryRanges));
          for (ULONG i = 0; i < list->NumberOfMemoryRanges; i++) {
            const auto& mr = list->MemoryRanges[i];
            if (mr.DataSize == 0) {
              continue;
            }
            if (cursor > dumpSize || mr.DataSize > (dumpSize - cursor)) {
              ranges.clear();
              break;
            }
            MinidumpMemoryRange r{};
            r.start = mr.StartOfMemoryRange;
            r.end = mr.StartOfMemoryRange + mr.DataSize;
            r.bytes = base + cursor;
            ranges.push_back(r);
            cursor += mr.DataSize;
          }
          if (!ranges.empty()) {
            std::sort(ranges.begin(), ranges.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
            return true;
          }
        }
      }
    }

    // Fallback: MemoryListStream (typically contains thread stacks).
    {
      void* ptr = nullptr;
      ULONG size = 0;
      if (!ReadStreamSized(dumpBase, dumpSize, MemoryListStream, &ptr, &size) || !ptr || size < sizeof(MINIDUMP_MEMORY_LIST)) {
        // Some dumps omit MemoryListStream but still include per-thread stack memory in ThreadListStream.
        if (!threads || threads->empty()) {
          return false;
        }
        ranges.reserve(threads->size());
        for (const auto& tr : *threads) {
          const std::uint64_t rva = static_cast<std::uint64_t>(tr.stack.Memory.Rva);
          const std::uint64_t sz = static_cast<std::uint64_t>(tr.stack.Memory.DataSize);
          if (rva == 0 || sz == 0) {
            continue;
          }
          if (rva > dumpSize || sz > (dumpSize - rva)) {
            continue;
          }
          MinidumpMemoryRange r{};
          r.start = tr.stack.StartOfMemoryRange;
          r.end = tr.stack.StartOfMemoryRange + sz;
          r.bytes = base + rva;
          ranges.push_back(r);
        }
        if (!ranges.empty()) {
          std::sort(ranges.begin(), ranges.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
        }
        return !ranges.empty();
      }

      const auto* list = static_cast<const MINIDUMP_MEMORY_LIST*>(ptr);
      const std::uint64_t need =
        static_cast<std::uint64_t>(offsetof(MINIDUMP_MEMORY_LIST, MemoryRanges)) +
        static_cast<std::uint64_t>(list->NumberOfMemoryRanges) * static_cast<std::uint64_t>(sizeof(MINIDUMP_MEMORY_DESCRIPTOR));
      if (need > static_cast<std::uint64_t>(size)) {
        return false;
      }

      ranges.reserve(static_cast<std::size_t>(list->NumberOfMemoryRanges));
      for (ULONG i = 0; i < list->NumberOfMemoryRanges; i++) {
        const auto& mr = list->MemoryRanges[i];
        const std::uint64_t rva = static_cast<std::uint64_t>(mr.Memory.Rva);
        const std::uint64_t sz = static_cast<std::uint64_t>(mr.Memory.DataSize);
        if (rva == 0 || sz == 0) {
          continue;
        }
        if (rva > dumpSize || sz > (dumpSize - rva)) {
          continue;
        }
        MinidumpMemoryRange r{};
        r.start = mr.StartOfMemoryRange;
        r.end = mr.StartOfMemoryRange + sz;
        r.bytes = base + rva;
        ranges.push_back(r);
      }

      if (!ranges.empty()) {
        std::sort(ranges.begin(), ranges.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
      }
      return !ranges.empty();
    }
  }

  bool Read(std::uint64_t addr, void* dst, std::size_t n, std::size_t& outRead) const
  {
    outRead = 0;
    if (!dst || n == 0 || ranges.empty()) {
      return false;
    }

    const auto it = std::upper_bound(ranges.begin(), ranges.end(), addr, [](std::uint64_t value, const MinidumpMemoryRange& r) {
      return value < r.start;
    });
    if (it == ranges.begin()) {
      return false;
    }
    const auto& r = *(it - 1);
    if (addr < r.start || addr >= r.end || !r.bytes) {
      return false;
    }
    const std::uint64_t avail = r.end - addr;
    const std::size_t copyN = static_cast<std::size_t>(std::min<std::uint64_t>(avail, static_cast<std::uint64_t>(n)));
    std::memcpy(dst, r.bytes + static_cast<std::size_t>(addr - r.start), copyN);
    outRead = copyN;
    return copyN > 0;
  }
};

static thread_local const MinidumpMemoryView* g_stackwalkMemView = nullptr;

BOOL CALLBACK ReadProcessMemoryFromMinidump64(HANDLE, DWORD64 baseAddr, PVOID buffer, DWORD size, LPDWORD bytesRead)
{
  if (bytesRead) {
    *bytesRead = 0;
  }
  if (!g_stackwalkMemView || !buffer || size == 0) {
    return FALSE;
  }
  std::size_t got = 0;
  const bool ok = g_stackwalkMemView->Read(static_cast<std::uint64_t>(baseAddr), buffer, static_cast<std::size_t>(size), got);
  if (bytesRead) {
    *bytesRead = static_cast<DWORD>(got);
  }
  return ok ? TRUE : FALSE;
}

struct SymSession
{
  HANDLE process = nullptr;
  bool ok = false;

  explicit SymSession(const std::vector<ModuleInfo>& modules)
  {
    process = GetCurrentProcess();

    DWORD opts = SymGetOptions();
    opts |= SYMOPT_UNDNAME;
    opts |= SYMOPT_DEFERRED_LOADS;
    opts |= SYMOPT_FAIL_CRITICAL_ERRORS;
    opts |= SYMOPT_NO_PROMPTS;
    SymSetOptions(opts);

    ok = SymInitializeW(process, nullptr, FALSE) ? true : false;
    if (!ok) {
      return;
    }

    for (const auto& m : modules) {
      if (m.path.empty() || m.base == 0 || m.end <= m.base) {
        continue;
      }
      const DWORD64 base = static_cast<DWORD64>(m.base);
      const DWORD size = static_cast<DWORD>(std::min<std::uint64_t>(m.end - m.base, 0xFFFFFFFFull));
      SymLoadModuleExW(process, nullptr, m.path.c_str(), m.filename.empty() ? nullptr : m.filename.c_str(), base, size, nullptr, 0);
    }
  }

  ~SymSession()
  {
    if (ok && process) {
      SymCleanup(process);
    }
  }
};

std::wstring FormatModulePlusOffset(const std::vector<ModuleInfo>& modules, std::uint64_t addr)
{
  if (auto idx = FindModuleIndexForAddress(modules, addr)) {
    const auto& m = modules[*idx];
    const std::uint64_t off = addr - m.base;
    wchar_t buf[1024]{};
    swprintf_s(buf, L"%s+0x%llx", m.filename.c_str(), static_cast<unsigned long long>(off));
    return buf;
  }
  wchar_t buf[64]{};
  swprintf_s(buf, L"0x%llx", static_cast<unsigned long long>(addr));
  return buf;
}

std::vector<std::uint64_t> StackWalkAddrsForContext(
  HANDLE process,
  const MinidumpMemoryView& mem,
  const CONTEXT& inCtx,
  std::size_t maxFrames)
{
  std::vector<std::uint64_t> pcs;
  if (!process || maxFrames == 0) {
    return pcs;
  }

  CONTEXT ctx = inCtx;
  STACKFRAME64 frame{};
  frame.AddrPC.Offset = ctx.Rip;
  frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrFrame.Offset = ctx.Rbp;
  frame.AddrFrame.Mode = AddrModeFlat;
  frame.AddrStack.Offset = ctx.Rsp;
  frame.AddrStack.Mode = AddrModeFlat;

  g_stackwalkMemView = &mem;
  const HANDLE thread = GetCurrentThread();

  constexpr std::size_t kMaxHard = 128;
  const std::size_t limit = std::min(maxFrames, kMaxHard);
  for (std::size_t i = 0; i < limit; i++) {
    const DWORD64 pc = frame.AddrPC.Offset;
    if (pc == 0) {
      break;
    }
    pcs.push_back(static_cast<std::uint64_t>(pc));

    const BOOL ok = StackWalk64(
      IMAGE_FILE_MACHINE_AMD64,
      process,
      thread,
      &frame,
      &ctx,
      ReadProcessMemoryFromMinidump64,
      SymFunctionTableAccess64,
      SymGetModuleBase64,
      nullptr);
    if (!ok) {
      break;
    }
    if (frame.AddrPC.Offset == pc) {
      break;
    }
  }

  g_stackwalkMemView = nullptr;
  return pcs;
}

std::uint32_t CallstackFrameWeight(std::size_t depth)
{
  if (depth == 0) return 16;
  if (depth == 1) return 12;
  if (depth == 2) return 8;
  if (depth <= 5) return 4;
  if (depth <= 10) return 2;
  return 1;
}

std::wstring ConfidenceForTopSuspectCallstack(std::uint32_t topScore, std::uint32_t secondScore, std::size_t firstDepth)
{
  if (firstDepth <= 2 && (topScore >= 24u || topScore >= (secondScore + 12u))) {
    return ConfidenceHigh();
  }
  if (firstDepth <= 6 && (topScore >= 12u || topScore >= (secondScore + 6u))) {
    return ConfidenceMid();
  }
  return ConfidenceLow();
}

std::vector<SuspectItem> ComputeCallstackSuspectsFromAddrs(const std::vector<ModuleInfo>& modules, const std::vector<std::uint64_t>& pcs)
{
  std::vector<SuspectItem> out;
  if (modules.empty() || pcs.empty()) {
    return out;
  }

  struct Row
  {
    std::size_t modIndex = 0;
    std::uint32_t score = 0;
    std::size_t firstDepth = 0;
  };
  std::unordered_map<std::size_t, Row> byModule;

  for (std::size_t i = 0; i < pcs.size(); i++) {
    auto mi = FindModuleIndexForAddress(modules, pcs[i]);
    if (!mi) {
      continue;
    }
    const auto& m = modules[*mi];
    if (m.is_systemish || m.is_game_exe) {
      continue;
    }
    const std::uint32_t w = CallstackFrameWeight(i);
    auto it = byModule.find(*mi);
    if (it == byModule.end()) {
      Row r{};
      r.modIndex = *mi;
      r.score = w;
      r.firstDepth = i;
      byModule.emplace(*mi, r);
    } else {
      it->second.score += w;
      it->second.firstDepth = std::min<std::size_t>(it->second.firstDepth, i);
    }
  }

  if (byModule.empty()) {
    return out;
  }

  std::vector<Row> rows;
  rows.reserve(byModule.size());
  for (const auto& [_, row] : byModule) {
    rows.push_back(row);
  }

  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.score > b.score; });

  const std::uint32_t topScore = rows[0].score;
  const std::uint32_t secondScore = (rows.size() > 1) ? rows[1].score : 0;
  const std::wstring conf = ConfidenceForTopSuspectCallstack(topScore, secondScore, rows[0].firstDepth);

  const std::size_t n = std::min<std::size_t>(rows.size(), 5);
  out.reserve(n);
  for (std::size_t i = 0; i < n; i++) {
    const auto& row = rows[i];
    const auto& m = modules[row.modIndex];

    SuspectItem si{};
    si.confidence = (i == 0) ? conf : ConfidenceMid();
    si.module_filename = m.filename;
    si.module_path = m.path;
    si.inferred_mod_name = m.inferred_mod_name;
    si.score = row.score;
    si.reason =
      L"콜스택 상위 프레임에서 가중치=" + std::to_wstring(row.score) +
      L", 최초 깊이=" + std::to_wstring(row.firstDepth);
    out.push_back(std::move(si));
  }

  return out;
}

bool TryReadContextFromLocation(void* dumpBase, std::uint64_t dumpSize, const MINIDUMP_LOCATION_DESCRIPTOR& loc, CONTEXT& out)
{
  if (!dumpBase) {
    return false;
  }
  const std::uint64_t rva = static_cast<std::uint64_t>(loc.Rva);
  const std::uint64_t sz = static_cast<std::uint64_t>(loc.DataSize);
  if (rva == 0 || sz == 0 || rva > dumpSize || sz > (dumpSize - rva)) {
    return false;
  }

  const auto* base = static_cast<const std::uint8_t*>(dumpBase);
  const std::size_t copyN = static_cast<std::size_t>(std::min<std::uint64_t>(sz, sizeof(CONTEXT)));
  std::memset(&out, 0, sizeof(out));
  std::memcpy(&out, base + rva, copyN);
  return true;
}

std::vector<std::wstring> FormatCallstackForDisplay(const std::vector<ModuleInfo>& modules, const std::vector<std::uint64_t>& pcs, std::size_t maxFrames)
{
  std::vector<std::wstring> out;
  if (pcs.empty() || maxFrames == 0) {
    return out;
  }

  std::size_t firstNonSystem = pcs.size();
  for (std::size_t i = 0; i < pcs.size(); i++) {
    auto mi = FindModuleIndexForAddress(modules, pcs[i]);
    if (!mi) {
      continue;
    }
    const auto& m = modules[*mi];
    if (!m.is_systemish && !m.is_game_exe) {
      firstNonSystem = i;
      break;
    }
  }

  const std::size_t start = (firstNonSystem != pcs.size() && firstNonSystem > 2) ? (firstNonSystem - 2) : 0;
  const std::size_t end = std::min<std::size_t>(pcs.size(), start + maxFrames);
  out.reserve(end - start);
  for (std::size_t i = start; i < end; i++) {
    out.push_back(FormatModulePlusOffset(modules, pcs[i]));
  }
  return out;
}

bool TryComputeStackwalkSuspects(
  void* dumpBase,
  std::uint64_t dumpSize,
  const std::vector<ModuleInfo>& modules,
  const std::vector<std::uint32_t>& targetTids,
  std::uint32_t excTid,
  const std::optional<CONTEXT>& excCtx,
  const std::vector<ThreadRecord>& threads,
  AnalysisResult& out)
{
  if (!dumpBase || modules.empty() || targetTids.empty() || threads.empty()) {
    return false;
  }

  MinidumpMemoryView mem;
  if (!mem.Init(dumpBase, dumpSize, &threads)) {
    return false;
  }

  SymSession sym(modules);
  if (!sym.ok) {
    return false;
  }

  struct Candidate
  {
    std::uint32_t tid = 0;
    std::vector<std::uint64_t> pcs;
    std::vector<SuspectItem> suspects;
    std::uint32_t topScore = 0;
  };

  Candidate best{};
  Candidate bestAny{};
  for (const auto tid : targetTids) {
    CONTEXT ctx{};
    bool haveCtx = false;
    if (tid != 0 && tid == excTid && excCtx) {
      ctx = *excCtx;
      haveCtx = true;
    } else {
      const auto it = std::find_if(threads.begin(), threads.end(), [&](const ThreadRecord& tr) { return tr.tid == tid; });
      if (it != threads.end() && ReadThreadContextWin64(dumpBase, dumpSize, *it, ctx)) {
        haveCtx = true;
      }
    }
    if (!haveCtx) {
      continue;
    }
    if (ctx.Rip == 0 || ctx.Rsp == 0) {
      continue;
    }

    auto pcs = StackWalkAddrsForContext(sym.process, mem, ctx, /*maxFrames=*/64);
    if (pcs.empty()) {
      continue;
    }

    if (bestAny.pcs.size() < pcs.size()) {
      bestAny.tid = tid;
      bestAny.pcs = pcs;
    }

    auto suspects = ComputeCallstackSuspectsFromAddrs(modules, pcs);
    if (suspects.empty()) {
      continue;
    }

    const std::uint32_t topScore = suspects[0].score;
    const bool prefer = (best.tid != excTid && tid == excTid);
    if (best.suspects.empty() || prefer || topScore > best.topScore) {
      best.tid = tid;
      best.pcs = std::move(pcs);
      best.suspects = std::move(suspects);
      best.topScore = topScore;
    }
  }

  if (best.suspects.empty()) {
    if (!bestAny.pcs.empty()) {
      out.stackwalk_primary_tid = bestAny.tid;
      out.stackwalk_primary_frames = FormatCallstackForDisplay(modules, bestAny.pcs, /*maxFrames=*/12);
    }
    return false;
  }

  // Boost confidence when Crash Logger agrees with our top module (best-effort).
  if (!out.crash_logger_top_modules.empty() && !best.suspects.empty()) {
    const auto topLower = WideLower(best.suspects[0].module_filename);
    for (const auto& m : out.crash_logger_top_modules) {
      if (WideLower(m) == topLower) {
        best.suspects[0].confidence = ConfidenceHigh();
        best.suspects[0].reason += L" (Crash Logger 콜스택에도 등장)";
        break;
      }
    }
  }

  out.suspects_from_stackwalk = true;
  out.suspects = std::move(best.suspects);
  out.stackwalk_primary_tid = best.tid;
  out.stackwalk_primary_frames = FormatCallstackForDisplay(modules, best.pcs, /*maxFrames=*/12);
  return true;
}

#if 0  // moved to EvidenceBuilder.cpp
// (legacy copy removed; see EvidenceBuilder.cpp)
#endif  // moved to EvidenceBuilder.cpp

}  // namespace

bool AnalyzeDump(const std::wstring& dumpPath, const std::wstring& outDir, const AnalyzeOptions& opt, AnalysisResult& out, std::wstring* err)
{
  (void)opt;
  out = AnalysisResult{};
  out.dump_path = dumpPath;
  out.out_dir = outDir;

  const std::wstring dumpNameLower = WideLower(std::filesystem::path(dumpPath).filename().wstring());
  const bool nameCrash = (dumpNameLower.find(L"_crash_") != std::wstring::npos);
  const bool nameHang = (dumpNameLower.find(L"_hang_") != std::wstring::npos);

  // Memory-map the dump to avoid loading large FullMemory dumps into RAM.
  MappedFile mf{};
  if (!mf.Open(dumpPath, err)) {
    return false;
  }
  const std::uint64_t dumpSize = mf.size;
  void* dumpBase = mf.view;

  const auto allModules = LoadAllModules(dumpBase, dumpSize);
  std::vector<std::wstring> modulePaths;
  modulePaths.reserve(allModules.size());
  for (const auto& m : allModules) {
    if (!m.path.empty()) {
      modulePaths.push_back(m.path);
    }
  }
  const auto mo2Index = TryBuildMo2IndexFromModulePaths(modulePaths);

  // Exception info
  std::optional<CONTEXT> excCtx;
  void* excPtr = nullptr;
  ULONG excSize = 0;
  if (ReadStreamSized(dumpBase, dumpSize, ExceptionStream, &excPtr, &excSize) && excPtr && excSize >= sizeof(MINIDUMP_EXCEPTION_STREAM)) {
    const auto* es = static_cast<const MINIDUMP_EXCEPTION_STREAM*>(excPtr);
    out.exc_code = es->ExceptionRecord.ExceptionCode;
    out.exc_tid = es->ThreadId;
    out.exc_addr = es->ExceptionRecord.ExceptionAddress;
    CONTEXT ctx{};
    if (TryReadContextFromLocation(dumpBase, dumpSize, es->ThreadContext, ctx)) {
      excCtx = ctx;
    }
  }

  // Fault module
  if (out.exc_addr != 0) {
    if (auto idx = FindModuleIndexForAddress(allModules, out.exc_addr)) {
      const auto& m = allModules[*idx];
      out.fault_module_path = m.path;
      out.fault_module_filename = m.filename;
      const std::uint64_t off = out.exc_addr - m.base;
      wchar_t buf[1024]{};
      swprintf_s(buf, L"%s+0x%llx", m.filename.c_str(), static_cast<unsigned long long>(off));
      out.fault_module_plus_offset = buf;
      out.inferred_mod_name = m.inferred_mod_name;
    } else if (auto m = ModuleForAddress(dumpBase, dumpSize, out.exc_addr)) {  // fallback
      out.fault_module_path = m->path;
      out.fault_module_filename = m->filename;
      out.fault_module_plus_offset = m->plusOffset;
      out.inferred_mod_name = InferMo2ModNameFromPath(out.fault_module_path);
    }
  }

  // SkyrimDiag blackbox (optional)
  void* bbPtr = nullptr;
  ULONG bbSize = 0;
  if (ReadStreamSized(dumpBase, dumpSize, skydiag::protocol::kMinidumpUserStream_Blackbox, &bbPtr, &bbSize) && bbPtr &&
      bbSize >= offsetof(skydiag::SharedLayout, resources)) {
    const auto* snap = static_cast<const skydiag::SharedLayout*>(bbPtr);
    const auto ver = snap->header.version;
    if (snap->header.magic == skydiag::kMagic && (ver == 1u || ver == skydiag::kVersion)) {
      out.has_blackbox = true;
      out.pid = snap->header.pid;
      out.state_flags = snap->header.state_flags;

      std::uint32_t cap = snap->header.capacity;
      if (cap == 0 || cap > skydiag::kEventCapacity) {
        cap = skydiag::kEventCapacity;
      }
      const std::uint64_t freq = snap->header.qpc_freq ? snap->header.qpc_freq : 1;
      const std::uint64_t start = snap->header.start_qpc;
      const std::uint32_t writeIndex = snap->header.write_index;
      const std::uint32_t begin = (writeIndex > cap) ? (writeIndex - cap) : 0;

      out.events.clear();
      out.events.reserve(static_cast<std::size_t>(std::min<std::uint32_t>(writeIndex, cap)));
      for (std::uint32_t i = begin; i < writeIndex; i++) {
        const auto& ev = snap->events[i % cap];
        const std::uint32_t seq1 = ev.seq;
        if ((seq1 & 1u) != 0u) {
          continue;
        }

        skydiag::BlackboxEvent tmp{};
        std::memcpy(&tmp, &ev, sizeof(tmp));
        const std::uint32_t seq2 = ev.seq;
        if (seq1 != seq2 || (seq2 & 1u) != 0u) {
          continue;
        }

        if (tmp.type == static_cast<std::uint16_t>(skydiag::EventType::kInvalid)) {
          continue;
        }

        EventRow row{};
        row.i = i;
        row.tid = tmp.tid;
        row.type = tmp.type;
        row.type_name = EventTypeName(tmp.type);
        row.a = tmp.payload.a;
        row.b = tmp.payload.b;
        row.c = tmp.payload.c;
        row.d = tmp.payload.d;
        row.t_ms = (tmp.qpc >= start)
          ? (1000.0 * (static_cast<double>(tmp.qpc - start) / static_cast<double>(freq)))
          : 0.0;
        out.events.push_back(std::move(row));
      }

      // Resource log (optional; v2+ includes it after the blackbox events).
      out.resources.clear();
      if (bbSize >= sizeof(skydiag::SharedLayout)) {
        const auto& rl = snap->resources;
        const std::uint32_t rCap = skydiag::kResourceCapacity;
        const std::uint32_t rWrite = rl.write_index;
        const std::uint32_t rBegin = (rWrite > rCap) ? (rWrite - rCap) : 0;

        out.resources.reserve(static_cast<std::size_t>(std::min<std::uint32_t>(rWrite, rCap)));

        for (std::uint32_t i = rBegin; i < rWrite; i++) {
          const auto& ent = rl.entries[i % rCap];
          const std::uint32_t seq1 = ent.seq;
          if ((seq1 & 1u) != 0u) {
            continue;
          }

          skydiag::ResourceEntry tmp{};
          std::memcpy(&tmp, &ent, sizeof(tmp));
          const std::uint32_t seq2 = ent.seq;
          if (seq1 != seq2 || (seq2 & 1u) != 0u) {
            continue;
          }

          const std::size_t maxN = sizeof(tmp.path_utf8);
          std::size_t len = 0;
          while (len < maxN && tmp.path_utf8[len] != '\0') {
            len++;
          }
          if (len == 0) {
            continue;
          }

          ResourceRow rr{};
          rr.tid = tmp.tid;
          rr.t_ms = (tmp.qpc >= start)
            ? (1000.0 * (static_cast<double>(tmp.qpc - start) / static_cast<double>(freq)))
            : 0.0;
          rr.path = ToWide(std::string_view(tmp.path_utf8, len));
          rr.kind = ResourceKindFromPath(rr.path);
          if (mo2Index) {
            rr.providers = FindMo2ProvidersForDataPath(*mo2Index, rr.path, /*maxProviders=*/8);
            rr.is_conflict = rr.providers.size() >= 2;
          }
          out.resources.push_back(std::move(rr));
        }

        // Cap display size (best-effort) to keep UI/report manageable.
        constexpr std::size_t kMaxKeep = 80;
        if (out.resources.size() > kMaxKeep) {
          out.resources.erase(out.resources.begin(), out.resources.end() - kMaxKeep);
        }
      }
    }
  }

  // WCT stream (optional)
  void* wctPtr = nullptr;
  ULONG wctSize = 0;
  if (ReadStreamSized(dumpBase, dumpSize, skydiag::protocol::kMinidumpUserStream_WctJson, &wctPtr, &wctSize) && wctPtr && wctSize > 0) {
    out.has_wct = true;
    out.wct_json_utf8.assign(static_cast<const char*>(wctPtr), static_cast<std::size_t>(wctSize));
  }

  bool hasHangEvent = false;
  for (const auto& ev : out.events) {
    if (ev.type == static_cast<std::uint16_t>(skydiag::EventType::kHangMark)) {
      hasHangEvent = true;
      break;
    }
  }

  // Determine whether this looks like a hang/freeze capture.
  bool hangLike = false;
  bool capSaysHang = false;
  if (out.has_wct) {
    if (auto cap = TryParseWctCaptureDecision(out.wct_json_utf8)) {
      capSaysHang =
        (cap->kind == "hang") ||
        (cap->thresholdSec > 0u && cap->secondsSinceHeartbeat >= static_cast<double>(cap->thresholdSec));
      hangLike = capSaysHang;
    } else {
      hangLike = nameHang;
    }

    if (hasHangEvent) {
      hangLike = true;
    }

    // Best-effort override: some manual hotkey dumps are named "_Hang_" even when the game is fine.
    // If the last heartbeat is very recent, treat it as a snapshot (not hang-like).
    if (!capSaysHang && !hasHangEvent) {
      constexpr double kNotHangHeartbeatAgeSec = 5.0;
      if (out.has_blackbox) {
        if (auto hbAge = InferHeartbeatAgeFromEventsSec(out.events)) {
          if (*hbAge < kNotHangHeartbeatAgeSec) {
            hangLike = false;
          }
        }
      }
    }
  } else {
    hangLike = nameHang || hasHangEvent;
  }

  // Optional: Crash Logger SSE/AE log integration (best-effort)
  {
    // Manual snapshot dumps often include WCT even when the game is fine, so only search Crash Logger logs when the
    // capture looks crash/hang-like.
    const bool shouldSearchCrashLogger = (out.exc_code != 0) || nameCrash || hangLike;
    if (shouldSearchCrashLogger) {
      std::wstring clErr;
      const auto dumpFs = std::filesystem::path(dumpPath);
      const auto mo2Base = TryInferMo2BaseDirFromModulePaths(modulePaths);
      if (auto logPath = TryFindCrashLoggerLogForDump(dumpFs, mo2Base, &clErr)) {
        out.crash_logger_log_path = logPath->wstring();

        std::wstring readErr;
        auto logUtf8 = ReadWholeFileUtf8(*logPath, &readErr);
        if (logUtf8) {
          std::unordered_map<std::wstring, std::wstring> canonicalByLower;
          canonicalByLower.reserve(allModules.size());
          for (const auto& m : allModules) {
            if (!m.filename.empty()) {
              canonicalByLower.emplace(WideLower(m.filename), m.filename);
            }
          }
          out.crash_logger_top_modules = ParseCrashLoggerTopModules(*logUtf8, canonicalByLower);
        }
      }
    }
  }

  // Suspects (prefer callstack/stackwalk; fallback to stack scan)
  {
    const bool shouldAnalyzeStacks = (out.exc_tid != 0) || hangLike;
    if (shouldAnalyzeStacks) {
      std::vector<std::uint32_t> tids;
      if (out.exc_tid != 0) {
        tids.push_back(out.exc_tid);
      } else if (out.has_wct) {
        tids = ExtractWctCandidateThreadIds(out.wct_json_utf8, /*maxN=*/8);
      }
      if (out.has_blackbox) {
        if (auto mainTid = InferMainThreadIdFromEvents(out.events)) {
          tids.push_back(*mainTid);
        }
      }
      if (!tids.empty()) {
        // Dedup
        std::sort(tids.begin(), tids.end());
        tids.erase(std::unique(tids.begin(), tids.end()), tids.end());
        const auto threads = LoadThreads(dumpBase, dumpSize);
        if (!TryComputeStackwalkSuspects(dumpBase, dumpSize, allModules, tids, out.exc_tid, excCtx, threads, out)) {
          out.suspects_from_stackwalk = false;
          out.suspects = ComputeStackScanSuspects(dumpBase, dumpSize, allModules, tids);
        }
      }
    }
  }

  BuildEvidenceAndSummary(out);
  if (err) err->clear();
  return true;
}

}  // namespace skydiag::dump_tool

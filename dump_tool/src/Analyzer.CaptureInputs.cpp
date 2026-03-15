#include "AnalyzerPipeline.h"

#include "AnalyzerInternals.h"
#include "CrashLogger.h"
#include "CrashLoggerParseCore.h"
#include "Mo2Index.h"
#include "OutputWriterInternals.h"
#include "PluginRules.h"
#include "Utf.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string_view>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "SkyrimDiagProtocol.h"
#include "SkyrimDiagShared.h"

namespace skydiag::dump_tool {

using skydiag::dump_tool::internal::output_writer::ReadTextFileUtf8;
using skydiag::dump_tool::minidump::FindModuleIndexForAddress;
using skydiag::dump_tool::minidump::GetThreadStackBytes;
using skydiag::dump_tool::minidump::IsGameExeModule;
using skydiag::dump_tool::minidump::IsLikelyWindowsSystemModulePath;
using skydiag::dump_tool::minidump::IsSystemishModule;
using skydiag::dump_tool::minidump::LoadThreads;
using skydiag::dump_tool::minidump::ModuleForAddress;
using skydiag::dump_tool::minidump::ModuleInfo;
using skydiag::dump_tool::minidump::ReadStreamSized;
using skydiag::dump_tool::minidump::WideLower;

std::optional<CONTEXT> ParseExceptionInfo(
  void* dumpBase,
  std::uint64_t dumpSize,
  AnalysisResult& out)
{
  void* excPtr = nullptr;
  ULONG excSize = 0;
  if (!ReadStreamSized(dumpBase, dumpSize, ExceptionStream, &excPtr, &excSize) || !excPtr || excSize < sizeof(MINIDUMP_EXCEPTION_STREAM)) {
    return std::nullopt;
  }
  const auto* es = static_cast<const MINIDUMP_EXCEPTION_STREAM*>(excPtr);
  out.exc_code = es->ExceptionRecord.ExceptionCode;
  out.exc_tid = es->ThreadId;
  out.exc_addr = es->ExceptionRecord.ExceptionAddress;
  out.exc_info.clear();
  {
    const ULONG n = es->ExceptionRecord.NumberParameters;
    constexpr ULONG kMaxN =
      static_cast<ULONG>(sizeof(es->ExceptionRecord.ExceptionInformation) / sizeof(es->ExceptionRecord.ExceptionInformation[0]));
    const ULONG take = (n < kMaxN) ? n : kMaxN;
    out.exc_info.reserve(take);
    for (ULONG i = 0; i < take; i++) {
      out.exc_info.push_back(static_cast<std::uint64_t>(es->ExceptionRecord.ExceptionInformation[i]));
    }
  }
  CONTEXT ctx{};
  if (internal::TryReadContextFromLocation(dumpBase, dumpSize, es->ThreadContext, ctx)) {
    return ctx;
  }
  return std::nullopt;
}

void ResolveFaultModule(
  void* dumpBase,
  std::uint64_t dumpSize,
  const std::vector<ModuleInfo>& allModules,
  AnalysisResult& out)
{
  if (out.exc_addr == 0) {
    return;
  }
  if (auto idx = FindModuleIndexForAddress(allModules, out.exc_addr)) {
    const auto& m = allModules[*idx];
    out.fault_module_path = m.path;
    out.fault_module_filename = m.filename;
    const std::uint64_t off = out.exc_addr - m.base;
    out.fault_module_offset = off;
    wchar_t buf[1024]{};
    swprintf_s(buf, L"%s+0x%llx", m.filename.c_str(), static_cast<unsigned long long>(off));
    out.fault_module_plus_offset = buf;
    out.inferred_mod_name = m.inferred_mod_name;
  } else if (auto m = ModuleForAddress(dumpBase, dumpSize, out.exc_addr)) {
    out.fault_module_path = m->path;
    out.fault_module_filename = m->filename;
    out.fault_module_plus_offset = m->plusOffset;
    if (out.exc_addr >= m->base) {
      out.fault_module_offset = (out.exc_addr - m->base);
    }
    out.inferred_mod_name = InferMo2ModNameFromPath(out.fault_module_path);
  }

  if (!out.inferred_mod_name.empty()) {
    const std::wstring inferredLower = WideLower(out.inferred_mod_name);
    const std::wstring faultLower = WideLower(out.fault_module_filename);
    const bool inferredLooksBinaryName =
      (inferredLower.size() >= 4 && inferredLower.substr(inferredLower.size() - 4) == L".dll") ||
      (inferredLower.size() >= 4 && inferredLower.substr(inferredLower.size() - 4) == L".exe");
    const bool faultIsSystem =
      IsSystemishModule(out.fault_module_filename) || IsLikelyWindowsSystemModulePath(out.fault_module_path);
    if (faultIsSystem || IsGameExeModule(out.fault_module_filename) || inferredLooksBinaryName || inferredLower == faultLower) {
      out.inferred_mod_name.clear();
    }
  }
}

void ParseBlackboxStream(
  void* dumpBase,
  std::uint64_t dumpSize,
  const std::optional<Mo2Index>& mo2Index,
  const std::vector<std::wstring>& modulePaths,
  AnalysisResult& out)
{
  void* bbPtr = nullptr;
  ULONG bbSize = 0;
  if (!ReadStreamSized(dumpBase, dumpSize, skydiag::protocol::kMinidumpUserStream_Blackbox, &bbPtr, &bbSize) || !bbPtr ||
      bbSize < offsetof(skydiag::SharedLayout, resources)) {
    return;
  }
  const auto* snap = static_cast<const skydiag::SharedLayout*>(bbPtr);
  const auto ver = snap->header.version;
  if (snap->header.magic != skydiag::kMagic || (ver != 1u && ver != skydiag::kVersion)) {
    return;
  }

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
    row.type_name = internal::EventTypeName(tmp.type);
    row.a = tmp.payload.a;
    row.b = tmp.payload.b;
    row.c = tmp.payload.c;
    row.d = tmp.payload.d;
    row.detail = internal::FormatEventDetail(row.type, row.a, row.b, row.c, row.d);
    row.t_ms = (tmp.qpc >= start)
      ? (1000.0 * (static_cast<double>(tmp.qpc - start) / static_cast<double>(freq)))
      : 0.0;
    out.events.push_back(std::move(row));
  }

  out.resources.clear();
  if (bbSize < sizeof(skydiag::SharedLayout)) {
    return;
  }

  const auto& rl = snap->resources;
  const std::uint32_t rCap = skydiag::kResourceCapacity;
  const std::uint32_t rWrite = rl.write_index;
  const std::uint32_t rBegin = (rWrite > rCap) ? (rWrite - rCap) : 0;

  out.resources.reserve(static_cast<std::size_t>(std::min<std::uint32_t>(rWrite, rCap)));

  std::unordered_map<std::wstring, std::vector<std::wstring>> mo2ProvidersCache;

  const auto normMo2Key = [](std::wstring_view relPath) {
    std::wstring key(relPath);
    while (!key.empty() && (key.front() == L'\\' || key.front() == L'/')) {
      key.erase(key.begin());
    }
    for (auto& ch : key) {
      if (ch == L'/') {
        ch = L'\\';
      }
      ch = static_cast<wchar_t>(towlower(ch));
    }
    return key;
  };

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
    rr.path = Utf8ToWide(std::string_view(tmp.path_utf8, len));
    rr.kind = internal::ResourceKindFromPath(rr.path);
    if (mo2Index) {
      const std::wstring key = normMo2Key(rr.path);
      auto it = mo2ProvidersCache.find(key);
      if (it == mo2ProvidersCache.end()) {
        rr.providers = FindMo2ProvidersForDataPath(*mo2Index, rr.path, /*maxProviders=*/8);
        it = mo2ProvidersCache.emplace(key, rr.providers).first;
      } else {
        rr.providers = it->second;
      }
      rr.is_conflict = rr.providers.size() >= 2;
    }
    out.resources.push_back(std::move(rr));
  }

  constexpr std::size_t kMaxKeep = 120;
  if (out.resources.size() > kMaxKeep) {
    out.resources.erase(out.resources.begin(), out.resources.end() - kMaxKeep);
  }
}

void IntegratePluginScan(
  const std::wstring& dumpPath,
  const std::vector<ModuleInfo>& allModules,
  void* dumpBase,
  std::uint64_t dumpSize,
  const AnalyzeOptions& opt,
  AnalysisResult& out)
{
  void* pluginPtr = nullptr;
  ULONG pluginSize = 0;
  if (ReadStreamSized(dumpBase, dumpSize, skydiag::protocol::kMinidumpUserStream_PluginInfo, &pluginPtr, &pluginSize) &&
      pluginPtr && pluginSize > 0) {
    out.has_plugin_scan = true;
    out.plugin_scan_json_utf8.assign(static_cast<const char*>(pluginPtr), static_cast<std::size_t>(pluginSize));
  }
  if (!out.has_plugin_scan) {
    const std::filesystem::path dumpFs(dumpPath);
    const auto sidecarPath = dumpFs.parent_path() / (dumpFs.stem().wstring() + L"_PluginScan.json");
    std::string sidecarJson;
    if (ReadTextFileUtf8(sidecarPath, &sidecarJson) && !sidecarJson.empty()) {
      out.has_plugin_scan = true;
      out.plugin_scan_json_utf8 = std::move(sidecarJson);
    }
  }

  ParsedPluginScan parsedPluginScan{};
  bool parsedPluginScanOk = false;
  if (out.has_plugin_scan && !out.plugin_scan_json_utf8.empty()) {
    parsedPluginScanOk = ParsePluginScanJson(out.plugin_scan_json_utf8, &parsedPluginScan);
    if (parsedPluginScanOk) {
      out.missing_masters = ComputeMissingMasters(parsedPluginScan);

      const bool hasHeader171 = AnyPluginHeaderVersionGte(parsedPluginScan, 1.71);
      bool hasBees = false;
      for (const auto& m : allModules) {
        if (WideLower(m.filename) == L"bees.dll") {
          hasBees = true;
          break;
        }
      }
      bool gameVersionLt1130 = false;
      if (!out.game_version.empty()) {
        gameVersionLt1130 = IsGameVersionLessThan(out.game_version, "1.6.1130");
      }
      out.needs_bees = hasHeader171 && gameVersionLt1130 && !hasBees;
    }
  }

  if (parsedPluginScanOk && !opt.data_dir.empty()) {
    PluginRules pluginRules;
    const auto rulesPath = std::filesystem::path(opt.data_dir) / L"plugin_rules.json";
    if (!pluginRules.LoadFromJson(rulesPath)) {
      out.diagnostics.push_back(L"[Data] failed to load plugin_rules.json");
    } else {
      PluginRulesContext ctx{};
      ctx.scan = &parsedPluginScan;
      ctx.game_version = out.game_version;
      ctx.use_korean = (opt.language == i18n::Language::kKorean);
      ctx.missing_masters = out.missing_masters;
      ctx.loaded_module_filenames.reserve(allModules.size());
      for (const auto& m : allModules) {
        if (!m.filename.empty()) {
          ctx.loaded_module_filenames.push_back(m.filename);
        }
      }
      out.plugin_diagnostics = pluginRules.Evaluate(ctx);
    }
  }
}

bool DetermineHangLike(
  bool nameHang,
  const AnalysisResult& out)
{
  bool hasHangEvent = false;
  for (const auto& ev : out.events) {
    if (ev.type == static_cast<std::uint16_t>(skydiag::EventType::kHangMark)) {
      hasHangEvent = true;
      break;
    }
  }

  bool hangLike = false;
  bool capSaysHang = false;
  if (out.has_wct) {
    if (auto cap = internal::TryParseWctCaptureDecision(out.wct_json_utf8)) {
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

    if (!capSaysHang && !hasHangEvent) {
      constexpr double kNotHangHeartbeatAgeSec = 5.0;
      if (out.has_blackbox) {
        if (auto hbAge = internal::InferHeartbeatAgeFromEventsSec(out.events)) {
          if (*hbAge < kNotHangHeartbeatAgeSec) {
            hangLike = false;
          }
        }
      }
    }
  } else {
    hangLike = nameHang || hasHangEvent;
  }

  return hangLike;
}

void IntegrateCrashLoggerLog(
  const std::wstring& dumpPath,
  const std::vector<ModuleInfo>& allModules,
  const std::vector<std::wstring>& modulePaths,
  const std::optional<Mo2Index>& mo2Index,
  AnalysisResult& out)
{
  std::wstring clErr;
  const auto dumpFs = std::filesystem::path(dumpPath);
  std::optional<std::filesystem::path> gameRootDir;
  for (const auto& m : allModules) {
    if (m.path.empty() || m.filename.empty()) {
      continue;
    }
    const std::wstring lower = WideLower(m.filename);
    if (lower == L"skyrimse.exe" || lower == L"skyrimae.exe" || lower == L"skyrimvr.exe" || lower == L"skyrim.exe") {
      gameRootDir = std::filesystem::path(m.path).parent_path();
      break;
    }
  }

  const auto mo2Base = TryInferMo2BaseDirFromModulePaths(modulePaths);
  auto logPath = TryFindCrashLoggerLogForDump(dumpFs, mo2Base, mo2Index ? &*mo2Index : nullptr, gameRootDir, &clErr);
  if (!logPath) {
    if (!clErr.empty()) {
      out.diagnostics.push_back(L"[CrashLogger] log not found: " + clErr);
    }
    return;
  }

  out.crash_logger_log_path = logPath->wstring();

  std::wstring readErr;
  auto logUtf8 = ReadWholeFileUtf8(*logPath, &readErr);
  if (!logUtf8) {
    out.diagnostics.push_back(L"[CrashLogger] failed to read log: " + readErr);
    return;
  }

  if (auto ver = crashlogger_core::ParseCrashLoggerVersionAscii(*logUtf8)) {
    out.crash_logger_version = Utf8ToWide(*ver);
  }

  std::unordered_map<std::wstring, std::wstring> canonicalByLower;
  canonicalByLower.reserve(allModules.size());
  for (const auto& m : allModules) {
    if (!m.filename.empty()) {
      canonicalByLower.emplace(WideLower(m.filename), m.filename);
    }
  }
  out.crash_logger_top_modules = ParseCrashLoggerTopModules(*logUtf8, canonicalByLower);

  if (auto cpp = crashlogger_core::ParseCrashLoggerCppExceptionDetailsAscii(*logUtf8)) {
    if (!cpp->type.empty()) {
      out.crash_logger_cpp_exception_type = Utf8ToWide(cpp->type);
    }
    if (!cpp->info.empty()) {
      out.crash_logger_cpp_exception_info = Utf8ToWide(cpp->info);
    }
    if (!cpp->throw_location.empty()) {
      out.crash_logger_cpp_exception_throw_location = Utf8ToWide(cpp->throw_location);
    }
    if (!cpp->module.empty()) {
      std::wstring mod = Utf8ToWide(cpp->module);
      const auto it = canonicalByLower.find(WideLower(mod));
      out.crash_logger_cpp_exception_module = (it != canonicalByLower.end()) ? it->second : mod;
    }
  }

  auto rawRefs = crashlogger_core::ParseCrashLoggerObjectRefsAscii(*logUtf8);
  auto aggRefs = crashlogger_core::AggregateCrashLoggerObjectRefs(rawRefs);
  std::unordered_map<std::string, std::uint32_t> espCount;
  for (const auto& raw : rawRefs) {
    espCount[crashlogger_core::AsciiLower(raw.esp_name)]++;
  }
  for (const auto& agg : aggRefs) {
    AnalysisResult::CrashLoggerModReference modRef;
    modRef.esp_name = Utf8ToWide(agg.esp_name);
    modRef.best_object_type = Utf8ToWide(agg.object_type);
    modRef.best_location = Utf8ToWide(agg.location);
    modRef.object_name = Utf8ToWide(agg.object_name);
    modRef.form_id = Utf8ToWide(agg.form_id);
    auto cit = espCount.find(crashlogger_core::AsciiLower(agg.esp_name));
    modRef.ref_count = (cit != espCount.end()) ? cit->second : 1;
    modRef.relevance_score = agg.relevance_score;
    out.crash_logger_object_refs.push_back(std::move(modRef));
  }
}

void ComputeSuspects(
  void* dumpBase,
  std::uint64_t dumpSize,
  const std::vector<ModuleInfo>& allModules,
  const std::optional<CONTEXT>& excCtx,
  bool hangLike,
  const AnalyzeOptions& opt,
  AnalysisResult& out)
{
  const bool shouldAnalyzeStacks = (out.exc_tid != 0) || hangLike;
  if (!shouldAnalyzeStacks) {
    return;
  }

  std::vector<std::uint32_t> tids;
  if (out.exc_tid != 0) {
    tids.push_back(out.exc_tid);
  } else if (out.has_wct) {
    tids = internal::ExtractWctCandidateThreadIds(out.wct_json_utf8, /*maxN=*/8);
  }
  if (out.has_blackbox) {
    if (auto mainTid = internal::InferMainThreadIdFromEvents(out.events)) {
      tids.push_back(*mainTid);
    }
  }
  if (tids.empty()) {
    return;
  }

  std::sort(tids.begin(), tids.end());
  tids.erase(std::unique(tids.begin(), tids.end()), tids.end());
  const auto threads = LoadThreads(dumpBase, dumpSize);
  if (!internal::TryComputeStackwalkSuspects(dumpBase, dumpSize, allModules, tids, out.exc_tid, excCtx, threads, opt.language, out)) {
    out.suspects_from_stackwalk = false;
    out.diagnostics.push_back(L"[Stackwalk] DbgHelp stackwalk failed, falling back to stack scan");
    out.suspects = internal::ComputeStackScanSuspects(dumpBase, dumpSize, allModules, tids, opt.language);
  }
}

}  // namespace skydiag::dump_tool

#include "AnalyzerPipeline.h"

#include "AnalyzerInternals.h"
#include "CrashLogger.h"
#include "CrashLoggerParseCore.h"
#include "DumpObjects.h"
#include "MainThreadWait.h"
#include "ModalDialogWait.h"
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
using skydiag::dump_tool::minidump::ExternalSoftwareName;
using skydiag::dump_tool::minidump::FindModuleIndexForAddress;
using skydiag::dump_tool::minidump::IsGameExeModule;
using skydiag::dump_tool::minidump::IsSystemishModuleAt;
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
    out.fault_module_plus_offset = minidump::ModulePlusOffsetText(m.filename, off);
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
  out.fault_module_external_software = ExternalSoftwareName(out.fault_module_filename, out.fault_module_path);

  if (!out.inferred_mod_name.empty()) {
    const std::wstring inferredLower = WideLower(out.inferred_mod_name);
    const std::wstring faultLower = WideLower(out.fault_module_filename);
    const bool inferredLooksBinaryName =
      (inferredLower.size() >= 4 && inferredLower.substr(inferredLower.size() - 4) == L".dll") ||
      (inferredLower.size() >= 4 && inferredLower.substr(inferredLower.size() - 4) == L".exe");
    const bool faultIsSystem = IsSystemishModuleAt(out.fault_module_filename, out.fault_module_path);
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
  if (snap->header.magic != skydiag::kMagic ||
      (ver != 1u && ver != 2u && ver != 3u && ver != skydiag::kVersion)) {
    return;
  }

  out.has_blackbox = true;
  out.pid = snap->header.pid;
  out.state_flags = snap->header.state_flags;
  out.blackbox_crash_seq = snap->header.crash_seq;
  out.blackbox_exception_code = snap->header.crash.exception_code;
  out.blackbox_faulting_tid = snap->header.crash.faulting_tid;
  out.blackbox_exception_addr = snap->header.crash.exception_addr;

  std::uint32_t cap = snap->header.capacity;
  if (cap == 0 || cap > skydiag::kEventCapacity) {
    cap = skydiag::kEventCapacity;
  }
  const std::uint64_t freq = snap->header.qpc_freq ? snap->header.qpc_freq : 1;
  const std::uint64_t start = snap->header.start_qpc;
  out.blackbox_last_heartbeat_ms = (snap->header.last_heartbeat_qpc > start)
    ? (1000.0 * (static_cast<double>(snap->header.last_heartbeat_qpc - start) / static_cast<double>(freq)))
    : -1.0;
  const std::uint32_t writeIndex = snap->header.write_index;
  const std::uint32_t begin = (writeIndex > cap) ? (writeIndex - cap) : 0;

  out.events.clear();
  out.events.reserve(static_cast<std::size_t>(std::min<std::uint32_t>(writeIndex, cap)));
  for (std::uint32_t i = begin; i < writeIndex; i++) {
    const auto& ev = snap->events[i % cap];
    const std::uint32_t seq1 = ev.seq;
    // The writer commits ring index i as seq i*2. Any other even value is a
    // slot a lap behind or one that two writers of different laps spliced.
    if ((seq1 & 1u) != 0u || seq1 != i * 2u) {
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
  Mo2FolderCache mo2FolderCache;

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
    // Committed as seq i*2, as in the event ring above.
    if ((seq1 & 1u) != 0u || seq1 != i * 2u) {
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
    // Plugins before v0.2.68 stored the game's ANSI path bytes as they came.
    rr.path = Utf8OrAnsiToWide(std::string_view(tmp.path_utf8, len));
    rr.kind = internal::ResourceKindFromPath(rr.path);
    if (mo2Index) {
      const std::wstring key = normMo2Key(rr.path);
      auto it = mo2ProvidersCache.find(key);
      if (it == mo2ProvidersCache.end()) {
        rr.providers = FindMo2ProvidersForDataPath(*mo2Index, rr.path, /*maxProviders=*/8, &mo2FolderCache);
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

void LoadPluginScanJson(
  const std::wstring& dumpPath,
  void* dumpBase,
  std::uint64_t dumpSize,
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
}

namespace {

// The MO2 and game folders below come from the dump: its module paths and
// the helper's plugin scan. Like the module files themselves (review M16),
// they are read only on a local fixed disk, so a crafted dump cannot make
// the analyzer list or open files on a UNC share.
bool IsLocalFixedDriveFolder(const std::filesystem::path& dir)
{
  return minidump::IsLocalFixedDrivePath(dir.native());
}

// The first module path under an MO2 "mods" folder on a local fixed disk, as
// the list the Mo2Index helpers take; empty when there is none. *refused (if
// given) is set when a module path names an MO2 folder elsewhere.
std::vector<std::wstring> LocalMo2ModulePath(const std::vector<std::wstring>& modulePaths, bool* refused)
{
  for (const auto& path : modulePaths) {
    const auto base = TryInferMo2BaseDirFromModulePaths({ path });
    if (!base) {
      continue;
    }
    if (IsLocalFixedDriveFolder(*base)) {
      return { path };
    }
    if (refused) {
      *refused = true;
    }
  }
  return {};
}

bool CapturedMo2FoldersAreLocal(const CapturedMo2State& state)
{
  for (const auto* dir : { &state.base, &state.modsDir, &state.overwriteDir, &state.profilesDir }) {
    if (!dir->empty() && !IsLocalFixedDriveFolder(*dir)) {
      return false;
    }
  }
  return true;
}

}  // namespace

std::optional<Mo2Index> BuildMo2IndexForDump(const std::vector<std::wstring>& modulePaths, AnalysisResult& out)
{
  std::optional<Mo2Index> idx;
  bool refused = false;
  auto captured = TryParseCapturedMo2State(out.plugin_scan_json_utf8);
  if (captured && !CapturedMo2FoldersAreLocal(*captured)) {
    refused = true;
    captured.reset();
  }
  if (captured) {
    idx = BuildMo2IndexFromCapture(*captured);
  } else {
    // A dump captured before the helper recorded MO2 state, or state it could
    // not read: what is on disk now may be another profile or changed mods.
    idx = TryBuildMo2IndexFromModulePaths(LocalMo2ModulePath(modulePaths, &refused));
  }
  if (refused && !idx) {
    out.diagnostics.push_back(L"[MO2] the dump names an MO2 folder that is not on a local fixed disk; not read");
  }
  if (idx) {
    out.mo2_providers_source = idx->source == Mo2Index::Source::kCrashTime ? "crash_time" : "analysis_time";
    out.mo2_profile_name = idx->profileName;
  }
  return idx;
}

void IntegratePluginScan(
  const std::wstring& dumpPath,
  const std::vector<ModuleInfo>& allModules,
  void* dumpBase,
  std::uint64_t dumpSize,
  const AnalyzeOptions& opt,
  AnalysisResult& out)
{
  if (!out.has_plugin_scan) {
    LoadPluginScanJson(dumpPath, dumpBase, dumpSize, out);
  }

  ParsedPluginScan parsedPluginScan{};
  bool parsedPluginScanOk = false;
  if (out.has_plugin_scan && !out.plugin_scan_json_utf8.empty()) {
    parsedPluginScanOk = ParsePluginScanJson(out.plugin_scan_json_utf8, &parsedPluginScan);
    if (parsedPluginScanOk) {
      out.missing_masters = ComputeMissingMasters(parsedPluginScan);
      out.skse_log = parsedPluginScan.skse_log;

      const bool hasHeader171 = AnyPluginHeaderVersionGte(parsedPluginScan, 1.71);
      // BEES ships SKSE\Plugins\BackportedESLSupport.dll; "bees.dll" is kept
      // for any build named after the mod (the Helper preflight checks both).
      bool hasBees = false;
      for (const auto& m : allModules) {
        const auto name = WideLower(m.filename);
        if (name == L"backportedeslsupport.dll" || name == L"bees.dll") {
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
      if (out.has_blackbox) {
        if (auto hbAge = internal::InferHeartbeatAgeFromEventsSec(out.events)) {
          if (*hbAge < internal::kNotHangHeartbeatAgeSec) {
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

crashlogger_core::CrashLoggerSystemNamedModFn CrashLoggerSystemNamedModTest(
  const std::vector<ModuleInfo>& allModules)
{
  return [&allModules](std::string_view moduleUtf8, std::uint64_t address) {
    const std::wstring module = Utf8ToWide(std::string(moduleUtf8));
    const auto isModCopy = [](const ModuleInfo& m) {
      return !IsSystemishModuleAt(m.filename, m.path) && m.external_software.empty();
    };
    // Such a proxy loads the Windows DLL of the same name too, so the frame
    // address picks the copy.
    if (address != 0) {
      if (const auto index = FindModuleIndexForAddress(allModules, address)) {
        const auto& m = allModules[*index];
        return WideLower(m.filename) == WideLower(module) && isModCopy(m);
      }
    }
    // Without one, only a name no Windows copy shares.
    bool found = false;
    for (const auto& m : allModules) {
      if (WideLower(m.filename) != WideLower(module)) {
        continue;
      }
      if (!isModCopy(m)) {
        return false;
      }
      found = true;
    }
    return found;
  };
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
      // CrashLogger.ini and a relative Crashlog Directory are read from here.
      if (const auto dir = std::filesystem::path(m.path).parent_path(); IsLocalFixedDriveFolder(dir)) {
        gameRootDir = dir;
      }
      break;
    }
  }

  const auto mo2Base = TryInferMo2BaseDirFromModulePaths(LocalMo2ModulePath(modulePaths, nullptr));
  CrashLoggerPairingMetadata pairing{};
  auto logPath = TryFindCrashLoggerLogForDump(
    dumpFs,
    mo2Base,
    mo2Index ? &*mo2Index : nullptr,
    gameRootDir,
    &clErr,
    &pairing);
  if (!logPath) {
    if (!clErr.empty()) {
      out.diagnostics.push_back(L"[CrashLogger] log not found: " + clErr);
    }
    return;
  }

  out.crash_logger_log_path = logPath->wstring();
  out.crash_logger_pairing_time_delta_ms = pairing.time_delta_ms;
  out.crash_logger_pairing_runner_up_time_delta_ms = pairing.runner_up_time_delta_ms;
  out.crash_logger_pairing_eligible_candidate_count = pairing.eligible_candidate_count;
  out.crash_logger_pairing_nearby_competitor_count = pairing.nearby_competitor_count;
  out.crash_logger_pairing_selected_kind = pairing.selected_kind;
  out.crash_logger_pairing_same_directory = pairing.selected_same_directory;
  out.crash_logger_pairing_ambiguous = pairing.ambiguous;
  out.crash_logger_pairing_confidence = pairing.ambiguous
    ? "low"
    : (pairing.time_delta_ms <= 10'000ull ? "high" : "medium");
  if (pairing.ambiguous) {
    out.diagnostics.push_back(
      L"[CrashLogger] ambiguous log pairing; nearby competing logs=" +
      std::to_wstring(pairing.nearby_competitor_count));
  }

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
  // A thread dump holds every thread's call stack, so its module ranking is a
  // tally over all of them, not the stack of the thread that faulted or
  // stalled: it names no suspect and feeds no evidence or advice.
  if (pairing.selected_kind == "thread_dump" || crashlogger_core::IsCrashLoggerThreadDumpLogAscii(*logUtf8)) {
    out.diagnostics.push_back(
      L"[CrashLogger] paired log is a thread dump; its call-stack modules are not used as suspects");
  } else {
    out.crash_logger_top_modules =
      ParseCrashLoggerTopModules(*logUtf8, canonicalByLower, CrashLoggerSystemNamedModTest(allModules));
  }

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

  for (const auto& type : crashlogger_core::ParseCrashLoggerObjectTypesAscii(*logUtf8)) {
    out.crash_logger_object_types.push_back(Utf8ToWide(type));
  }

  if (auto exceptionLine = crashlogger_core::ParseCrashLoggerExceptionLineAscii(*logUtf8)) {
    out.crash_logger_exception_type = Utf8ToWide(exceptionLine->type);
    out.crash_logger_exception_location = Utf8ToWide(exceptionLine->location);
    out.crash_logger_exception_instruction = Utf8ToWide(exceptionLine->instruction);
  }
  for (const auto& object : crashlogger_core::ParseCrashLoggerUnattributedObjectsAscii(*logUtf8)) {
    AnalysisResult::CrashLoggerModReference ref;
    ref.best_object_type = Utf8ToWide(object.object_type);
    ref.best_location = Utf8ToWide(object.location);
    ref.object_name = Utf8ToWide(object.object_name);
    ref.form_id = Utf8ToWide(object.form_id);
    ref.ref_count = 1;
    ref.relevance_score = object.relevance_score;
    out.crash_logger_unattributed_objects.push_back(std::move(ref));
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
  AnalysisResult& out,
  const AddressResolver* gameFunctions)
{
  const bool shouldAnalyzeStacks = (out.exc_tid != 0) || hangLike;
  if (!shouldAnalyzeStacks) {
    return;
  }

  std::vector<std::uint32_t> tids;
  std::optional<std::uint32_t> mainTid;
  if (hangLike && out.has_blackbox) {
    mainTid = internal::InferMainThreadIdFromEvents(out.events);
    if (mainTid) {
      tids.push_back(*mainTid);
    }
  }
  if (out.exc_tid != 0) {
    tids.push_back(out.exc_tid);
  } else if (mainTid.has_value()) {
    // ADR-0005: a freeze is judged from the game main thread. Other threads
    // join only when WCT proves they are in a wait cycle; long-waiting worker
    // threads (a plugin's own idle loop) are not evidence of the stall.
    if (const auto wct = internal::TryParseWctFreezeSummary(out.wct_json_utf8)) {
      for (const auto tid : wct->cycle_thread_ids) {
        tids.push_back(tid);
      }
    }
  } else if (out.has_wct) {
    for (const auto tid : internal::ExtractWctCandidateThreadIds(out.wct_json_utf8, /*maxN=*/8)) {
      tids.push_back(tid);
    }
  }
  if (tids.empty()) {
    return;
  }

  std::vector<std::uint32_t> uniqueTids;
  uniqueTids.reserve(tids.size());
  for (const auto tid : tids) {
    if (tid != 0u && std::find(uniqueTids.begin(), uniqueTids.end(), tid) == uniqueTids.end()) {
      uniqueTids.push_back(tid);
    }
  }
  tids = std::move(uniqueTids);
  const auto threads = LoadThreads(dumpBase, dumpSize);
  if (out.exc_tid != 0u && excCtx) {
    out.dump_objects = ReadDumpObjects(dumpBase, dumpSize, threads, allModules, *excCtx);
  }
  if (!hangLike) {
    out.hook_stub_race = internal::DetectHookStubRace(dumpBase, dumpSize, allModules, threads, out, excCtx);
  }
  const std::uint32_t preferredTid = out.exc_tid != 0u ? out.exc_tid : mainTid.value_or(0u);
  const std::uint32_t modalProbeTid = (hangLike && mainTid.has_value()) ? *mainTid : 0u;
  std::vector<ModalStackFrame> mainThreadFrames;
  // Threads the game itself started (BSJobs job threads and the like): the
  // main thread may be waiting for one of them (FindBlockedGameThread).
  std::vector<std::uint32_t> gameThreadTids;
  std::vector<ThreadTopFrames> gameThreadFrames;
  if (modalProbeTid != 0u) {
    const auto gameExe = std::find_if(allModules.begin(), allModules.end(), [](const ModuleInfo& m) { return m.is_game_exe; });
    if (gameExe != allModules.end()) {
      for (const auto& [tid, start] : minidump::LoadThreadStartAddresses(dumpBase, dumpSize)) {
        if (tid != modalProbeTid && start >= gameExe->base && start < gameExe->end) {
          gameThreadTids.push_back(tid);
        }
      }
    }
  }
  if (!internal::TryComputeStackwalkSuspects(
        dumpBase,
        dumpSize,
        allModules,
        tids,
        preferredTid,
        out.exc_tid,
        excCtx,
        threads,
        opt.language,
        out,
        modalProbeTid,
        &mainThreadFrames,
        gameThreadTids.empty() ? nullptr : &gameThreadTids,
        &gameThreadFrames,
        gameFunctions)) {
    out.suspects_from_stackwalk = false;
  }
  if (!out.suspects_from_stackwalk && out.stackwalk_only_thread_entry_hook_plugins) {
    // The walk reached the thread start and its only plugin frames wrap the
    // thread's entry: no plugin is on the crashing path. A pointer scan would
    // only add stale return addresses (Crash Logger's own, the MO2 VFS).
    out.diagnostics.push_back(
      L"[Stackwalk] complete walk; its only plugin frames wrap the thread entry, so no stack scan");
  } else if (!out.suspects_from_stackwalk) {
    // The helper treats the "failed" diagnostic as degraded capture quality,
    // so keep it for walks that produced no caller frames at all.
    out.diagnostics.push_back(out.stackwalk_total_frames >= 2u
      ? L"[Stackwalk] formal stackwalk found no actionable module, falling back to stack scan"
      : L"[Stackwalk] DbgHelp stackwalk failed, falling back to stack scan");
    const std::vector<std::uint32_t> scanTids =
      (hangLike && mainTid.has_value()) ? std::vector<std::uint32_t>{ *mainTid } : tids;
    out.suspects = internal::ComputeStackScanSuspects(
      dumpBase,
      dumpSize,
      allModules,
      scanTids,
      out.exc_tid,
      opt.language,
      excCtx ? &*excCtx : nullptr);
    // The walk or Crash Logger already showed these only wrap the thread's
    // entry; a raw stack slot holding their return address says no more.
    std::erase_if(out.suspects, [&](const SuspectItem& suspect) {
      return std::any_of(out.thread_entry_hook_modules.begin(), out.thread_entry_hook_modules.end(),
        [&](const std::wstring& hook) { return _wcsicmp(hook.c_str(), suspect.module_filename.c_str()) == 0; });
    });
    if (hangLike) {
      for (auto& suspect : out.suspects) {
        suspect.confidence_level = i18n::ConfidenceLevel::kLow;
        suspect.confidence = i18n::ConfidenceText(opt.language, suspect.confidence_level);
        suspect.reason += opt.language == i18n::Language::kEnglish
          ? L" (main-thread pointer scan only; weak freeze clue)"
          : L" (메인 스레드 포인터 스캔만 사용한 약한 프리징 단서)";
      }
    }
  }

  if (modalProbeTid != 0u) {
    const auto wctSummary = internal::TryParseWctFreezeSummary(out.wct_json_utf8);
    ModalDialogWaitInput modalInput{};
    modalInput.main_thread_id = modalProbeTid;
    modalInput.wct = wctSummary ? &*wctSummary : nullptr;
    modalInput.main_thread_frames = &mainThreadFrames;
    out.modal_dialog_wait = ResolveModalDialogWait(modalInput);
  }

  if (out.modal_dialog_wait.detected) {
    // The heartbeat stopped because the main thread is pumping a dialog.
    // Modules on that stack (or left over in its memory) are bystanders.
    for (auto& suspect : out.suspects) {
      suspect.confidence_level = i18n::ConfidenceLevel::kLow;
      suspect.confidence = i18n::ConfidenceText(opt.language, suspect.confidence_level);
      suspect.reason += opt.language == i18n::Language::kEnglish
        ? L" (main thread is waiting in a modal dialog; stack modules are not the hang cause)"
        : L" (메인 스레드가 modal 대화상자에서 대기 중이므로 스택 모듈은 프리징 원인이 아님)";
    }
    return;
  }

  if (modalProbeTid != 0u) {
    out.main_thread_wait = ClassifyMainThreadWait(mainThreadFrames);
    FindBlockedGameThread(out.main_thread_wait, gameThreadFrames);
    // The pointer-scan fallback reads only the main thread for freezes.
    out.suspects_from_main_thread =
      !out.suspects_from_stackwalk || out.stackwalk_primary_tid == modalProbeTid;
  }
  if (IsBystanderWait(out.main_thread_wait) && out.suspects_from_main_thread) {
    // The main thread was sitting in a wait it did not get from these
    // modules: the engine's own sleep/wait or the graphics driver. Plugins on
    // its stack were only on the call path (ADR-0009).
    for (auto& suspect : out.suspects) {
      suspect.confidence_level = i18n::ConfidenceLevel::kLow;
      suspect.confidence = i18n::ConfidenceText(opt.language, suspect.confidence_level);
      suspect.reason += opt.language == i18n::Language::kEnglish
        ? L" (main thread was waiting in " + out.main_thread_wait.waiting_module +
            L"; modules on its stack are on the call path, not shown to be the freeze cause)"
        : L" (메인 스레드가 " + out.main_thread_wait.waiting_module +
            L" 안에서 대기 중이었으므로 스택의 모듈은 호출 경로일 뿐 프리징 원인으로 확인된 것이 아님)";
    }
  }

  if (HasBlockedGameThread(out.main_thread_wait)) {
    // The main thread waited for a game thread that was inside this plugin's
    // code; that plugin, not the main thread's call path, is the lead.
    const auto& wait = out.main_thread_wait;
    const bool en = opt.language == i18n::Language::kEnglish;
    SuspectItem lead{};
    lead.confidence_level = i18n::ConfidenceLevel::kMedium;
    lead.confidence = i18n::ConfidenceText(opt.language, lead.confidence_level);
    lead.module_filename = wait.blocked_module;
    lead.module_path = wait.blocked_module_path;
    lead.inferred_mod_name = wait.blocked_mod_name;
    lead.score = out.suspects.empty() ? 1u : out.suspects.front().score + 1u;
    lead.reason = en
      ? (L"The main thread was waiting for game thread " + std::to_wstring(wait.blocked_thread_tid) +
          L", which was inside this plugin's code (top: " + wait.blocked_top + L")")
      : (L"메인 스레드가 기다린 게임 스레드(tid " + std::to_wstring(wait.blocked_thread_tid) +
          L")가 이 플러그인 코드 안에 있었음(맨 위: " + wait.blocked_top + L")");
    std::erase_if(out.suspects, [&](const SuspectItem& s) {
      return _wcsicmp(s.module_filename.c_str(), wait.blocked_module.c_str()) == 0;
    });
    out.suspects.insert(out.suspects.begin(), std::move(lead));
  }

  if (hangLike && mainTid.has_value() && !out.suspects.empty()) {
    constexpr std::size_t kNearStackSlots = 32u;
    constexpr std::size_t kMinimumThreadGroup = 4u;
    const auto matchingTids = internal::FindThreadsWithNearStackModule(
      dumpBase,
      dumpSize,
      allModules,
      out.suspects[0].module_filename,
      kNearStackSlots);
    const std::uint32_t stableCount = internal::CountWctThreadsWithStableContextSwitches(
      out.wct_json_utf8,
      matchingTids);
    const bool includesMainThread =
      std::find(matchingTids.begin(), matchingTids.end(), *mainTid) != matchingTids.end();
    if (includesMainThread && matchingTids.size() >= kMinimumThreadGroup &&
        static_cast<std::size_t>(stableCount) == matchingTids.size()) {
      out.hang_thread_module_consensus.has_consensus = true;
      out.hang_thread_module_consensus.main_thread_id = *mainTid;
      out.hang_thread_module_consensus.module_filename = out.suspects[0].module_filename;
      out.hang_thread_module_consensus.matching_thread_count = static_cast<std::uint32_t>(matchingTids.size());
      out.hang_thread_module_consensus.stable_thread_count = stableCount;
      out.hang_thread_module_consensus.os_lock_cycle_proven = false;
      out.suspects[0].reason += opt.language == i18n::Language::kEnglish
        ? (L" (same module appeared near the active stack of " + std::to_wstring(matchingTids.size()) +
            L" threads; all had zero context-switch delta across WCT passes)")
        : (L" (동일 모듈이 " + std::to_wstring(matchingTids.size()) +
            L"개 스레드의 현재 스택 상단에 반복되고 모두 WCT 간 context-switch 변화가 없음)");
    }
  }
}

}  // namespace skydiag::dump_tool

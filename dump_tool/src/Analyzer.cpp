#include "Analyzer.h"
#include "AddressResolver.h"
#include "Bucket.h"
#include "CrashHistory.h"
#include "EvidenceBuilder.h"
#include "GraphicsInjectionDiag.h"
#include "CrashLogger.h"
#include "CrashLoggerParseCore.h"
#include "AnalyzerInternals.h"
#include "MinidumpUtil.h"
#include "Mo2Index.h"
#include "Utf.h"

#include <Windows.h>

#include <DbgHelp.h>

#include <algorithm>
#include <cwctype>
#include <cstddef>
#include <cctype>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "SkyrimDiagProtocol.h"
#include "SkyrimDiagShared.h"

namespace skydiag::dump_tool {

using skydiag::dump_tool::minidump::FindModuleIndexForAddress;
using skydiag::dump_tool::minidump::GetThreadStackBytes;
using skydiag::dump_tool::minidump::IsGameExeModule;
using skydiag::dump_tool::minidump::IsKnownHookFramework;
using skydiag::dump_tool::minidump::IsLikelyWindowsSystemModulePath;
using skydiag::dump_tool::minidump::IsSystemishModule;
using skydiag::dump_tool::minidump::LoadHookFrameworksFromJson;
using skydiag::dump_tool::minidump::LoadAllModules;
using skydiag::dump_tool::minidump::LoadThreads;
using skydiag::dump_tool::minidump::MappedFile;
using skydiag::dump_tool::minidump::ModuleForAddress;
using skydiag::dump_tool::minidump::ModuleInfo;
using skydiag::dump_tool::minidump::ReadStreamSized;
using skydiag::dump_tool::minidump::ReadThreadContextWin64;
using skydiag::dump_tool::minidump::ThreadRecord;
using skydiag::dump_tool::minidump::WideLower;

std::string NowIso8601Utc()
{
  const std::time_t now = std::time(nullptr);
  if (now == static_cast<std::time_t>(-1)) {
    return {};
  }
  std::tm tmUtc{};
  if (gmtime_s(&tmUtc, &now) != 0) {
    return {};
  }
  char buf[32]{};
  if (std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmUtc) == 0) {
    return {};
  }
  return buf;
}

class ScopedHistoryFileLock
{
public:
  explicit ScopedHistoryFileLock(const std::filesystem::path& historyPath)
    : m_lockDir(historyPath.wstring() + L".lock")
  {
  }

  bool Acquire(DWORD timeoutMs, DWORD pollMs = 25)
  {
    if (m_acquired) {
      return true;
    }
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
      std::error_code ec;
      if (std::filesystem::create_directory(m_lockDir, ec)) {
        m_acquired = true;
        return true;
      }
      if (ec && ec != std::errc::file_exists) {
        return false;
      }
      if (GetTickCount64() >= deadline) {
        return false;
      }
      Sleep(pollMs);
    }
  }

  ~ScopedHistoryFileLock()
  {
    if (!m_acquired) {
      return;
    }
    std::error_code ec;
    std::filesystem::remove(m_lockDir, ec);
  }

private:
  std::filesystem::path m_lockDir;
  bool m_acquired = false;
};

bool TryReadTextFileUtf8(const std::filesystem::path& path, std::string* out)
{
  if (!out) {
    return false;
  }
  out->clear();

  std::ifstream f(path, std::ios::binary);
  if (!f.is_open()) {
    return false;
  }

  std::ostringstream ss;
  ss << f.rdbuf();
  *out = ss.str();
  return true;
}

std::wstring ConfidenceText(i18n::Language lang, i18n::ConfidenceLevel level)
{
  return std::wstring(i18n::ConfidenceLabel(lang, level));
}

std::uint32_t CrashLoggerRankBonus(std::size_t rank)
{
  if (rank == 0) {
    return 18u;
  }
  if (rank == 1) {
    return 14u;
  }
  if (rank == 2) {
    return 10u;
  }
  if (rank <= 4) {
    return 8u;
  }
  return 6u;
}

void ApplyCrashLoggerCorroborationToSuspects(AnalysisResult* out)
{
  if (!out || out->suspects.empty() || out->crash_logger_top_modules.empty()) {
    return;
  }

  std::unordered_map<std::wstring, std::size_t> rankByModule;
  rankByModule.reserve(out->crash_logger_top_modules.size());
  for (std::size_t i = 0; i < out->crash_logger_top_modules.size(); ++i) {
    const auto key = WideLower(out->crash_logger_top_modules[i]);
    if (key.empty()) {
      continue;
    }
    if (rankByModule.find(key) == rankByModule.end()) {
      rankByModule.emplace(key, i);
    }
  }
  if (rankByModule.empty()) {
    return;
  }

  const std::wstring cppModuleLower = WideLower(out->crash_logger_cpp_exception_module);
  struct Ranked
  {
    std::size_t index = 0;
    std::uint32_t score = 0;
    std::uint32_t bonus = 0;
    std::uint32_t effective = 0;
    bool matchedCppModule = false;
    std::size_t crashLoggerRank = 0;
  };

  std::vector<Ranked> rows;
  rows.reserve(out->suspects.size());
  for (std::size_t i = 0; i < out->suspects.size(); ++i) {
    const auto key = WideLower(out->suspects[i].module_filename);
    const auto it = rankByModule.find(key);
    if (it == rankByModule.end()) {
      rows.push_back(Ranked{ i, out->suspects[i].score, 0u, out->suspects[i].score, false, 0u });
      continue;
    }

    const bool matchedCpp = !cppModuleLower.empty() && (cppModuleLower == key);
    std::uint32_t bonus = CrashLoggerRankBonus(it->second);
    if (matchedCpp) {
      bonus += 10u;
    }
    rows.push_back(Ranked{ i, out->suspects[i].score, bonus, out->suspects[i].score + bonus, matchedCpp, it->second });
  }

  std::stable_sort(rows.begin(), rows.end(), [&](const Ranked& a, const Ranked& b) {
    if (a.effective != b.effective) {
      return a.effective > b.effective;
    }
    if (a.bonus != b.bonus) {
      return a.bonus > b.bonus;
    }
    if (a.score != b.score) {
      return a.score > b.score;
    }
    return a.index < b.index;
  });

  std::vector<SuspectItem> reordered;
  reordered.reserve(out->suspects.size());
  const bool en = (out->language == i18n::Language::kEnglish);
  for (const auto& r : rows) {
    auto item = out->suspects[r.index];
    if (r.bonus > 0) {
      item.reason += en
        ? (L" (Crash Logger corroboration bonus=+" + std::to_wstring(r.bonus)
          + L", rank=" + std::to_wstring(r.crashLoggerRank + 1u) + L")")
        : (L" (Crash Logger 교차검증 보정=+" + std::to_wstring(r.bonus)
          + L", 순위=" + std::to_wstring(r.crashLoggerRank + 1u) + L")");
      if (r.matchedCppModule) {
        item.reason += en
          ? L" (Crash Logger C++ exception module match)"
          : L" (Crash Logger C++ 예외 모듈 일치)";
      }
    }
    reordered.push_back(std::move(item));
  }

  out->suspects = std::move(reordered);

  if (!out->suspects.empty() &&
      !IsKnownHookFramework(out->suspects[0].module_filename) &&
      rankByModule.find(WideLower(out->suspects[0].module_filename)) != rankByModule.end()) {
    const auto topKey = WideLower(out->suspects[0].module_filename);
    const auto rank = rankByModule[topKey];
    if (rank <= 1 || (!cppModuleLower.empty() && topKey == cppModuleLower)) {
      out->suspects[0].confidence_level = i18n::ConfidenceLevel::kHigh;
    } else if (out->suspects[0].confidence_level == i18n::ConfidenceLevel::kLow) {
      out->suspects[0].confidence_level = i18n::ConfidenceLevel::kMedium;
    }
    out->suspects[0].confidence = ConfidenceText(out->language, out->suspects[0].confidence_level);
  }
}

bool AnalyzeDump(const std::wstring& dumpPath, const std::wstring& outDir, const AnalyzeOptions& opt, AnalysisResult& out, std::wstring* err)
{
  out = AnalysisResult{};
  out.language = opt.language;
  out.dump_path = dumpPath;
  out.out_dir = outDir;
  out.online_symbol_source_allowed = opt.allow_online_symbols;
  out.path_redaction_applied = opt.redact_paths;

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

  // Optional: allow external hook-framework list override.
  if (!opt.data_dir.empty()) {
    LoadHookFrameworksFromJson(std::filesystem::path(opt.data_dir) / L"hook_frameworks.json");
  }

  const auto allModules = LoadAllModules(dumpBase, dumpSize);
  if (!opt.game_version.empty()) {
    out.game_version = opt.game_version;
  } else {
    for (const auto& m : allModules) {
      if (m.is_game_exe && !m.version.empty()) {
        out.game_version = m.version;
        break;
      }
    }
  }

  std::vector<std::wstring> modulePaths;
  modulePaths.reserve(allModules.size());
  for (const auto& m : allModules) {
    if (!m.path.empty()) {
      modulePaths.push_back(m.path);
    }
  }
  const auto mo2Index = TryBuildMo2IndexFromModulePaths(modulePaths);
  std::unordered_map<std::wstring, std::vector<std::wstring>> mo2ProvidersCache;

  // Exception info
  std::optional<CONTEXT> excCtx;
  void* excPtr = nullptr;
  ULONG excSize = 0;
  if (ReadStreamSized(dumpBase, dumpSize, ExceptionStream, &excPtr, &excSize) && excPtr && excSize >= sizeof(MINIDUMP_EXCEPTION_STREAM)) {
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
      out.fault_module_offset = off;
      wchar_t buf[1024]{};
      swprintf_s(buf, L"%s+0x%llx", m.filename.c_str(), static_cast<unsigned long long>(off));
      out.fault_module_plus_offset = buf;
      out.inferred_mod_name = m.inferred_mod_name;
    } else if (auto m = ModuleForAddress(dumpBase, dumpSize, out.exc_addr)) {  // fallback
      out.fault_module_path = m->path;
      out.fault_module_filename = m->filename;
      out.fault_module_plus_offset = m->plusOffset;
      if (out.exc_addr >= m->base) {
        out.fault_module_offset = (out.exc_addr - m->base);
      }
      out.inferred_mod_name = InferMo2ModNameFromPath(out.fault_module_path);
    }
  }

  // Guard against treating system/game binary names as inferred mod names.
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

  // Graphics injection diagnostics (best-effort, data-driven via JSON rules).
  if (!opt.data_dir.empty()) {
    GraphicsInjectionDiag graphicsDiag;
    const auto rulesPath = std::filesystem::path(opt.data_dir) / L"graphics_injection_rules.json";
    if (graphicsDiag.LoadRules(rulesPath)) {
      std::vector<std::wstring> moduleFilenames;
      moduleFilenames.reserve(allModules.size());
      for (const auto& m : allModules) {
        if (!m.filename.empty()) {
          moduleFilenames.push_back(m.filename);
        }
      }
      out.graphics_env = graphicsDiag.DetectEnvironment(moduleFilenames);
      out.graphics_diag = graphicsDiag.Diagnose(
        moduleFilenames,
        out.fault_module_filename,
        opt.language == i18n::Language::kKorean);
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

      // Resource log (optional; v2+ includes it after the blackbox events).
      out.resources.clear();
      if (bbSize >= sizeof(skydiag::SharedLayout)) {
        const auto& rl = snap->resources;
        const std::uint32_t rCap = skydiag::kResourceCapacity;
        const std::uint32_t rWrite = rl.write_index;
        const std::uint32_t rBegin = (rWrite > rCap) ? (rWrite - rCap) : 0;

        out.resources.reserve(static_cast<std::size_t>(std::min<std::uint32_t>(rWrite, rCap)));

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
            // Provider mapping can be expensive in large modpacks. Cache by normalized rel-path.
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

  // Plugin scan stream (optional).
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
    if (TryReadTextFileUtf8(sidecarPath, &sidecarJson) && !sidecarJson.empty()) {
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
    if (pluginRules.LoadFromJson(rulesPath)) {
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

    // Best-effort override: some manual hotkey dumps are named "_Hang_" even when the game is fine.
    // If the last heartbeat is very recent, treat it as a snapshot (not hang-like).
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

  // Optional: Crash Logger SSE/AE log integration (best-effort)
  {
    // Manual snapshot dumps often include WCT even when the game is fine, so only search Crash Logger logs when the
    // capture looks crash/hang-like.
    const bool shouldSearchCrashLogger = (out.exc_code != 0) || nameCrash || hangLike;
    if (shouldSearchCrashLogger) {
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
      if (auto logPath = TryFindCrashLoggerLogForDump(dumpFs, mo2Base, mo2Index ? &*mo2Index : nullptr, gameRootDir, &clErr)) {
        out.crash_logger_log_path = logPath->wstring();

        std::wstring readErr;
        auto logUtf8 = ReadWholeFileUtf8(*logPath, &readErr);
        if (logUtf8) {
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
        tids = internal::ExtractWctCandidateThreadIds(out.wct_json_utf8, /*maxN=*/8);
      }
      if (out.has_blackbox) {
        if (auto mainTid = internal::InferMainThreadIdFromEvents(out.events)) {
          tids.push_back(*mainTid);
        }
      }
      if (!tids.empty()) {
        // Dedup
        std::sort(tids.begin(), tids.end());
        tids.erase(std::unique(tids.begin(), tids.end()), tids.end());
        const auto threads = LoadThreads(dumpBase, dumpSize);
        if (!internal::TryComputeStackwalkSuspects(dumpBase, dumpSize, allModules, tids, out.exc_tid, excCtx, threads, opt.language, out)) {
          out.suspects_from_stackwalk = false;
          out.suspects = internal::ComputeStackScanSuspects(dumpBase, dumpSize, allModules, tids, opt.language);
        }
      }
    }
  }

  ApplyCrashLoggerCorroborationToSuspects(&out);

  internal::ComputeCrashBucket(out);

  // Signature matching from external pattern DB.
  if (!opt.data_dir.empty()) {
    SignatureDatabase sigDb;
    const auto sigPath = std::filesystem::path(opt.data_dir) / L"crash_signatures.json";
    if (sigDb.LoadFromJson(sigPath)) {
      SignatureMatchInput input{};
      input.exc_code = out.exc_code;
      input.fault_module = out.fault_module_filename;
      input.fault_offset = out.fault_module_offset;
      input.exc_address = out.exc_addr;
      input.fault_module_is_system =
        IsSystemishModule(out.fault_module_filename) || IsLikelyWindowsSystemModulePath(out.fault_module_path);
      input.callstack_modules.reserve(
        out.stackwalk_primary_frames.size() +
        out.crash_logger_top_modules.size() +
        out.suspects.size());
      for (const auto& frame : out.stackwalk_primary_frames) {
        if (!frame.empty()) {
          input.callstack_modules.push_back(frame);
        }
      }
      for (const auto& m : out.crash_logger_top_modules) {
        if (!m.empty()) {
          input.callstack_modules.push_back(m);
        }
      }
      for (const auto& s : out.suspects) {
        if (!s.module_filename.empty()) {
          input.callstack_modules.push_back(s.module_filename);
        }
      }
      out.signature_match = sigDb.Match(input, opt.language == i18n::Language::kKorean);
    }
  }

  // Resolve game EXE offsets to known function names (best-effort).
  if (!opt.data_dir.empty() && !out.game_version.empty()) {
    AddressResolver resolver;
    const auto addrDb = std::filesystem::path(opt.data_dir) / L"address_db" / L"skyrimse_functions.json";
    if (resolver.LoadFromJson(addrDb, out.game_version)) {
      if (!out.fault_module_filename.empty() && IsGameExeModule(out.fault_module_filename)) {
        if (auto fn = resolver.Resolve(out.fault_module_offset)) {
          out.resolved_functions[out.fault_module_offset] = *fn;
        }
      }
    }
  }

  // Persist short rolling history and compute repeated-suspect stats.
  {
    std::filesystem::path historyDir;
    if (!opt.output_dir.empty()) {
      historyDir = opt.output_dir;
    } else if (!outDir.empty()) {
      historyDir = outDir;
    } else {
      historyDir = std::filesystem::path(dumpPath).parent_path();
    }

    if (!historyDir.empty()) {
      const auto historyPath = historyDir / "crash_history.json";
      ScopedHistoryFileLock historyLock(historyPath);
      if (historyLock.Acquire(/*timeoutMs=*/2000)) {
        CrashHistory history;
        history.LoadFromFile(historyPath);

        CrashHistoryEntry entry{};
        entry.timestamp_utc = NowIso8601Utc();
        entry.dump_file = WideToUtf8(std::filesystem::path(dumpPath).filename().wstring());
        entry.bucket_key = WideToUtf8(out.crash_bucket_key);
        if (!out.suspects.empty()) {
          entry.top_suspect = WideToUtf8(out.suspects[0].module_filename);
          entry.confidence = WideToUtf8(out.suspects[0].confidence);
          for (const auto& s : out.suspects) {
            if (!s.module_filename.empty()) {
              entry.all_suspects.push_back(WideToUtf8(s.module_filename));
            }
          }
        }
        if (out.signature_match) {
          entry.signature_id = out.signature_match->id;
        }

        history.AddEntry(std::move(entry));
        history.SaveToFile(historyPath);
        out.history_stats = history.GetModuleStats(20);

        const auto bucketStats = history.GetBucketStats(WideToUtf8(out.crash_bucket_key));
        if (bucketStats.count > 1) {
          out.history_correlation.count = bucketStats.count;
          out.history_correlation.first_seen = bucketStats.first_seen;
          out.history_correlation.last_seen = bucketStats.last_seen;
        }
      }
    }
  }

  // Best-effort troubleshooting guide matching.
  if (!opt.data_dir.empty()) {
    const auto guidesPath = std::filesystem::path(opt.data_dir) / L"troubleshooting_guides.json";
    try {
      std::ifstream gf(guidesPath);
      if (gf.is_open()) {
        const auto gj = nlohmann::json::parse(gf, nullptr, true);
        if (gj.contains("guides") && gj["guides"].is_array()) {
          const bool en = (opt.language == i18n::Language::kEnglish);
          const std::string excHex = [&]() {
            char buf[32]{};
            if (out.exc_code != 0) {
              snprintf(buf, sizeof(buf), "0x%08X", out.exc_code);
            }
            return std::string(buf);
          }();
          const std::string sigId = out.signature_match ? out.signature_match->id : "";
          const bool isHang = hangLike;
          const bool isLoading = (out.state_flags & skydiag::kState_Loading) != 0u;
          const bool isSnapshot = (out.exc_code == 0 && !hangLike);

          for (const auto& guide : gj["guides"]) {
            if (!guide.is_object() || !guide.contains("match")) continue;
            const auto& match = guide["match"];
            bool matched = true;

            if (match.contains("exc_code")) {
              if (match.value("exc_code", "") != excHex) { matched = false; }
            }
            if (matched && match.contains("signature_id")) {
              if (match.value("signature_id", "") != sigId) { matched = false; }
            }
            if (matched && match.contains("state_flags_contains")) {
              const auto req = match.value("state_flags_contains", "");
              if (req == "hang" && !isHang) { matched = false; }
              if (req == "loading" && !isLoading) { matched = false; }
              if (req == "snapshot" && !isSnapshot) { matched = false; }
            }

            if (matched) {
              const std::string titleKey = en ? "title_en" : "title_ko";
              const std::string stepsKey = en ? "steps_en" : "steps_ko";
              out.troubleshooting_title = Utf8ToWide(guide.value(titleKey, guide.value("title_en", "")));
              if (guide.contains(stepsKey) && guide[stepsKey].is_array()) {
                for (const auto& step : guide[stepsKey]) {
                  if (step.is_string()) {
                    out.troubleshooting_steps.push_back(Utf8ToWide(step.get<std::string>()));
                  }
                }
              }
              break;  // first match wins
            }
          }
        }
      }
    } catch (...) {
      // Best-effort; silently skip on failure.
    }
  }

  BuildEvidenceAndSummary(out, opt.language);
  if (err) err->clear();
  return true;
}

}  // namespace skydiag::dump_tool

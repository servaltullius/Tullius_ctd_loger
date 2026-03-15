#include "Analyzer.h"
#include "AnalyzerPipeline.h"
#include "AddressResolver.h"
#include "Bucket.h"
#include "CandidateConsensus.h"
#include "EvidenceBuilder.h"
#include "FreezeCandidateConsensus.h"
#include "GraphicsInjectionDiag.h"
#include "OutputWriterInternals.h"
#include "TroubleshootingGuide.h"
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
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

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


using internal::output_writer::ReadTextFileUtf8;
using internal::output_writer::DefaultOutDirForDump;
using internal::output_writer::FindIncidentManifestForDump;
using internal::output_writer::TryLoadIncidentManifestJson;

// Bonus scores for Crash Logger top-module ranking.
// Crash Logger already identifies likely suspects; higher rank → higher bonus.
constexpr std::uint32_t kCrashLoggerBonusRank0 = 18u;  // top suspect
constexpr std::uint32_t kCrashLoggerBonusRank1 = 14u;
constexpr std::uint32_t kCrashLoggerBonusRank2 = 10u;
constexpr std::uint32_t kCrashLoggerBonusRank3to4 = 8u;
constexpr std::uint32_t kCrashLoggerBonusDeep   = 6u;  // rank > 4

std::uint32_t CrashLoggerRankBonus(std::size_t rank)
{
  if (rank == 0) return kCrashLoggerBonusRank0;
  if (rank == 1) return kCrashLoggerBonusRank1;
  if (rank == 2) return kCrashLoggerBonusRank2;
  if (rank <= 4) return kCrashLoggerBonusRank3to4;
  return kCrashLoggerBonusDeep;
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
    out->suspects[0].confidence = i18n::ConfidenceText(out->language, out->suspects[0].confidence_level);
  }
}

// ===== Main analysis orchestrator =====

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

  // Exception info + fault module
  const auto excCtx = ParseExceptionInfo(dumpBase, dumpSize, out);
  ResolveFaultModule(dumpBase, dumpSize, allModules, out);

  // Graphics injection diagnostics (best-effort, data-driven via JSON rules).
  if (!opt.data_dir.empty()) {
    GraphicsInjectionDiag graphicsDiag;
    const auto rulesPath = std::filesystem::path(opt.data_dir) / L"graphics_injection_rules.json";
    if (!graphicsDiag.LoadRules(rulesPath)) {
      out.diagnostics.push_back(L"[Data] failed to load graphics_injection_rules.json");
    } else {
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
  ParseBlackboxStream(dumpBase, dumpSize, mo2Index, modulePaths, out);

  // WCT stream (optional)
  void* wctPtr = nullptr;
  ULONG wctSize = 0;
  if (ReadStreamSized(dumpBase, dumpSize, skydiag::protocol::kMinidumpUserStream_WctJson, &wctPtr, &wctSize) && wctPtr && wctSize > 0) {
    out.has_wct = true;
    out.wct_json_utf8.assign(static_cast<const char*>(wctPtr), static_cast<std::size_t>(wctSize));
  }

  // Plugin scan + rules
  IntegratePluginScan(dumpPath, allModules, dumpBase, dumpSize, opt, out);

  // Hang detection
  const bool hangLike = DetermineHangLike(nameHang, out);

  // Crash Logger integration (best-effort)
  {
    const bool shouldSearchCrashLogger = (out.exc_code != 0) || nameCrash || hangLike;
    if (shouldSearchCrashLogger) {
      IntegrateCrashLoggerLog(dumpPath, allModules, modulePaths, mo2Index, out);
    }
  }

  // Suspects (prefer callstack/stackwalk; fallback to stack scan)
  ComputeSuspects(dumpBase, dumpSize, allModules, excCtx, hangLike, opt, out);
  if (out.symbol_runtime_degraded) {
    out.diagnostics.push_back(L"[Symbols] degraded runtime environment detected; stackwalk/source lookup may be limited");
  }

  ApplyCrashLoggerCorroborationToSuspects(&out);

  internal::ComputeCrashBucket(out);

  // Signature matching from external pattern DB.
  if (!opt.data_dir.empty()) {
    SignatureDatabase sigDb;
    const auto sigPath = std::filesystem::path(opt.data_dir) / L"crash_signatures.json";
    if (!sigDb.LoadFromJson(sigPath)) {
      out.diagnostics.push_back(L"[Data] failed to load crash_signatures.json");
    } else {
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
    if (!resolver.LoadFromJson(addrDb, out.game_version)) {
      out.diagnostics.push_back(L"[Data] failed to load address_db/skyrimse_functions.json");
    } else {
      if (!out.fault_module_filename.empty() && IsGameExeModule(out.fault_module_filename)) {
        if (auto fn = resolver.Resolve(out.fault_module_offset)) {
          out.resolved_functions[out.fault_module_offset] = *fn;
        }
      }
    }
  }

  LoadIncidentCaptureProfile(dumpPath, outDir, out);
  const auto analysisTimestamp = NowIso8601Utc();
  const auto historyPath = ResolveCrashHistoryPath(dumpPath, outDir, opt);
  LoadCrashHistoryContext(historyPath, analysisTimestamp, out);

  // Best-effort troubleshooting guide matching.
  if (!opt.data_dir.empty()) {
    TroubleshootingGuideDatabase tsDb;
    const auto guidesPath = std::filesystem::path(opt.data_dir) / L"troubleshooting_guides.json";
    if (!tsDb.LoadFromJson(guidesPath)) {
      out.diagnostics.push_back(L"[Data] failed to load troubleshooting_guides.json");
    } else {
      TroubleshootingMatchInput tsInput{};
      tsInput.exc_code = out.exc_code;
      tsInput.signature_id = out.signature_match ? out.signature_match->id : "";
      tsInput.is_hang = hangLike;
      tsInput.is_loading = (out.state_flags & skydiag::kState_Loading) != 0u;
      tsInput.is_snapshot = (out.exc_code == 0 && !hangLike);
      if (auto result = tsDb.Match(tsInput, opt.language)) {
        out.troubleshooting_title = std::move(result->title);
        out.troubleshooting_steps = std::move(result->steps);
      }
    }
  }

  BlackboxFreezeSummary blackboxFreezeSummary{};
  if (out.has_blackbox) {
    blackboxFreezeSummary = internal::BuildBlackboxFreezeSummary(
      out.events,
      (out.state_flags & skydiag::kState_Loading) != 0u);
  }
  out.blackbox_freeze_summary = std::move(blackboxFreezeSummary);
  out.first_chance_summary = internal::BuildFirstChanceSummary(
    out.events,
    (out.state_flags & skydiag::kState_Loading) != 0u);

  BuildEvidenceAndSummary(out, opt.language);
  FreezeSignalInput freezeSignals{};
  freezeSignals.is_hang_like = out.is_hang_like;
  freezeSignals.is_snapshot_like = out.is_snapshot_like;
  freezeSignals.is_manual_capture = out.is_manual_capture;
  freezeSignals.loading_context = (out.state_flags & skydiag::kState_Loading) != 0u;
  freezeSignals.wct = internal::TryParseWctFreezeSummary(out.wct_json_utf8);
  if (out.blackbox_freeze_summary.has_context) {
    freezeSignals.blackbox = out.blackbox_freeze_summary;
  }
  freezeSignals.first_chance = out.first_chance_summary;
  freezeSignals.actionable_candidates = out.actionable_candidates;
  out.freeze_analysis = BuildFreezeCandidateConsensus(freezeSignals, opt.language);
  out.freeze_analysis.first_chance_context = out.first_chance_summary;
  if (out.freeze_analysis.has_analysis) {
    BuildEvidenceAndSummary(out, opt.language);
  }
  AppendCrashHistoryEntry(historyPath, dumpPath, analysisTimestamp, out);
  if (err) err->clear();
  return true;
}

}  // namespace skydiag::dump_tool

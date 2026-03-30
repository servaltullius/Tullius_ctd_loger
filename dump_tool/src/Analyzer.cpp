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

// Bounded corroboration bonuses from Crash Logger top-module ranking.
constexpr std::uint32_t kCrashLoggerBonusRank0 = 18u;  // top suspect
constexpr std::uint32_t kCrashLoggerBonusRank1 = 14u;
constexpr std::uint32_t kCrashLoggerBonusRank2 = 10u;
constexpr std::uint32_t kCrashLoggerBonusRank3to4 = 8u;
constexpr std::uint32_t kCrashLoggerBonusDeep   = 6u;  // rank > 4
constexpr std::uint32_t kCrashLoggerDirectFaultPromotion = 32u;
constexpr std::uint32_t kCrashLoggerFirstActionablePromotion = 20u;
constexpr std::uint32_t kCrashLoggerProbableStreakPromotion = 14u;
constexpr std::uint32_t kCrashLoggerCppExceptionSupport = 10u;

std::uint32_t CrashLoggerRankBonus(std::size_t rank)
{
  if (rank == 0) return kCrashLoggerBonusRank0;
  if (rank == 1) return kCrashLoggerBonusRank1;
  if (rank == 2) return kCrashLoggerBonusRank2;
  if (rank <= 4) return kCrashLoggerBonusRank3to4;
  return kCrashLoggerBonusDeep;
}

std::wstring NormalizeCrashLoggerModuleFilename(std::wstring_view module)
{
  if (module.empty()) {
    return {};
  }

  const std::size_t pathPos = module.find_last_of(L"\\/");
  if (pathPos == std::wstring_view::npos) {
    return std::wstring(module);
  }

  return std::wstring(module.substr(pathPos + 1));
}

std::wstring CanonicalizeCrashLoggerFrameModule(
  std::string_view moduleUtf8,
  const std::unordered_map<std::wstring, std::wstring>& canonicalByFilenameLower)
{
  const std::wstring module = NormalizeCrashLoggerModuleFilename(Utf8ToWide(std::string(moduleUtf8)));
  const std::wstring key = WideLower(module);
  if (const auto it = canonicalByFilenameLower.find(key); it != canonicalByFilenameLower.end()) {
    return it->second;
  }
  return module;
}

bool CanPromoteCrashLoggerFrameModule(std::wstring_view module)
{
  if (module.empty()) {
    return false;
  }
  return !IsKnownHookFramework(module) &&
         !IsSystemishModule(module) &&
         !IsGameExeModule(module);
}

void IntegrateCrashLoggerFrameSignals(
  const std::vector<ModuleInfo>& allModules,
  AnalysisResult* out)
{
  if (!out || out->crash_logger_log_path.empty()) {
    return;
  }

  std::wstring readErr;
  const auto logUtf8 = ReadWholeFileUtf8(std::filesystem::path(out->crash_logger_log_path), &readErr);
  if (!logUtf8) {
    if (!readErr.empty()) {
      out->diagnostics.push_back(L"[CrashLogger] failed to re-read log for frame signals: " + readErr);
    }
    return;
  }

  std::unordered_map<std::wstring, std::wstring> canonicalByFilenameLower;
  canonicalByFilenameLower.reserve(allModules.size());
  for (const auto& module : allModules) {
    if (!module.filename.empty()) {
      canonicalByFilenameLower.emplace(WideLower(module.filename), module.filename);
    }
  }

  const auto rawSignals = crashlogger_core::ParseCrashLoggerFrameSignalsAscii(*logUtf8);
  if (!rawSignals.direct_fault_module.empty()) {
    out->crash_logger_direct_fault_module =
      CanonicalizeCrashLoggerFrameModule(rawSignals.direct_fault_module, canonicalByFilenameLower);
  }
  if (!rawSignals.first_actionable_probable_module.empty()) {
    out->crash_logger_first_actionable_probable_module =
      CanonicalizeCrashLoggerFrameModule(rawSignals.first_actionable_probable_module, canonicalByFilenameLower);
  }
  if (!rawSignals.probable_streak_module.empty()) {
    out->crash_logger_probable_streak_module =
      CanonicalizeCrashLoggerFrameModule(rawSignals.probable_streak_module, canonicalByFilenameLower);
  }
  out->crash_logger_probable_streak_length = rawSignals.probable_streak_length;

  const bool directFaultEligible =
    CanPromoteCrashLoggerFrameModule(out->crash_logger_direct_fault_module);
  const bool firstActionableEligible =
    CanPromoteCrashLoggerFrameModule(out->crash_logger_first_actionable_probable_module);
  const bool probableStreakEligible =
    (out->crash_logger_probable_streak_length >= 2u) &&
    CanPromoteCrashLoggerFrameModule(out->crash_logger_probable_streak_module);

  out->crash_logger_frame_signal_strength = 0;
  if (directFaultEligible) {
    out->crash_logger_frame_signal_strength += kCrashLoggerDirectFaultPromotion;
  }
  if (firstActionableEligible) {
    out->crash_logger_frame_signal_strength += kCrashLoggerFirstActionablePromotion;
  }
  if (probableStreakEligible) {
    out->crash_logger_frame_signal_strength += kCrashLoggerProbableStreakPromotion;
  }
}

struct CrashLoggerFramePromotion
{
  std::uint32_t directFaultPromotion = 0;
  std::uint32_t firstActionablePromotion = 0;
  std::uint32_t probableStreakPromotion = 0;
  std::uint32_t cppExceptionSupport = 0;
  std::uint32_t topModuleRankBonus = 0;
  std::uint32_t frameSignalStrength = 0;
  std::uint32_t totalPromotion = 0;
};

void ApplyCrashLoggerCorroborationToSuspects(AnalysisResult* out)
{
  if (!out || out->suspects.empty() ||
      (out->crash_logger_top_modules.empty() &&
       out->crash_logger_frame_signal_strength == 0 &&
       out->crash_logger_cpp_exception_module.empty())) {
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
  const bool directFaultEligible =
    CanPromoteCrashLoggerFrameModule(out->crash_logger_direct_fault_module);
  const bool firstActionableEligible =
    CanPromoteCrashLoggerFrameModule(out->crash_logger_first_actionable_probable_module);
  const bool probableStreakEligible =
    (out->crash_logger_probable_streak_length >= 2u) &&
    CanPromoteCrashLoggerFrameModule(out->crash_logger_probable_streak_module);
  const std::wstring directFaultLower =
    directFaultEligible ? WideLower(out->crash_logger_direct_fault_module) : std::wstring{};
  const std::wstring firstActionableLower =
    firstActionableEligible ? WideLower(out->crash_logger_first_actionable_probable_module) : std::wstring{};
  const std::wstring probableStreakLower =
    probableStreakEligible ? WideLower(out->crash_logger_probable_streak_module) : std::wstring{};
  const std::wstring cppModuleLower = WideLower(out->crash_logger_cpp_exception_module);

  struct Ranked
  {
    std::size_t index = 0;
    std::uint32_t score = 0;
    CrashLoggerFramePromotion promotion;
    std::uint32_t effective = 0;
    bool matchedCppModule = false;
    std::optional<std::size_t> matchedRank;
  };

  std::vector<Ranked> rows;
  rows.reserve(out->suspects.size());
  for (std::size_t i = 0; i < out->suspects.size(); ++i) {
    const auto key = WideLower(out->suspects[i].module_filename);
    std::optional<std::size_t> matchedRank;
    if (const auto it = rankByModule.find(key); it != rankByModule.end()) {
      matchedRank = it->second;
    }

    const bool directFaultMatches = directFaultEligible && (directFaultLower == key);
    const bool firstActionableMatches = firstActionableEligible && (firstActionableLower == key);
    const bool probableStreakMatches = probableStreakEligible && (probableStreakLower == key);
    const bool cppModuleMatches = !cppModuleLower.empty() && (cppModuleLower == key);

    CrashLoggerFramePromotion promotion{};
    promotion.directFaultPromotion = directFaultMatches ? kCrashLoggerDirectFaultPromotion : 0u;
    promotion.firstActionablePromotion = firstActionableMatches ? kCrashLoggerFirstActionablePromotion : 0u;
    promotion.probableStreakPromotion = probableStreakMatches ? kCrashLoggerProbableStreakPromotion : 0u;
    promotion.cppExceptionSupport = cppModuleMatches ? kCrashLoggerCppExceptionSupport : 0u;
    promotion.topModuleRankBonus = matchedRank ? CrashLoggerRankBonus(*matchedRank) : 0u;
    promotion.frameSignalStrength = promotion.directFaultPromotion +
                                    promotion.firstActionablePromotion +
                                    promotion.probableStreakPromotion;
    promotion.totalPromotion = promotion.frameSignalStrength + promotion.cppExceptionSupport + promotion.topModuleRankBonus;

    Ranked row{};
    row.index = i;
    row.score = out->suspects[i].score;
    row.promotion = promotion;
    row.effective = row.score + row.promotion.totalPromotion;
    row.matchedCppModule = cppModuleMatches;
    row.matchedRank = matchedRank;
    rows.push_back(std::move(row));
  }

  std::stable_sort(rows.begin(), rows.end(), [&](const Ranked& a, const Ranked& b) {
    if (a.effective != b.effective) {
      return a.effective > b.effective;
    }
    if (a.promotion.frameSignalStrength != b.promotion.frameSignalStrength) {
      return a.promotion.frameSignalStrength > b.promotion.frameSignalStrength;
    }
    if (a.promotion.totalPromotion != b.promotion.totalPromotion) {
      return a.promotion.totalPromotion > b.promotion.totalPromotion;
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
    if (r.promotion.totalPromotion > 0) {
      item.reason += en
        ? (L" (Crash Logger frame promotion=+" + std::to_wstring(r.promotion.totalPromotion) + L")")
        : (L" (Crash Logger frame 승격=+" + std::to_wstring(r.promotion.totalPromotion) + L")");
      if (r.promotion.directFaultPromotion > 0) {
        item.reason += en ? L" (direct fault match)" : L" (direct fault 일치)";
      }
      if (r.promotion.firstActionablePromotion > 0) {
        item.reason += en ? L" (first actionable probable frame match)" : L" (첫 actionable probable frame 일치)";
      }
      if (r.promotion.probableStreakPromotion > 0) {
        item.reason += en ? L" (probable frame streak match)" : L" (probable frame streak 일치)";
      }
      if (r.matchedCppModule) {
        item.reason += en ? L" (Crash Logger C++ exception module support)"
                          : L" (Crash Logger C++ 예외 모듈 보강)";
      }
      if (r.promotion.topModuleRankBonus > 0 && r.matchedRank) {
        item.reason += en
          ? (L" (Crash Logger top-module rank=" + std::to_wstring(*r.matchedRank + 1u) + L")")
          : (L" (Crash Logger 상위 모듈 순위=" + std::to_wstring(*r.matchedRank + 1u) + L")");
      }
    }
    reordered.push_back(std::move(item));
  }

  out->suspects = std::move(reordered);

  if (!out->suspects.empty() && !rows.empty()) {
    const auto& topRow = rows.front();
    if (topRow.promotion.directFaultPromotion > 0) {
      out->suspects[0].confidence_level = i18n::ConfidenceLevel::kHigh;
    } else if (topRow.promotion.frameSignalStrength > 0 ||
               topRow.promotion.cppExceptionSupport > 0 ||
               (!topRow.matchedRank || *topRow.matchedRank <= 1u)) {
      if (out->suspects[0].confidence_level == i18n::ConfidenceLevel::kLow ||
          out->suspects[0].confidence_level == i18n::ConfidenceLevel::kUnknown) {
        out->suspects[0].confidence_level = i18n::ConfidenceLevel::kMedium;
      }
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
      IntegrateCrashLoggerFrameSignals(allModules, &out);
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
    AddressResolver::LoadStatus loadStatus = AddressResolver::LoadStatus::kOk;
    if (!resolver.LoadFromJson(addrDb, out.game_version, &loadStatus)) {
      switch (loadStatus) {
      case AddressResolver::LoadStatus::kFileOpenFailed:
        out.diagnostics.push_back(L"[Data] address_db/skyrimse_functions.json not found");
        break;
      case AddressResolver::LoadStatus::kMissingGameVersion:
        out.diagnostics.push_back(
          L"[Data] address_db/skyrimse_functions.json has no entry for game_version " +
          Utf8ToWide(out.game_version));
        break;
      case AddressResolver::LoadStatus::kInvalidJson:
      case AddressResolver::LoadStatus::kEmptyEntries:
      default:
        out.diagnostics.push_back(L"[Data] failed to load address_db/skyrimse_functions.json");
        break;
      }
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

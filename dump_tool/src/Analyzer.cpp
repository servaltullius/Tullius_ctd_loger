#include "Analyzer.h"
#include "AnalyzerPipeline.h"
#include "AnalyzerScoringPolicy.h"
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
#include "CleanExitEvidence.h"
#include "DumpIdentity.h"
#include "AnalyzerInternals.h"
#include "MinidumpUtil.h"
#include "MemorySnapshot.h"
#include "ProblematicModules.h"
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

using skydiag::dump_tool::minidump::IsExternalSoftwareModule;
using skydiag::dump_tool::minidump::IsGameExeModule;
using skydiag::dump_tool::minidump::IsKnownHookFramework;
using skydiag::dump_tool::minidump::IsLikelyWindowsSystemModulePath;
using skydiag::dump_tool::minidump::IsSystemishModule;
using skydiag::dump_tool::minidump::IsSystemishModuleAt;
using skydiag::dump_tool::minidump::LoadHookFrameworksFromJson;
using skydiag::dump_tool::minidump::LoadAllModules;
using skydiag::dump_tool::minidump::MappedFile;
using skydiag::dump_tool::minidump::ModuleInfo;
using skydiag::dump_tool::minidump::ReadStreamSized;
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
         !IsExternalSoftwareModule(module) &&
         !IsGameExeModule(module);
}

bool IsCrashLoggerFrameModuleLoadedFromSystemPath(
  std::wstring_view module,
  const std::vector<ModuleInfo>& allModules)
{
  const std::wstring key = WideLower(NormalizeCrashLoggerModuleFilename(module));
  if (key.empty()) {
    return false;
  }

  for (const auto& loaded : allModules) {
    // A GPU vendor tool or overlay is no more a mod than a Windows DLL.
    if (WideLower(loaded.filename) == key &&
        (IsLikelyWindowsSystemModulePath(loaded.path) || !loaded.external_software.empty())) {
      return true;
    }
  }
  return false;
}

// The paired Crash Logger log is about this crash: not an ambiguous time
// pairing, and its exception location ("KERNELBASE.dll+00C483A", possibly
// path-qualified) does not contradict the dump's fault. A fault inside Crash
// Logger itself is a secondary fault while it reported this crash, so the log
// still describes it. DowngradeCrashLoggerLogForOtherLocation already folds a
// contradicting location into the ambiguous flag; the check stays here so the
// gate holds on its own.
bool CrashLoggerLogDescribesThisCrash(const AnalysisResult& out)
{
  return !out.crash_logger_log_path.empty() && !out.crash_logger_pairing_ambiguous &&
         !crashlogger_core::CrashLoggerLocationContradictsDumpFault(
           out.crash_logger_exception_location,
           out.fault_module_filename,
           out.fault_module_offset);
}

// The time pairing (up to 120 s apart) can pick another crash's log even with
// no competing log nearby. When the log names a different exception location
// than the dump, handle its clues like an ambiguous pairing: still shown, but
// Low, and they neither reorder suspects nor raise candidates.
void DowngradeCrashLoggerLogForOtherLocation(AnalysisResult* out)
{
  if (!out || out->crash_logger_log_path.empty() ||
      !crashlogger_core::CrashLoggerLocationContradictsDumpFault(
        out->crash_logger_exception_location,
        out->fault_module_filename,
        out->fault_module_offset)) {
    return;
  }
  out->crash_logger_location_mismatch = true;
  out->crash_logger_pairing_ambiguous = true;
  out->crash_logger_pairing_confidence = "low";
  out->diagnostics.push_back(
    L"[CrashLogger] log exception location " + out->crash_logger_exception_location +
    L" differs from the dump fault " + out->fault_module_plus_offset +
    L"; treating the pairing as ambiguous");
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

  // An ENB/ReShade d3d11.dll or dxgi.dll outside Windows is a mod (review H4).
  const auto rawSignals =
    crashlogger_core::ParseCrashLoggerFrameSignalsAscii(*logUtf8, CrashLoggerSystemNamedModTest(allModules));
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
  for (const auto& hook : rawSignals.thread_entry_hook_modules) {
    const auto module = CanonicalizeCrashLoggerFrameModule(hook, canonicalByFilenameLower);
    if (std::find(out->thread_entry_hook_modules.begin(), out->thread_entry_hook_modules.end(), module) ==
        out->thread_entry_hook_modules.end()) {
      out->thread_entry_hook_modules.push_back(module);
    }
  }

  // A frame the parser placed in a mod named like a Windows DLL is promotable
  // by that frame; the name checks below would call it a Windows DLL.
  out->crash_logger_direct_fault_eligible =
    rawSignals.direct_fault_in_system_named_mod ||
    (CanPromoteCrashLoggerFrameModule(out->crash_logger_direct_fault_module) &&
     !IsCrashLoggerFrameModuleLoadedFromSystemPath(out->crash_logger_direct_fault_module, allModules));
  out->crash_logger_first_actionable_probable_eligible =
    rawSignals.first_actionable_probable_in_system_named_mod ||
    (CanPromoteCrashLoggerFrameModule(out->crash_logger_first_actionable_probable_module) &&
     !IsCrashLoggerFrameModuleLoadedFromSystemPath(
       out->crash_logger_first_actionable_probable_module,
       allModules));
  out->crash_logger_probable_streak_eligible =
    (out->crash_logger_probable_streak_length >= 2u) &&
    (rawSignals.probable_streak_in_system_named_mod ||
     (CanPromoteCrashLoggerFrameModule(out->crash_logger_probable_streak_module) &&
      !IsCrashLoggerFrameModuleLoadedFromSystemPath(
        out->crash_logger_probable_streak_module,
        allModules)));

  out->crash_logger_frame_signal_strength = 0;
  if (out->crash_logger_direct_fault_eligible) {
    out->crash_logger_frame_signal_strength += kCrashLoggerDirectFaultPromotion;
  }
  if (out->crash_logger_first_actionable_probable_eligible) {
    out->crash_logger_frame_signal_strength += kCrashLoggerFirstActionablePromotion;
  }
  if (out->crash_logger_probable_streak_eligible) {
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

void ApplyCrashLoggerCorroborationToSuspects(
  AnalysisResult* out,
  const std::vector<ModuleInfo>& allModules)
{
  if (!out || out->suspects.empty() ||
      (out->crash_logger_top_modules.empty() &&
       out->crash_logger_frame_signal_strength == 0 &&
       out->crash_logger_cpp_exception_module.empty())) {
    return;
  }
  if (out->crash_logger_pairing_ambiguous) {
    // Keep the parsed log visible as a low-confidence clue, but do not let an
    // ambiguous time pairing reorder independently derived stack suspects.
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
  const bool directFaultEligible = out->crash_logger_direct_fault_eligible;
  const bool firstActionableEligible = out->crash_logger_first_actionable_probable_eligible;
  const bool probableStreakEligible = out->crash_logger_probable_streak_eligible;
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
        : (L" (Crash Logger 프레임 가산=+" + std::to_wstring(r.promotion.totalPromotion) + L")");
      if (r.promotion.directFaultPromotion > 0) {
        item.reason += en ? L" (direct fault match)" : L" (DLL 안에서 바로 난 예외와 일치)";
      }
      if (r.promotion.firstActionablePromotion > 0) {
        item.reason += en ? L" (first actionable probable frame match)" : L" (첫 번째 유력 DLL 프레임과 일치)";
      }
      if (r.promotion.probableStreakPromotion > 0) {
        item.reason += en ? L" (probable frame streak match)" : L" (유력 DLL 프레임 연속과 일치)";
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
    const bool hasCrashLoggerSupport =
      topRow.promotion.frameSignalStrength > 0 ||
      topRow.promotion.cppExceptionSupport > 0 ||
      (topRow.matchedRank && *topRow.matchedRank <= 1u);
    out->suspects[0].confidence_level = internal::policy::ConfidenceAfterCrashLoggerCorroboration(
      out->suspects[0].confidence_level,
      hasCrashLoggerSupport,
      out->suspects_from_stackwalk);
    out->suspects[0].confidence = i18n::ConfidenceText(out->language, out->suspects[0].confidence_level);
  }
}

void CapPointerScanSuspectConfidence(AnalysisResult& out)
{
  if (out.suspects_from_stackwalk) {
    return;
  }
  // Pointer-density scans do not prove that a raw stack slot is a return
  // address. Keep their user-visible confidence Low even when Crash Logger
  // corroboration reorders the same module to the top. The blocked game
  // thread's plugin is not one of them: it comes from the walk of that
  // thread, so it keeps its Medium lead (ADR-0009, review L7).
  const bool hasBlockedLead = HasBlockedGameThread(out.main_thread_wait);
  for (auto& suspect : out.suspects) {
    if (hasBlockedLead &&
        _wcsicmp(suspect.module_filename.c_str(), out.main_thread_wait.blocked_module.c_str()) == 0) {
      continue;
    }
    suspect.confidence_level = i18n::ConfidenceLevel::kLow;
    suspect.confidence = i18n::ConfidenceText(out.language, suspect.confidence_level);
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
  if (!ComputeDumpIdentity(mf.file.get(), dumpBase, dumpSize, &out.dump_identity, err)) {
    return false;
  }

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
  LoadPluginScanJson(dumpPath, dumpBase, dumpSize, out);
  const auto mo2Index = BuildMo2IndexForDump(modulePaths, out);
  out.problematic_modules = FindProblematicModules(allModules);

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
      // ENB, ReShade and DXVK are recognised by DLLs named like the Windows
      // ones they replace (d3d11.dll, dxgi.dll, dinput8.dll, d3d9.dll). The
      // game loads the Windows copies from System32 anyway, so only a copy
      // from elsewhere (the game folder) is an injector; before this, every
      // report said ENB and ReShade were detected.
      std::vector<std::wstring> moduleFilenames;
      moduleFilenames.reserve(allModules.size());
      for (const auto& m : allModules) {
        if (!m.filename.empty() && !IsLikelyWindowsSystemModulePath(m.path)) {
          moduleFilenames.push_back(m.filename);
        }
      }
      const bool faultInWindows = IsLikelyWindowsSystemModulePath(out.fault_module_path);
      out.graphics_env = graphicsDiag.DetectEnvironment(moduleFilenames);
      out.graphics_diag = graphicsDiag.Diagnose(
        moduleFilenames,
        faultInWindows ? std::wstring{} : out.fault_module_filename,
        opt.language == i18n::Language::kKorean);
    }
  }

  // SkyrimDiag blackbox (optional)
  ParseBlackboxStream(dumpBase, dumpSize, mo2Index, modulePaths, out);
  TryConsumeCleanExitEvidence(dumpPath, out);

  // WCT stream (optional)
  void* wctPtr = nullptr;
  ULONG wctSize = 0;
  if (ReadStreamSized(dumpBase, dumpSize, skydiag::protocol::kMinidumpUserStream_WctJson, &wctPtr, &wctSize) && wctPtr && wctSize > 0) {
    out.has_wct = true;
    out.wct_json_utf8.assign(static_cast<const char*>(wctPtr), static_cast<std::size_t>(wctSize));
  }

  // RAM, commit and VRAM at capture time
  ReadMemorySnapshot(dumpBase, dumpSize, out.memory);
  if (out.has_wct) {
    // Freeze dumps taken from a PSS snapshot: the clone is charged to system
    // commit while the dump is written (ADR-0012).
    const auto wct = internal::TryParseWctFreezeSummary(out.wct_json_utf8);
    out.memory.commit_includes_snapshot_clone = wct && wct->pss_snapshot_used;
  }

  // Plugin scan + rules
  IntegratePluginScan(dumpPath, allModules, dumpBase, dumpSize, opt, out);

  // Hang detection
  const bool hangLike = DetermineHangLike(nameHang, out);

  // Crash Logger integration (best-effort)
  {
    const bool shouldSearchCrashLogger =
      opt.use_crash_logger &&
      !out.is_filtered_clean_exit &&
      ((out.exc_code != 0) || nameCrash || hangLike);
    if (shouldSearchCrashLogger) {
      IntegrateCrashLoggerLog(dumpPath, allModules, modulePaths, mo2Index, out);
      DowngradeCrashLoggerLogForOtherLocation(&out);
      IntegrateCrashLoggerFrameSignals(allModules, &out);
    }
  }

  // Names of SkyrimSE.exe functions, for game frames of the stack walk
  // (best-effort).
  AddressResolver gameFunctions;
  bool gameFunctionsLoaded = false;
  if (!opt.data_dir.empty() && !out.game_version.empty()) {
    const auto addrDb = std::filesystem::path(opt.data_dir) / L"address_db" / L"skyrimse_functions.json";
    AddressResolver::LoadStatus loadStatus = AddressResolver::LoadStatus::kOk;
    if (!gameFunctions.LoadFromJson(addrDb, out.game_version, &loadStatus)) {
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
      gameFunctionsLoaded = true;
    }
  }

  // Suspects (prefer callstack/stackwalk; fallback to stack scan)
  ComputeSuspects(dumpBase, dumpSize, allModules, excCtx, hangLike, opt, out, gameFunctionsLoaded ? &gameFunctions : nullptr);
  if (out.symbol_runtime_degraded) {
    out.diagnostics.push_back(L"[Symbols] degraded runtime environment detected; stackwalk/source lookup may be limited");
  }

  if (!out.modal_dialog_wait.detected) {
    // A modal wait already explains the hang; corroborating bystander stack
    // modules with Crash Logger frames would only re-promote them.
    ApplyCrashLoggerCorroborationToSuspects(&out, allModules);
  }
  CapPointerScanSuspectConfidence(out);
  if (out.hook_stub_race.detected) {
    // ADR-0016: the dump names whose hook stub the crash ran into; the stack
    // above an address outside every module is guesswork by comparison.
    const auto& race = out.hook_stub_race;
    const bool en = out.language == i18n::Language::kEnglish;
    SuspectItem lead{};
    lead.confidence_level = i18n::ConfidenceLevel::kHigh;
    lead.confidence = i18n::ConfidenceText(out.language, lead.confidence_level);
    lead.module_filename = race.target_module;
    lead.module_path = race.target_module_path;
    lead.inferred_mod_name = race.target_mod_name;
    lead.score = out.suspects.empty() ? 1u : out.suspects.front().score + 1u;
    lead.reason = en
      ? L"The crash ran the unfinished hook stub that now jumps into this plugin"
      : L"크래시가 아직 다 써지지 않은 이 플러그인의 후킹 점프 코드를 실행함";
    std::erase_if(out.suspects, [&](const SuspectItem& s) {
      return _wcsicmp(s.module_filename.c_str(), race.target_module.c_str()) == 0;
    });
    // The rest came from a stack that starts outside every module.
    for (auto& suspect : out.suspects) {
      suspect.confidence_level = i18n::ConfidenceLevel::kLow;
      suspect.confidence = i18n::ConfidenceText(out.language, suspect.confidence_level);
    }
    out.suspects.insert(out.suspects.begin(), std::move(lead));
  }

  if (out.is_filtered_clean_exit) {
    out.suspects.clear();
    out.actionable_candidates.clear();
    out.crash_logger_object_refs.clear();
    out.crash_logger_unattributed_objects.clear();
    out.dump_objects.clear();
    out.crash_logger_top_modules.clear();
    out.thread_entry_hook_modules.clear();
    out.stackwalk_truncation.clear();
    out.fault_unloaded_module.clear();
    out.hook_stub_race = {};
    out.crash_logger_object_types.clear();
    out.inferred_mod_name.clear();
    out.graphics_diag.reset();
    out.plugin_diagnostics.clear();
    out.missing_masters.clear();
    out.needs_bees = false;
  }

  internal::ComputeCrashBucket(out);

  // Signature matching from external pattern DB.
  if (!out.is_filtered_clean_exit && !opt.data_dir.empty()) {
    SignatureDatabase sigDb;
    const auto sigPath = std::filesystem::path(opt.data_dir) / L"crash_signatures.json";
    if (!sigDb.LoadFromJson(sigPath)) {
      out.diagnostics.push_back(L"[Data] failed to load crash_signatures.json");
    } else {
      SignatureMatchInput input{};
      input.exc_code = out.exc_code;
      input.game_version = out.game_version;
      input.fault_module = out.fault_module_filename;
      input.fault_offset = out.fault_module_offset;
      if (out.exc_code == 0xC0000005u && out.exc_info.size() >= 2u) {
        input.access_violation_address = out.exc_info[1];
      }
      input.fault_module_is_system = IsSystemishModuleAt(out.fault_module_filename, out.fault_module_path);
      input.callstack_modules.reserve(
        out.stackwalk_primary_frames.size() +
        out.crash_logger_top_modules.size() +
        out.suspects.size());
      for (const auto& frame : out.stackwalk_primary_frames) {
        if (!frame.empty()) {
          input.callstack_modules.push_back(frame);
        }
      }
      // A Crash Logger log is paired by time; only one that describes this
      // crash may feed a signature (a known root cause names a fix).
      const bool crashLoggerThisCrash = CrashLoggerLogDescribesThisCrash(out);
      if (crashLoggerThisCrash) {
        for (const auto& m : out.crash_logger_top_modules) {
          if (!m.empty()) {
            input.callstack_modules.push_back(m);
          }
        }
        input.cpp_exception_info = out.crash_logger_cpp_exception_info;
        input.object_types = out.crash_logger_object_types;
      }
      for (const auto& object : out.dump_objects) {
        if (!object.type_name.empty()) {
          input.object_types.push_back(object.type_name);
        }
      }
      for (const auto& s : out.suspects) {
        if (!s.module_filename.empty()) {
          input.callstack_modules.push_back(s.module_filename);
        }
      }
      out.signature_match = sigDb.Match(input, opt.language == i18n::Language::kKorean);
      // "may be a caught throw" does not fit a throw Crash Logger saw crash
      // the game (crash test, 2026-10-12).
      if (out.signature_match && out.signature_match->id == "CPP_EXCEPTION_HANDLED" &&
          IsCrashLoggerConfirmedCppException(out)) {
        out.signature_match.reset();
      }
    }
  }

  LoadIncidentCaptureProfile(dumpPath, outDir, out);
  const auto analysisTimestamp = NowIso8601Utc();
  const auto historyPath = ResolveCrashHistoryPath(dumpPath, outDir, opt);
  if (!out.is_filtered_clean_exit) {
    LoadCrashHistoryContext(historyPath, dumpPath, analysisTimestamp, out);
  }

  // Best-effort troubleshooting guide matching.
  if (!out.is_filtered_clean_exit && !opt.data_dir.empty()) {
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
      (out.state_flags & skydiag::kState_Loading) != 0u,
      out.blackbox_last_heartbeat_ms);
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
  if (out.hang_thread_module_consensus.has_consensus) {
    freezeSignals.thread_module_consensus = out.hang_thread_module_consensus;
  }
  if (out.modal_dialog_wait.detected) {
    freezeSignals.modal_dialog_wait = out.modal_dialog_wait;
  }
  if (!out.main_thread_wait.kind.empty()) {
    freezeSignals.main_thread_wait = out.main_thread_wait;
  }
  if (out.main_thread_lock_owner.tid != 0u) {
    freezeSignals.lock_owner = out.main_thread_lock_owner;
    freezeSignals.crash_logger_thread_dump_deadlock = out.crash_logger_thread_dump_deadlock;
  }
  if (out.engine_spin.loop_found) {
    freezeSignals.engine_spin = out.engine_spin;
  }
  freezeSignals.actionable_candidates = out.actionable_candidates;
  out.freeze_analysis = BuildFreezeCandidateConsensus(freezeSignals, opt.language);
  out.freeze_analysis.first_chance_context = out.first_chance_summary;
  // A lock-owner freeze state is known only now and is bucketed by the owner,
  // so the history read under the main thread's wait key is read again and
  // the candidates it fed are rebuilt from it.
  if (internal::RekeyLockOwnerFreezeBucket(out) && !out.is_filtered_clean_exit) {
    LoadCrashHistoryContext(historyPath, dumpPath, analysisTimestamp, out);
    out.freeze_analysis = {};
    BuildEvidenceAndSummary(out, opt.language);
    freezeSignals.actionable_candidates = out.actionable_candidates;
    out.freeze_analysis = BuildFreezeCandidateConsensus(freezeSignals, opt.language);
    out.freeze_analysis.first_chance_context = out.first_chance_summary;
  }
  if (out.freeze_analysis.state_id == "crash_logger_thread_dump_deadlock") {
    // The generic freeze checklist ends in "disable recently added mods",
    // which this cause does not call for.
    out.troubleshooting_title.clear();
    out.troubleshooting_steps.clear();
  }
  if (out.freeze_analysis.has_analysis) {
    BuildEvidenceAndSummary(out, opt.language);
  }
  if (!out.is_filtered_clean_exit) {
    AppendCrashHistoryEntry(historyPath, dumpPath, analysisTimestamp, out);
  }
  if (err) err->clear();
  return true;
}

}  // namespace skydiag::dump_tool

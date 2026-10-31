#include "EvidenceBuilder.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>

#include "EvidenceBuilderPrivate.h"
#include "MinidumpUtil.h"
#include "SkyrimDiagShared.h"
#include "WctTypes.h"

namespace skydiag::dump_tool {

namespace {

void BuildEvidenceAndSummaryImpl(AnalysisResult& r, i18n::Language lang)
{
  r.language = lang;
  const bool en = (lang == i18n::Language::kEnglish);

  r.evidence.clear();
  r.recommendations.clear();

  // A missing master stops the game while it loads plugins at startup. A
  // session that finished loading a save or cell had its masters, so the
  // scanned list is stale or not the one the game used: a clue, not a cause.
  if (internal::GameReachedWorld(r)) {
    for (auto& pd : r.plugin_diagnostics) {
      if (pd.rule_id == "MISSING_MASTER") {
        pd.confidence_level = i18n::ConfidenceLevel::kLow;
        pd.confidence = i18n::ConfidenceText(lang, pd.confidence_level);
      }
    }
  }

  // Capture type (best-effort from filename / streams)
  std::wstring lowerName = minidump::WideLower(std::filesystem::path(r.dump_path).filename().wstring());
  const bool nameCrash = (lowerName.find(L"_crash_") != std::wstring::npos);
  const bool nameHang = (lowerName.find(L"_hang_") != std::wstring::npos);
  const bool nameManual = (lowerName.find(L"_manual_") != std::wstring::npos);

  bool hasCrashEvent = false;
  bool hasHangEvent = false;
  for (const auto& ev : r.events) {
    if (ev.type == static_cast<std::uint16_t>(skydiag::EventType::kCrash)) {
      hasCrashEvent = true;
    }
    if (ev.type == static_cast<std::uint16_t>(skydiag::EventType::kHangMark)) {
      hasHangEvent = true;
    }
  }

  const auto wct = (r.has_wct && !r.wct_json_utf8.empty()) ? internal::TrySummarizeWct(r.wct_json_utf8) : std::nullopt;
  const auto hbAge = internal::InferHeartbeatAgeFromResultSec(r);
  const bool heartbeatSuggestsNotHang = hbAge && (*hbAge < internal::kNotHangHeartbeatAgeSec);
  const bool manualFromWct = wct && wct->has_capture && (wct->capture_kind == "manual");
  const bool wctSuggestsHang = wct && ((wct->cycles > 0) || wct->suggestsHang);
  const bool manualCaptureHint = nameManual || manualFromWct;

  const bool hasException = (r.exc_code != 0u);
  const bool handledCppException = (r.exc_code == 0xE06D7363u);
  const bool likelyHandledExceptionFalsePositive =
    handledCppException && !wctSuggestsHang && (manualCaptureHint || heartbeatSuggestsNotHang || nameCrash);
  // A manual capture can include a "Crash" blackbox marker due to handled exceptions (or false triggers),
  // but that does not necessarily mean the game actually CTD'd. Prefer exception stream presence for crash classification.
  const bool isCrashLike =
    !r.is_filtered_clean_exit &&
    (nameCrash || (hasException && !likelyHandledExceptionFalsePositive) ||
     (hasCrashEvent && !manualCaptureHint && !handledCppException));
  const bool nameHangEffective = nameHang && !manualFromWct && !heartbeatSuggestsNotHang;
  const bool isHangLike =
    !r.is_filtered_clean_exit &&
    (nameHangEffective || hasHangEvent || wctSuggestsHang);
  const bool isSnapshotLike = !isCrashLike && !isHangLike;
  const bool isManualCapture =
    !r.is_filtered_clean_exit &&
    (manualCaptureHint || (nameHang && isSnapshotLike));

  r.is_crash_like = isCrashLike;
  r.is_hang_like = isHangLike;
  r.is_snapshot_like = isSnapshotLike;
  r.is_manual_capture = isManualCapture;

  // A GPU vendor tool or overlay is not a mod to disable; it takes the system
  // DLL path with its own wording (FaultPlace).
  // An ENB/ReShade d3d11.dll in the game folder is a mod, not Windows.
  const bool isSystem = minidump::IsSystemishModuleAt(r.fault_module_filename, r.fault_module_path) ||
    !r.fault_module_external_software.empty();
  const bool hasModule = !r.fault_module_filename.empty();
  const bool isGameExe = minidump::IsGameExeModule(r.fault_module_filename);
  const bool isHookFramework = minidump::IsKnownHookFramework(r.fault_module_filename);
  const auto hitch = internal::ComputeHitchSummary(r.events);
  const std::wstring suspectBasis = r.suspects_from_stackwalk
    ? (en ? L"callstack" : L"콜스택")
    : (en ? L"stack scan" : L"스택 스캔");

  internal::EvidenceBuildContext ctx{};
  ctx.en = en;
  ctx.hasException = hasException;
  ctx.isFilteredCleanExit = r.is_filtered_clean_exit;
  ctx.isForcedClose = r.forced_close.detected && !r.is_filtered_clean_exit;
  ctx.isCrashLike = isCrashLike;
  ctx.isHangLike = isHangLike;
  ctx.isSnapshotLike = isSnapshotLike;
  ctx.isManualCapture = isManualCapture;
  ctx.hasModule = hasModule;
  ctx.isSystem = isSystem;
  ctx.externalSoftware = r.fault_module_external_software;
  ctx.isGameExe = isGameExe;
  ctx.isHookFramework = isHookFramework;
  ctx.hasSignatureMatch = r.signature_match.has_value();
  ctx.wctSuggestsHang = wctSuggestsHang;
  ctx.hitch = hitch;
  ctx.wct = wct;
  ctx.suspectBasis = suspectBasis;

  internal::BuildActionableCandidates(r, lang, ctx);
  for (auto& candidate : r.actionable_candidates) {
    candidate.standalone_callstack = internal::CandidateHasStandaloneCallstackSupport(r, candidate);
  }
  if (internal::IsWeakFaultLocationOnlySuspect(r, ctx) && !r.suspects.empty()) {
    auto& topSuspect = r.suspects[0];
    topSuspect.confidence_level = i18n::ConfidenceLevel::kLow;
    topSuspect.confidence = i18n::ConfidenceText(lang, topSuspect.confidence_level);
    topSuspect.reason += en
      ? L" (fault-location clue only; may be victim location)"
      : L" (fault-location 단서만 있음; 피해 위치일 수 있음)";
  }
  if (internal::FaultJumpedIntoFreedCode(r)) {
    // The module that called the freed code only used its address
    // (FaultJumpedIntoFreedCode): low, whatever its frame weight.
    const auto caller = minidump::WideLower(internal::FaultCallSiteModule(r));
    for (auto& suspect : r.suspects) {
      if (caller.empty() || minidump::WideLower(suspect.module_filename) != caller) {
        continue;
      }
      suspect.confidence_level = i18n::ConfidenceLevel::kLow;
      suspect.confidence = i18n::ConfidenceText(lang, suspect.confidence_level);
      suspect.reason += en
        ? L" (called code that had been freed; may be a victim of another plugin's removed hook)"
        : L" (해제된 코드를 호출했을 뿐; 다른 플러그인이 제거한 훅의 피해자일 수 있음)";
    }
  }
  internal::BuildEvidenceItems(r, lang, ctx);
  internal::BuildRecommendations(r, lang, ctx);
  r.summary_sentence = internal::BuildSummarySentence(r, lang, ctx);
  if (ctx.isForcedClose) {
    // The detail stays for mod authors; the player reads first that this is
    // no crash in play.
    r.summary_sentence = internal::ForcedCloseLead(r.forced_close, en) + L" " + r.summary_sentence;
  }
  r.quick_answer = internal::BuildQuickAnswer(r, lang, ctx);
}

}  // namespace

void BuildEvidenceAndSummary(AnalysisResult& r, i18n::Language lang)
{
  BuildEvidenceAndSummaryImpl(r, lang);
}

}  // namespace skydiag::dump_tool

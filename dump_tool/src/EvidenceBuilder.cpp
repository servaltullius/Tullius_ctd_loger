#include "EvidenceBuilder.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>

#include "EvidenceBuilderPrivate.h"
#include "MinidumpUtil.h"
#include "SkyrimDiagShared.h"

namespace skydiag::dump_tool {

namespace {

void BuildEvidenceAndSummaryImpl(AnalysisResult& r, i18n::Language lang)
{
  r.language = lang;
  const bool en = (lang == i18n::Language::kEnglish);

  r.evidence.clear();
  r.recommendations.clear();

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
  constexpr double kNotHangHeartbeatAgeSec = 5.0;
  const auto hbAge = internal::InferHeartbeatAgeFromResultSec(r);
  const bool heartbeatSuggestsNotHang = hbAge && (*hbAge < kNotHangHeartbeatAgeSec);
  const bool manualFromWct = wct && wct->has_capture && (wct->capture_kind == "manual");
  const bool wctSuggestsHang = wct && ((wct->cycles > 0) || wct->suggestsHang);
  const bool manualCaptureHint = nameManual || manualFromWct;

  const bool hasException = (r.exc_code != 0u);
  const bool handledCppException = (r.exc_code == 0xE06D7363u);
  const bool likelyHandledExceptionFalsePositive =
    handledCppException && !wctSuggestsHang && (manualCaptureHint || heartbeatSuggestsNotHang || nameCrash);
  // A manual capture can include a "Crash" blackbox marker due to handled exceptions (or false triggers),
  // but that does not necessarily mean the game actually CTD'd. Prefer exception stream presence for crash classification.
  const bool isCrashLike = nameCrash || (hasException && !likelyHandledExceptionFalsePositive) || (hasCrashEvent && !manualCaptureHint && !handledCppException);
  const bool nameHangEffective = nameHang && !manualFromWct && !heartbeatSuggestsNotHang;
  const bool isHangLike = nameHangEffective || hasHangEvent || wctSuggestsHang;
  const bool isSnapshotLike = !isCrashLike && !isHangLike;
  const bool isManualCapture = manualCaptureHint || (nameHang && isSnapshotLike);

  r.is_crash_like = isCrashLike;
  r.is_hang_like = isHangLike;
  r.is_snapshot_like = isSnapshotLike;
  r.is_manual_capture = isManualCapture;

  const bool isSystem = minidump::IsSystemishModule(r.fault_module_filename) || minidump::IsLikelyWindowsSystemModulePath(r.fault_module_path);
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
  ctx.isCrashLike = isCrashLike;
  ctx.isHangLike = isHangLike;
  ctx.isSnapshotLike = isSnapshotLike;
  ctx.isManualCapture = isManualCapture;
  ctx.hasModule = hasModule;
  ctx.isSystem = isSystem;
  ctx.isGameExe = isGameExe;
  ctx.isHookFramework = isHookFramework;
  ctx.hasSignatureMatch = r.signature_match.has_value();
  ctx.wctSuggestsHang = wctSuggestsHang;
  ctx.hitch = hitch;
  ctx.wct = wct;
  ctx.suspectBasis = suspectBasis;

  internal::BuildEvidenceItems(r, lang, ctx);
  internal::BuildRecommendations(r, lang, ctx);
  r.summary_sentence = internal::BuildSummarySentence(r, lang, ctx);
}

}  // namespace

void BuildEvidenceAndSummary(AnalysisResult& r, i18n::Language lang)
{
  BuildEvidenceAndSummaryImpl(r, lang);
}

}  // namespace skydiag::dump_tool

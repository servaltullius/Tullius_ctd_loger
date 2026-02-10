#include "EvidenceBuilderInternals.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>

#include "EvidenceBuilderInternalsPriv.h"
#include "SkyrimDiagShared.h"

namespace skydiag::dump_tool::internal {

void BuildEvidenceAndSummaryImpl(AnalysisResult& r, i18n::Language lang)
{
  r.language = lang;
  const bool en = (lang == i18n::Language::kEnglish);

  r.evidence.clear();
  r.recommendations.clear();

  // Capture type (best-effort from filename / streams)
  std::wstring lowerName = std::filesystem::path(r.dump_path).filename().wstring();
  std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
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

  const auto wct = (r.has_wct && !r.wct_json_utf8.empty()) ? TrySummarizeWct(r.wct_json_utf8) : std::nullopt;
  constexpr double kNotHangHeartbeatAgeSec = 5.0;
  const auto hbAge = InferHeartbeatAgeFromResultSec(r);
  const bool heartbeatSuggestsNotHang = hbAge && (*hbAge < kNotHangHeartbeatAgeSec);
  const bool manualFromWct = wct && wct->has_capture && (wct->capture_kind == "manual");
  const bool wctSuggestsHang = wct && ((wct->cycles > 0) || wct->suggestsHang);
  const bool manualCaptureHint = nameManual || manualFromWct;

  const bool hasException = (r.exc_code != 0u);
  // A manual capture can include a "Crash" blackbox marker due to handled exceptions (or false triggers),
  // but that does not necessarily mean the game actually CTD'd. Prefer exception stream presence for crash classification.
  const bool isCrashLike = nameCrash || hasException || (hasCrashEvent && !manualCaptureHint);
  const bool nameHangEffective = nameHang && !manualFromWct && !heartbeatSuggestsNotHang;
  const bool isHangLike = nameHangEffective || hasHangEvent || wctSuggestsHang;
  const bool isSnapshotLike = !isCrashLike && !isHangLike;
  const bool isManualCapture = manualCaptureHint || (nameHang && isSnapshotLike);

  const bool isSystem = IsSystemishModule(r.fault_module_filename);
  const bool hasModule = !r.fault_module_filename.empty();
  const bool isGameExe = IsGameExeModule(r.fault_module_filename);
  const auto hitch = ComputeHitchSummary(r.events);
  const std::wstring suspectBasis = r.suspects_from_stackwalk
    ? (en ? L"callstack" : L"콜스택")
    : (en ? L"stack scan" : L"스택 스캔");

  EvidenceBuildContext ctx{};
  ctx.en = en;
  ctx.hasException = hasException;
  ctx.isCrashLike = isCrashLike;
  ctx.isHangLike = isHangLike;
  ctx.isSnapshotLike = isSnapshotLike;
  ctx.isManualCapture = isManualCapture;
  ctx.hasModule = hasModule;
  ctx.isSystem = isSystem;
  ctx.isGameExe = isGameExe;
  ctx.wctSuggestsHang = wctSuggestsHang;
  ctx.hitch = hitch;
  ctx.wct = wct;
  ctx.suspectBasis = suspectBasis;

  BuildEvidenceItems(r, lang, ctx);
  BuildRecommendations(r, lang, ctx);
  r.summary_sentence = BuildSummarySentence(r, lang, ctx);
}

}  // namespace skydiag::dump_tool::internal

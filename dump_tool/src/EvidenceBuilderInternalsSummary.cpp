#include "EvidenceBuilderInternalsPriv.h"

#include <cwchar>

namespace skydiag::dump_tool::internal {

std::wstring BuildSummarySentence(const AnalysisResult& r, i18n::Language lang, const EvidenceBuildContext& ctx)
{
  const bool en = ctx.en;
  const bool isSnapshotLike = ctx.isSnapshotLike;
  const bool isHangLike = ctx.isHangLike;
  const bool isManualCapture = ctx.isManualCapture;
  const bool hasModule = ctx.hasModule;
  const bool isSystem = ctx.isSystem;
  const bool isGameExe = ctx.isGameExe;
  const auto& wct = ctx.wct;
  const std::wstring& suspectBasis = ctx.suspectBasis;

  const bool hasSuspect = !r.suspects.empty();
  std::wstring suspectWho;
  std::wstring suspectConf;
  if (hasSuspect) {
    const auto& s0 = r.suspects[0];
    suspectConf = s0.confidence.empty() ? ConfidenceText(lang, i18n::ConfidenceLevel::kMedium) : s0.confidence;
    if (!s0.inferred_mod_name.empty()) {
      suspectWho = s0.inferred_mod_name + L" (" + s0.module_filename + L")";
    } else {
      suspectWho = s0.module_filename;
    }
  }

  std::wstring who;
  if (!r.inferred_mod_name.empty()) {
    who = r.inferred_mod_name + L" (" + r.fault_module_filename + L")";
  } else if (!r.fault_module_filename.empty()) {
    who = r.fault_module_filename;
  } else {
    who = en ? L"(unknown)" : L"(알 수 없음)";
  }

  std::wstring summary;

  if (isSnapshotLike) {
    summary = isManualCapture
      ? (en
          ? L"Looks like a manual snapshot. This alone does not prove a problem. (Confidence: High)"
          : L"수동 캡처 스냅샷으로 보입니다. 이 결과만으로 '문제가 있다'고 단정할 수 없습니다. (신뢰도: 높음)")
      : (en
          ? L"Looks like a snapshot dump (not a crash/hang). Useful for state inspection, not root cause. (Confidence: High)"
          : L"스냅샷 덤프(크래시/행 아님)로 보입니다. 원인 판정용이 아니라 '상태 확인'에 유용합니다. (신뢰도: 높음)");
  } else if (hasModule && !isSystem && !isGameExe) {
    summary = en
      ? (L"Top suspect: " + who + L" — the crash appears to occur inside this DLL. (Confidence: High)")
      : (L"유력 후보: " + who + L" — 해당 DLL 내부에서 크래시가 발생한 것으로 보입니다. (신뢰도: 높음)");
  } else if (hasModule && isSystem) {
    if (hasSuspect && !suspectWho.empty()) {
      summary = en
        ? (L"Crash is reported in a Windows system DLL, but " + suspectBasis + L" points to " + suspectWho +
            L". (Confidence: " + suspectConf + L")")
        : (L"크래시가 Windows 시스템 DLL에서 보고되었지만, " + suspectBasis + L"에서는 " + suspectWho +
            L" 가 유력합니다. (신뢰도: " + suspectConf + L")");
    } else if (r.exc_code == 0xE06D7363u) {
      summary = en
        ? L"Reported in a Windows system DLL with 0xE06D7363 (C++ exception). Could be normal throw/catch; confirm this was an actual CTD. (Confidence: Low)"
        : L"0xE06D7363(C++ 예외)로 Windows 시스템 DLL에서 보고되었습니다. 정상 동작 중 throw/catch일 수도 있어 실제 CTD 여부 확인이 필요합니다. (신뢰도: 낮음)";
    } else {
      summary = en
        ? L"Crash is reported in a Windows system DLL. The real culprit may be another mod/DLL. (Confidence: Low)"
        : L"크래시가 Windows 시스템 DLL에서 보고되었습니다. 실제 원인은 다른 모드/DLL일 수 있습니다. (신뢰도: 낮음)";
    }
  } else if (hasModule && isGameExe) {
    if (hasSuspect && !suspectWho.empty()) {
      summary = en
        ? (L"Crash is reported in the game executable, but " + suspectBasis + L" points to " + suspectWho +
            L". (Confidence: " + suspectConf + L")")
        : (L"크래시 위치가 게임 본체(EXE)로 보고되었지만, " + suspectBasis + L"에서는 " + suspectWho +
            L" 가 유력합니다. (신뢰도: " + suspectConf + L")");
    } else {
      summary = en
        ? L"Crash is reported in the game executable. Version mismatch/hook conflict is possible. (Confidence: Medium)"
        : L"크래시 위치가 게임 본체(EXE)로 보고되었습니다. 버전 불일치/후킹 충돌 가능성이 있습니다. (신뢰도: 중간)";
    }
  } else {
    if (isHangLike) {
      std::wstring hangPrefix = en ? L"Likely a freeze/infinite loading." : L"프리징/무한로딩으로 추정됩니다.";
      if (wct && wct->has_capture && wct->thresholdSec > 0u) {
        const std::wstring kindW = ToWideAscii(wct->capture_kind);
        wchar_t hb[256]{};
        swprintf_s(
          hb,
          en ? L"Hang detected (capture=%s, heartbeatAge=%.1fs >= %us)." : L"프리징 감지(capture=%s, heartbeatAge=%.1fs >= %us).",
          kindW.c_str(),
          wct->secondsSinceHeartbeat,
          wct->thresholdSec);
        hangPrefix = hb;
      }

      if (hasSuspect && !suspectWho.empty()) {
        summary = en
          ? (hangPrefix + L" Candidate: " + suspectWho + L" — based on " + suspectBasis + L" heuristic. (Confidence: " + suspectConf + L")")
          : (hangPrefix + L" 후보: " + suspectWho + L" — " + suspectBasis + L" 기반 추정입니다. (신뢰도: " + suspectConf + L")");
      } else {
        summary = en
          ? (hangPrefix + L" Dump alone isn't enough to identify a candidate. (Confidence: Low)")
          : (hangPrefix + L" 덤프만으로 후보를 특정하기 어렵습니다. (신뢰도: 낮음)");
      }
    } else {
      if (hasSuspect && !suspectWho.empty()) {
        summary = en
          ? (L"Top suspect: " + suspectWho + L" — based on " + suspectBasis + L" heuristic. (Confidence: " + suspectConf + L")")
          : (L"유력 후보: " + suspectWho + L" — " + suspectBasis + L" 기반 추정입니다. (신뢰도: " + suspectConf + L")");
      } else {
        summary = en
          ? L"Dump alone isn't enough to identify a top suspect. (Confidence: Low)"
          : L"덤프만으로 유력 후보를 특정하기 어렵습니다. (신뢰도: 낮음)";
      }
    }
  }

  return summary;
}

}  // namespace skydiag::dump_tool::internal


#include "EvidenceBuilderInternalsPriv.h"

#include <cwchar>

namespace skydiag::dump_tool::internal {

std::wstring BuildSummarySentence(const AnalysisResult& r, i18n::Language lang, const EvidenceBuildContext& ctx)
{
  const bool en = ctx.en;
  if (r.signature_match.has_value()) {
    const auto& sig = *r.signature_match;
    return en
      ? (L"Known pattern [" + ToWideAscii(sig.id) + L"]: " + sig.cause + L" (Confidence: " + sig.confidence + L")")
      : (L"알려진 패턴 [" + ToWideAscii(sig.id) + L"]: " + sig.cause + L" (신뢰도: " + sig.confidence + L")");
  }

  const bool isSnapshotLike = ctx.isSnapshotLike;
  const bool isHangLike = ctx.isHangLike;
  const bool isManualCapture = ctx.isManualCapture;
  const bool hasModule = ctx.hasModule;
  const bool isSystem = ctx.isSystem;
  const bool isGameExe = ctx.isGameExe;
  const auto& wct = ctx.wct;
  const std::wstring& suspectBasis = ctx.suspectBasis;

  const bool hasSuspect = !r.suspects.empty();
  const SuspectItem* topSuspect = hasSuspect ? &r.suspects[0] : nullptr;
  const SuspectItem* firstNonHookSuspect = nullptr;
  if (hasSuspect) {
    for (const auto& s : r.suspects) {
      if (!IsKnownHookFramework(s.module_filename)) {
        firstNonHookSuspect = &s;
        break;
      }
    }
  }

  auto suspectDisplayName = [&](const SuspectItem& s) {
    if (!s.inferred_mod_name.empty()) {
      return s.inferred_mod_name + L" (" + s.module_filename + L")";
    }
    return s.module_filename;
  };

  std::wstring suspectWho;
  std::wstring suspectConf;
  if (topSuspect) {
    suspectConf = topSuspect->confidence.empty() ? ConfidenceText(lang, i18n::ConfidenceLevel::kMedium) : topSuspect->confidence;
    suspectWho = suspectDisplayName(*topSuspect);
  }

  std::wstring nonHookSuspectWho;
  std::wstring nonHookSuspectConf;
  if (firstNonHookSuspect) {
    nonHookSuspectConf =
      firstNonHookSuspect->confidence.empty() ? ConfidenceText(lang, i18n::ConfidenceLevel::kMedium) : firstNonHookSuspect->confidence;
    nonHookSuspectWho = suspectDisplayName(*firstNonHookSuspect);
  }
  const bool hasNonHookSuspect = (firstNonHookSuspect != nullptr);
  const bool topSuspectIsHookFramework = (topSuspect != nullptr) && IsKnownHookFramework(topSuspect->module_filename);

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
  } else if (hasModule && !isSystem && !isGameExe && ctx.isHookFramework) {
    if (hasNonHookSuspect && !nonHookSuspectWho.empty()) {
      summary = en
        ? (L"Crash is reported in " + who + L" (known hook framework), but " + suspectBasis + L" points to " + nonHookSuspectWho +
            L". (Confidence: " + nonHookSuspectConf + L")")
        : (L"크래시 위치가 " + who + L"(알려진 훅 프레임워크)로 보고되었지만, " + suspectBasis + L"에서는 " + nonHookSuspectWho +
            L" 가 유력합니다. (신뢰도: " + nonHookSuspectConf + L")");
    } else {
      summary = en
        ? (L"Top suspect: " + who + L" (known hook framework; may be a victim of another mod's corruption) — the crash appears to occur inside this DLL. (Confidence: Medium)")
        : (L"유력 후보: " + who + L" (알려진 훅 프레임워크; 다른 모드의 메모리 오염 피해자일 수 있음) — 해당 DLL 내부에서 크래시가 발생한 것으로 보입니다. (신뢰도: 중간)");
    }
  } else if (hasModule && !isSystem && !isGameExe) {
    summary = en
      ? (L"Top suspect: " + who + L" — the crash appears to occur inside this DLL. (Confidence: High)")
      : (L"유력 후보: " + who + L" — 해당 DLL 내부에서 크래시가 발생한 것으로 보입니다. (신뢰도: 높음)");
  } else if (hasModule && isSystem) {
    if (hasNonHookSuspect && !nonHookSuspectWho.empty()) {
      summary = en
        ? (L"Crash is reported in a Windows system DLL, but " + suspectBasis + L" points to " + nonHookSuspectWho +
            L". (Confidence: " + nonHookSuspectConf + L")")
        : (L"크래시가 Windows 시스템 DLL에서 보고되었지만, " + suspectBasis + L"에서는 " + nonHookSuspectWho +
            L" 가 유력합니다. (신뢰도: " + nonHookSuspectConf + L")");
    } else if (hasSuspect && !suspectWho.empty()) {
      summary = en
        ? (L"Crash is reported in a Windows system DLL, and the top stack candidate is " + suspectWho +
            L". This can still be a victim location rather than the root cause. (Confidence: Low)")
        : (L"크래시가 Windows 시스템 DLL에서 보고되었고, 스택 후보 1순위는 " + suspectWho +
            L" 입니다. 이 경우에도 실제 원인은 다른 DLL/모드일 수 있습니다. (신뢰도: 낮음)");
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
    if (hasNonHookSuspect && !nonHookSuspectWho.empty()) {
      summary = en
        ? (L"Crash is reported in the game executable, but " + suspectBasis + L" points to " + nonHookSuspectWho +
            L". (Confidence: " + nonHookSuspectConf + L")")
        : (L"크래시 위치가 게임 본체(EXE)로 보고되었지만, " + suspectBasis + L"에서는 " + nonHookSuspectWho +
            L" 가 유력합니다. (신뢰도: " + nonHookSuspectConf + L")");
    } else if (topSuspectIsHookFramework && !suspectWho.empty()) {
      summary = en
        ? (L"Crash is reported in the game executable, and the top stack candidate is " + suspectWho +
            L" (known hook framework). This DLL is often a victim frame owner, so avoid treating it as root cause by itself. (Confidence: Low)")
        : (L"크래시 위치가 게임 본체(EXE)로 보고되었고, 스택 후보 1순위는 " + suspectWho +
            L"(알려진 훅 프레임워크)입니다. 이 DLL은 피해 프레임 소유자로 자주 나타나므로 단독 원인으로 단정하기 어렵습니다. (신뢰도: 낮음)");
    } else if (hasSuspect && !suspectWho.empty()) {
      summary = en
        ? (L"Crash is reported in the game executable, and " + suspectBasis + L" points to " + suspectWho +
            L". (Confidence: " + suspectConf + L")")
        : (L"크래시 위치가 게임 본체(EXE)로 보고되었고, " + suspectBasis + L"에서는 " + suspectWho +
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

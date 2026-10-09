#include "EvidenceBuilderEvidencePipeline.h"

#include <algorithm>
#include <cwchar>

#include "SkyrimDiagShared.h"

namespace skydiag::dump_tool::internal {

void BuildHitchAndFreezeEvidence(AnalysisResult& r, i18n::Language lang, const EvidenceBuildContext& ctx)
{
  const bool en = ctx.en;
  const auto hitch = ctx.hitch;
  const bool isHangLike = ctx.isHangLike;

  if (hitch.count > 0) {
    EvidenceItem e{};
    e.confidence_level = i18n::ConfidenceLevel::kMedium;
    e.confidence = ConfidenceText(lang, e.confidence_level);
    e.title = en
      ? L"Stutter / hitch detected"
      : L"끊김/프레임 드랍(히치) 감지";
    e.details = L"count=" + std::to_wstring(hitch.count) +
      L", max=" + std::to_wstring(hitch.maxMs) + L"ms, p95=" + std::to_wstring(hitch.p95Ms) + L"ms";
    if (r.blackbox_freeze_summary.pause_gap_seconds > 0.0) {
      // A gap of minutes is the PC sleeping or the game minimized, not a stutter.
      e.details += en
        ? (L" (max includes a " + DescribeDuration(r.blackbox_freeze_summary.pause_gap_seconds, en) +
            L" pause: PC asleep or the game minimized, most likely)")
        : (L" (max에 " + DescribeDuration(r.blackbox_freeze_summary.pause_gap_seconds, en) +
            L" 정지가 포함됨: PC 절전이나 게임 최소화로 추정)");
    }
    r.evidence.push_back(std::move(e));

    if (!r.resources.empty()) {
      const auto suspects = InferPerfSuspectsFromResourceCorrelation(r.events, r.resources);
      if (!suspects.empty()) {
        EvidenceItem s{};
        s.confidence_level = i18n::ConfidenceLevel::kLow;
        s.confidence = ConfidenceText(lang, s.confidence_level);
        s.title = en
          ? L"Mods providing resources near hitch events (correlation)"
          : L"히치 시점 근처 리소스를 제공한 모드(상관분석)";
        s.details = JoinList(suspects, 5, L", ");
        r.evidence.push_back(std::move(s));
      }
    }

    if (auto anchor = InferCaptureAnchorMs(r)) {
      constexpr double kRecentWindowBeforeMs = 10000.0;
      constexpr double kRecentWindowAfterMs = 300.0;
      const auto recent = ComputeHitchSummaryInRange(
        r.events,
        *anchor - kRecentWindowBeforeMs,
        *anchor + kRecentWindowAfterMs);
      if (recent.count > 0) {
        EvidenceItem rw{};
        rw.confidence_level = i18n::ConfidenceLevel::kMedium;
        rw.confidence = ConfidenceText(lang, rw.confidence_level);
        rw.title = en
          ? L"Recent-window hitch stats (separate from overall)"
          : L"최근 구간 히치 통계(전체 통계와 분리)";
        rw.details = en
          ? (L"window=10s_before_to_0.3s_after_capture, count=" + std::to_wstring(recent.count)
              + L", max=" + std::to_wstring(recent.maxMs) + L"ms, p95=" + std::to_wstring(recent.p95Ms)
              + L"ms (overall max=" + std::to_wstring(hitch.maxMs) + L"ms)")
          : (L"캡처 기준 -10초~+0.3초, count=" + std::to_wstring(recent.count)
              + L", max=" + std::to_wstring(recent.maxMs) + L"ms, p95=" + std::to_wstring(recent.p95Ms)
              + L"ms (전체 max=" + std::to_wstring(hitch.maxMs) + L"ms)");
        r.evidence.push_back(std::move(rw));
      }
    }
  }

  if (isHangLike || (hitch.count > 0 && hitch.maxMs >= 2000)) {
    const auto preFreeze = BuildPreFreezeContextLine(r.events, en);
    if (!preFreeze.empty()) {
      EvidenceItem e{};
      e.confidence_level = i18n::ConfidenceLevel::kMedium;
      e.confidence = ConfidenceText(lang, e.confidence_level);
      e.title = en
        ? L"Context before freeze / big hitch (pre-freeze context)"
        : L"\ud504\ub9ac\uc9d5/\ud070 \ud788\uce58 \uc9c1\uc804 \uc0c1\ud669";
      e.details = preFreeze;
      r.evidence.push_back(std::move(e));
    }
  }

  if (isHangLike || ctx.isManualCapture) {
    if (const auto gameState = DescribeGameStateAtCapture(r.blackbox_freeze_summary, en); !gameState.empty() ||
        r.blackbox_freeze_summary.seconds_since_load_end >= 0.0) {
      EvidenceItem e{};
      e.confidence_level = i18n::ConfidenceLevel::kMedium;
      e.confidence = ConfidenceText(lang, e.confidence_level);
      e.title = en ? L"Game state at the capture" : L"캡처 당시 게임 상태";
      e.details = gameState;
      if (r.blackbox_freeze_summary.seconds_since_load_end >= 0.0) {
        e.details += (e.details.empty() ? L"" : L" ") +
          (en ? (L"The last load finished " + DescribeDuration(r.blackbox_freeze_summary.seconds_since_load_end, en) + L" earlier.")
              : (L"마지막 로딩은 " + DescribeDuration(r.blackbox_freeze_summary.seconds_since_load_end, en) + L" 전에 끝났습니다."));
      }
      r.evidence.push_back(std::move(e));
    }
  }
}

void BuildWctEvidence(AnalysisResult& r, i18n::Language lang, const EvidenceBuildContext& ctx)
{
  const bool en = ctx.en;
  const bool isSnapshotLike = ctx.isSnapshotLike;
  const bool isManualCapture = ctx.isManualCapture;
  const bool wctSuggestsHang = ctx.wctSuggestsHang;
  const auto& wct = ctx.wct;

  if (!r.modal_dialog_wait.detected && !r.main_thread_wait.kind.empty() && r.main_thread_wait.kind != "unknown") {
    // ADR-0009: what the frozen main thread was doing, from its top frames.
    const auto& wait = r.main_thread_wait;
    EvidenceItem e{};
    e.confidence_level = i18n::ConfidenceLevel::kMedium;
    e.confidence = ConfidenceText(lang, e.confidence_level);
    if (wait.kind == "engine_wait") {
      e.title = en ? L"Main thread was waiting inside the game engine" : L"메인 스레드가 게임 엔진 안에서 대기 중";
    } else if (wait.kind == "graphics_driver_wait") {
      e.title = en ? L"Main thread was waiting inside the graphics driver" : L"메인 스레드가 그래픽 드라이버 안에서 대기 중";
    } else if (wait.kind == "plugin_wait") {
      e.title = en ? L"Main thread was waiting in a plugin's call" : L"메인 스레드가 플러그인이 호출한 대기에 머묾";
    } else {
      e.title = en ? L"Main thread was running code" : L"메인 스레드가 코드를 실행 중";
    }
    std::vector<std::wstring> parts;
    if (!wait.wait_api.empty()) {
      parts.push_back((en ? L"wait API: " : L"대기 API: ") + wait.wait_api);
    }
    if (!wait.waiting_module.empty()) {
      parts.push_back((en ? L"module: " : L"모듈: ") + wait.waiting_module);
    }
    if (!wait.path_modules.empty()) {
      parts.push_back((en ? L"plugins further down (call path): " : L"스택 아래쪽 플러그인(호출 경로): ") +
        JoinList(wait.path_modules, wait.path_modules.size(), L", "));
    }
    e.details = JoinList(parts, parts.size(), L" | ");
    r.evidence.push_back(std::move(e));
  }

  if (r.modal_dialog_wait.detected) {
    const auto& modal = r.modal_dialog_wait;
    EvidenceItem e{};
    e.confidence_level = (modal.window_evidence && modal.stack_evidence)
      ? i18n::ConfidenceLevel::kHigh
      : i18n::ConfidenceLevel::kMedium;
    e.confidence = ConfidenceText(lang, e.confidence_level);
    e.title = en
      ? L"Main thread waiting in a modal dialog"
      : L"메인 스레드가 modal 대화상자에서 대기 중";
    std::vector<std::wstring> parts;
    parts.push_back(L"main_tid=" + std::to_wstring(modal.main_thread_id));
    if (modal.window_evidence) {
      parts.push_back((en ? L"dialog window: \"" : L"대화상자 창: \"") + modal.dialog_title + L"\"");
    }
    if (modal.stack_evidence) {
      parts.push_back((en ? L"wait API: " : L"대기 API: ") + modal.wait_api);
    }
    if (!modal.caller_module_filename.empty()) {
      parts.push_back((en ? L"opened by: " : L"호출 모듈: ") + modal.caller_module_filename +
        L" [" + ToWideAscii(modal.caller_kind) + L"]");
    }
    if (!modal.address_library_issue.empty()) {
      parts.push_back(L"Address Library: " + ToWideAscii(modal.address_library_issue));
    }
    if (!modal.dialog_text.empty()) {
      parts.push_back((en ? L"text: " : L"내용: ") + modal.dialog_text);
    }
    e.details = JoinList(parts, parts.size(), L" | ");
    r.evidence.push_back(std::move(e));
  }

  if (r.has_wct) {
    EvidenceItem e{};
    if (isSnapshotLike && isManualCapture && !wctSuggestsHang) {
      e.confidence_level = i18n::ConfidenceLevel::kLow;
      e.confidence = ConfidenceText(lang, e.confidence_level);
      e.title = en
        ? L"WCT snapshot (manual capture)"
        : L"WCT(Wait Chain) 스냅샷(수동 캡처)";
      e.details = en
        ? L"Manual captures always include WCT. This alone does not mean a hang."
        : L"수동 캡처에는 WCT가 항상 포함됩니다. 이것만으로 프리징/무한로딩을 의미하지 않습니다.";
    } else {
      e.confidence_level = i18n::ConfidenceLevel::kMedium;
      e.confidence = ConfidenceText(lang, e.confidence_level);
      e.title = en
        ? L"WCT (Wait Chain) included"
        : L"WCT(Wait Chain) 정보 포함";
      e.details = en
        ? L"For freezes/infinite loading, WCT can show which threads are waiting on what."
        : L"프리징/무한로딩처럼 멈춘 경우, 어떤 스레드가 무엇을 기다리는지 단서를 제공합니다.";
    }
    r.evidence.push_back(std::move(e));
  }

  if (wct) {
    EvidenceItem e{};
    e.confidence_level = i18n::ConfidenceLevel::kMedium;
    e.confidence = ConfidenceText(lang, e.confidence_level);
    e.title = en ? L"WCT summary" : L"WCT 요약";
    wchar_t buf[512]{};
    if (wct->has_capture && wct->thresholdSec > 0u) {
      const std::wstring kindW = ToWideAscii(wct->capture_kind);
      swprintf_s(
        buf,
        L"capture=%s, threads=%d, cycleThreads=%d, heartbeatAge=%.1fs (threshold=%us, loading=%d)",
        kindW.c_str(),
        wct->threads,
        wct->cycles,
        wct->secondsSinceHeartbeat,
        wct->thresholdSec,
        wct->isLoading ? 1 : 0);
    } else {
      swprintf_s(buf, L"threads=%d, cycleThreads=%d", wct->threads, wct->cycles);
    }
    e.details = buf;
    r.evidence.push_back(std::move(e));
  }
}

}  // namespace skydiag::dump_tool::internal

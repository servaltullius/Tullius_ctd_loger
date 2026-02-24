#include "EvidenceBuilderInternalsPriv.h"

#include <algorithm>
#include <cwchar>
#include <filesystem>
#include <vector>

#include "MinidumpUtil.h"
#include "SkyrimDiagShared.h"

namespace skydiag::dump_tool::internal {

void BuildEvidenceItems(AnalysisResult& r, i18n::Language lang, const EvidenceBuildContext& ctx)
{
  const bool en = ctx.en;
  const bool hasException = ctx.hasException;
  const bool isCrashLike = ctx.isCrashLike;
  const bool isHangLike = ctx.isHangLike;
  const bool isSnapshotLike = ctx.isSnapshotLike;
  const bool isManualCapture = ctx.isManualCapture;

  const bool hasModule = ctx.hasModule;
  const bool isSystem = ctx.isSystem;
  const bool isGameExe = ctx.isGameExe;

  const auto& wct = ctx.wct;
  const auto hitch = ctx.hitch;
  const bool wctSuggestsHang = ctx.wctSuggestsHang;

  if (r.signature_match.has_value()) {
    const auto& sig = *r.signature_match;
    EvidenceItem e{};
    e.confidence_level = sig.confidence_level;
    e.confidence = sig.confidence.empty() ? ConfidenceText(lang, sig.confidence_level) : sig.confidence;
    e.title = ctx.en
      ? (L"Known crash pattern: " + ToWideAscii(sig.id))
      : (L"알려진 크래시 패턴: " + ToWideAscii(sig.id));
    e.details = sig.cause;
    r.evidence.push_back(std::move(e));
  }

  if (r.graphics_diag.has_value()) {
    const auto& gd = *r.graphics_diag;
    EvidenceItem e{};
    e.confidence_level = gd.confidence_level;
    e.confidence = gd.confidence.empty() ? ConfidenceText(lang, gd.confidence_level) : gd.confidence;
    e.title = en
      ? (L"Graphics injection crash: " + ToWideAscii(gd.rule_id))
      : (L"그래픽 인젝션 크래시: " + ToWideAscii(gd.rule_id));
    e.details = gd.cause;
    r.evidence.push_back(std::move(e));
  }

  if (!r.plugin_diagnostics.empty()) {
    for (const auto& pd : r.plugin_diagnostics) {
      EvidenceItem e{};
      e.confidence_level = pd.confidence_level;
      e.confidence = pd.confidence.empty() ? ConfidenceText(lang, pd.confidence_level) : pd.confidence;
      e.title = en
        ? (L"Plugin diagnostics: " + ToWideAscii(pd.rule_id))
        : (L"플러그인 진단: " + ToWideAscii(pd.rule_id));
      e.details = pd.cause;
      r.evidence.push_back(std::move(e));
    }
  }

  if (!r.missing_masters.empty()) {
    EvidenceItem e{};
    e.confidence_level = i18n::ConfidenceLevel::kHigh;
    e.confidence = ConfidenceText(lang, e.confidence_level);
    e.title = en
      ? L"Missing plugin masters detected"
      : L"누락된 마스터 플러그인 감지";
    e.details = JoinList(r.missing_masters, 4, L", ");
    r.evidence.push_back(std::move(e));
  }

  if (r.needs_bees) {
    EvidenceItem e{};
    e.confidence_level = i18n::ConfidenceLevel::kHigh;
    e.confidence = ConfidenceText(lang, e.confidence_level);
    e.title = en
      ? L"BEES requirement detected"
      : L"BEES 필요 조건 감지";
    e.details = en
      ? L"Header 1.71 plugin(s) found on pre-1.6.1130 runtime without bees.dll."
      : L"1.71 헤더 플러그인이 있으나 1.6.1130 미만 런타임에서 bees.dll이 로드되지 않았습니다.";
    r.evidence.push_back(std::move(e));
  }

  if (hasException) {
    if (auto info = TryExplainExceptionInfo(r, en)) {
      EvidenceItem e{};
      e.confidence_level = i18n::ConfidenceLevel::kHigh;
      e.confidence = ConfidenceText(lang, e.confidence_level);
      e.title = en ? L"Exception parameter analysis" : L"예외 파라미터 분석";
      e.details = *info;
      r.evidence.push_back(std::move(e));
    }
  }

  if (isSnapshotLike) {
    EvidenceItem e{};
    e.confidence_level = i18n::ConfidenceLevel::kHigh;
    e.confidence = ConfidenceText(lang, e.confidence_level);
    e.title = en
      ? L"This dump looks like a state snapshot (not a crash/hang dump)"
      : L"이 덤프는 크래시 덤프가 아니라 '상태 스냅샷'으로 보임";
    e.details = en
      ? (isManualCapture
          ? L"Likely a manual snapshot. This alone does not prove there is a problem. (For state inspection)"
          : L"Captured without crash/hang signals. Treat it as a snapshot, not a root-cause dump.")
      : (isManualCapture
          ? L"수동 캡처로 추정됩니다. 이 결과만으로 '문제가 있다'고 단정할 수 없습니다. (상태 확인용)"
          : L"크래시/행 신호 없이 캡처된 덤프입니다. 원인 확정용이 아니라 '상태 확인용'입니다.");
    r.evidence.push_back(std::move(e));
  }

  if (!r.crash_logger_log_path.empty()) {
    EvidenceItem e{};
    e.confidence_level = i18n::ConfidenceLevel::kMedium;
    e.confidence = ConfidenceText(lang, e.confidence_level);
    e.title = en
      ? L"Crash Logger SSE/AE log auto-detected"
      : L"Crash Logger SSE/AE 로그를 자동으로 찾음";
    e.details = std::filesystem::path(r.crash_logger_log_path).filename().wstring();
    if (!r.crash_logger_version.empty()) {
      e.details += L" (" + r.crash_logger_version + L")";
    }
    r.evidence.push_back(std::move(e));
  }

  if (!r.crash_logger_top_modules.empty()) {
    EvidenceItem e{};
    e.confidence_level = i18n::ConfidenceLevel::kMedium;
    e.confidence = ConfidenceText(lang, e.confidence_level);
    e.title = en
      ? L"Crash Logger: top callstack modules"
      : L"Crash Logger 콜스택 상위 모듈";
    e.details = JoinList(r.crash_logger_top_modules, 4, L", ");
    r.evidence.push_back(std::move(e));
  }

  if (!r.crash_logger_cpp_exception_type.empty() ||
      !r.crash_logger_cpp_exception_info.empty() ||
      !r.crash_logger_cpp_exception_throw_location.empty() ||
      !r.crash_logger_cpp_exception_module.empty()) {
    std::vector<std::wstring> parts;
    if (!r.crash_logger_cpp_exception_type.empty()) {
      parts.push_back(L"Type: " + r.crash_logger_cpp_exception_type);
    }
    if (!r.crash_logger_cpp_exception_info.empty()) {
      parts.push_back(L"Info: " + r.crash_logger_cpp_exception_info);
    }
    if (!r.crash_logger_cpp_exception_throw_location.empty()) {
      parts.push_back(L"Throw: " + r.crash_logger_cpp_exception_throw_location);
    }
    if (!r.crash_logger_cpp_exception_module.empty()) {
      parts.push_back(L"Module: " + r.crash_logger_cpp_exception_module);
    }

    EvidenceItem e{};
    e.confidence_level = i18n::ConfidenceLevel::kMedium;
    e.confidence = ConfidenceText(lang, e.confidence_level);
    e.title = en
      ? L"Crash Logger: C++ exception details"
      : L"Crash Logger C++ 예외 정보";
    e.details = JoinList(parts, 8, L" | ");
    r.evidence.push_back(std::move(e));
  }

  if (!r.stackwalk_primary_frames.empty()) {
    EvidenceItem e{};
    e.confidence_level = !r.suspects.empty() ? r.suspects[0].confidence_level : i18n::ConfidenceLevel::kLow;
    e.confidence = !r.suspects.empty() ? r.suspects[0].confidence : ConfidenceText(lang, e.confidence_level);
    e.title = en
      ? L"Callstack (primary thread): top frames"
      : L"콜스택(대표 스레드) 상위 프레임";
    e.details = L"tid=" + std::to_wstring(r.stackwalk_primary_tid) + L": " + JoinList(r.stackwalk_primary_frames, 4, L" | ");
    r.evidence.push_back(std::move(e));
  }

  if (!r.suspects.empty()) {
    auto isActionableSuspect = [&](const SuspectItem& s) {
      return !minidump::IsKnownHookFramework(s.module_filename) &&
             !minidump::IsSystemishModule(s.module_filename) &&
             !minidump::IsLikelyWindowsSystemModulePath(s.module_path) &&
             !minidump::IsGameExeModule(s.module_filename);
    };
    const SuspectItem* selectedTop = &r.suspects[0];
    const bool topIsVictimish =
      minidump::IsKnownHookFramework(r.suspects[0].module_filename) ||
      minidump::IsSystemishModule(r.suspects[0].module_filename) ||
      minidump::IsLikelyWindowsSystemModulePath(r.suspects[0].module_path) ||
      minidump::IsGameExeModule(r.suspects[0].module_filename);
    if (topIsVictimish) {
      for (const auto& s : r.suspects) {
        if (isActionableSuspect(s)) {
          selectedTop = &s;
          break;
        }
      }
    }

    EvidenceItem e{};
    e.confidence_level = selectedTop->confidence_level;
    e.confidence = selectedTop->confidence.empty() ? ConfidenceText(lang, i18n::ConfidenceLevel::kMedium) : selectedTop->confidence;
    e.title = en
      ? (r.suspects_from_stackwalk ? L"Top suspect (callstack-based)" : L"Top suspect (stack-scan-based)")
      : (r.suspects_from_stackwalk ? L"콜스택 기반 유력 후보" : L"스택 스캔 기반 유력 후보");

    std::vector<std::wstring> display;
    auto appendDisplay = [&](const SuspectItem& s) {
      std::wstring who;
      if (!s.inferred_mod_name.empty()) {
        who = s.inferred_mod_name + L" (" + s.module_filename + L")";
      } else {
        who = s.module_filename;
      }
      display.push_back(who);
    };

    appendDisplay(*selectedTop);
    for (const auto& s : r.suspects) {
      if (&s == selectedTop) {
        continue;
      }
      appendDisplay(s);
      if (display.size() >= 3) {
        break;
      }
    }
    e.details = JoinList(display, 3, L", ");
    r.evidence.push_back(std::move(e));
  }

  if (!r.resources.empty()) {
    std::vector<std::wstring> recent;
    const std::size_t n = std::min<std::size_t>(r.resources.size(), 4);
    recent.reserve(n);
    for (std::size_t i = r.resources.size() - n; i < r.resources.size(); i++) {
      const auto& rr = r.resources[i];
      std::wstring line = rr.path;
      if (!rr.kind.empty() && rr.kind != L"(unknown)") {
        line = L"[" + rr.kind + L"] " + line;
      }
      if (rr.is_conflict && !rr.providers.empty()) {
        line += L" (providers: " + JoinList(rr.providers, 4, L", ") + L")";
      }
      recent.push_back(std::move(line));
    }

    EvidenceItem e{};
    e.confidence_level = i18n::ConfidenceLevel::kMedium;
    e.confidence = ConfidenceText(lang, e.confidence_level);
    e.title = en
      ? L"Recent resource loads (mesh/anim)"
      : L"최근 로드된 리소스(메쉬/애니) 기록";
    e.details = JoinList(recent, 4, L", ");
    r.evidence.push_back(std::move(e));

    std::vector<std::wstring> conflicts;
    for (const auto& rr : r.resources) {
      if (!rr.is_conflict || rr.providers.size() < 2) {
        continue;
      }
      std::wstring who = rr.path + L" <= " + JoinList(rr.providers, 6, L", ");
      conflicts.push_back(std::move(who));
      if (conflicts.size() >= 4) {
        break;
      }
    }

    if (!conflicts.empty()) {
      EvidenceItem c{};
      c.confidence_level = i18n::ConfidenceLevel::kMedium;
      c.confidence = ConfidenceText(lang, c.confidence_level);
      c.title = en
        ? L"Same file provided by multiple mods (possible conflict)"
        : L"동일 파일을 여러 모드가 제공(충돌 가능)";
      c.details = JoinList(conflicts, 4, L" | ");
      r.evidence.push_back(std::move(c));
    }

    // For crashes/hangs, highlight resources that happened closest to capture time.
    if (isCrashLike || isHangLike) {
      if (auto anchorMs = InferCaptureAnchorMs(r)) {
        const double windowBeforeMs = ((r.state_flags & skydiag::kState_Loading) != 0u) ? 15000.0 : 5000.0;
        const double windowAfterMs = 300.0;
        const auto hits = FindResourcesNearAnchor(r.resources, *anchorMs, windowBeforeMs, windowAfterMs);
        if (!hits.empty()) {
          std::vector<std::wstring> lines;
          lines.reserve(std::min<std::size_t>(hits.size(), 4));
          for (const auto* rr : hits) {
            if (!rr) {
              continue;
            }
            lines.push_back(FormatResourceHitLine(*rr, *anchorMs));
            if (lines.size() >= 4) {
              break;
            }
          }

          if (!lines.empty()) {
            EvidenceItem e{};
            e.confidence_level = i18n::ConfidenceLevel::kLow;
            e.confidence = ConfidenceText(lang, e.confidence_level);
            e.title = en
              ? (isCrashLike
                  ? L"Resources loaded near the crash moment (heuristic)"
                  : L"Resources loaded near the hang moment (heuristic)")
              : (isCrashLike
                  ? L"크래시 직전/직후 로드된 리소스(메쉬/애니) 추정"
                  : L"프리징/무한로딩 시점 근처 로드된 리소스(메쉬/애니) 추정");
            e.details = JoinList(lines, 4, L" | ");
            r.evidence.push_back(std::move(e));
          }

          std::vector<std::wstring> nearConflicts;
          for (const auto* rr : hits) {
            if (!rr || !rr->is_conflict || rr->providers.size() < 2) {
              continue;
            }
            nearConflicts.push_back(FormatResourceHitLine(*rr, *anchorMs));
            if (nearConflicts.size() >= 3) {
              break;
            }
          }
          if (!nearConflicts.empty()) {
            EvidenceItem c{};
            c.confidence_level = i18n::ConfidenceLevel::kMedium;
            c.confidence = ConfidenceText(lang, c.confidence_level);
            c.title = en
              ? L"Near-timestamp resources exist in multiple mods (possible conflict)"
              : L"시점 근처 리소스가 여러 모드에 존재(충돌 가능)";
            c.details = JoinList(nearConflicts, 3, L" | ");
            r.evidence.push_back(std::move(c));
          }

          const auto suspects = InferProviderScoresFromResources(hits);
          if (!suspects.empty()) {
            EvidenceItem s{};
            s.confidence_level = i18n::ConfidenceLevel::kLow;
            s.confidence = ConfidenceText(lang, s.confidence_level);
            s.title = en
              ? L"Mods providing near-timestamp resources (correlation)"
              : L"시점 근처 리소스를 제공한 모드(상관분석)";
            s.details = JoinList(suspects, 5, L", ");
            r.evidence.push_back(std::move(s));
          }
        }
      }
    }
  }

  if (hitch.count > 0) {
    EvidenceItem e{};
    e.confidence_level = i18n::ConfidenceLevel::kMedium;
    e.confidence = ConfidenceText(lang, e.confidence_level);
    e.title = en
      ? L"Stutter / hitch detected"
      : L"끊김/프레임 드랍(히치) 감지";
    e.details = L"count=" + std::to_wstring(hitch.count) +
      L", max=" + std::to_wstring(hitch.maxMs) + L"ms, p95=" + std::to_wstring(hitch.p95Ms) + L"ms";
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

  // Pre-freeze context: summarize events before the biggest hitch
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

  if (hasModule && !isSystem && !isGameExe) {
    EvidenceItem e{};
    e.confidence_level = i18n::ConfidenceLevel::kHigh;
    e.confidence = ConfidenceText(lang, e.confidence_level);
    e.title = en
      ? L"Exception occurred inside a specific DLL"
      : L"크래시가 특정 DLL 내부에서 발생";
    e.details = en
      ? (L"The exception address is within " + r.fault_module_filename + L". (Module+Offset: " + r.fault_module_plus_offset + L")")
      : (L"예외 주소가 " + r.fault_module_filename + L" 범위에 포함됩니다. (Module+Offset: " + r.fault_module_plus_offset + L")");
    r.evidence.push_back(std::move(e));
  } else if (hasModule && isSystem) {
    EvidenceItem e{};
    e.confidence_level = i18n::ConfidenceLevel::kLow;
    e.confidence = ConfidenceText(lang, e.confidence_level);
    e.title = en
      ? L"Exception reported in a Windows system DLL"
      : L"크래시가 Windows 시스템 DLL에서 보고됨";
    e.details = en
      ? (L"The exception address is reported in " + r.fault_module_filename +
          L". In this case the real culprit is often another mod/DLL. (Module+Offset: " + r.fault_module_plus_offset + L")")
      : (L"예외 주소가 " + r.fault_module_filename +
          L" 에서 보고됩니다. 이 경우 실제 원인은 다른 DLL/모드일 수 있습니다. (Module+Offset: " + r.fault_module_plus_offset + L")");
    r.evidence.push_back(std::move(e));
  } else if (!hasModule) {
    EvidenceItem e{};
    e.confidence_level = i18n::ConfidenceLevel::kLow;
    e.confidence = ConfidenceText(lang, e.confidence_level);
    e.title = en
      ? L"Could not determine the fault module"
      : L"fault module을 특정하지 못함";
    e.details = en
      ? L"The dump may lack module list/exception data."
      : L"덤프에 모듈 목록/예외 정보가 부족할 수 있습니다.";
    r.evidence.push_back(std::move(e));
  }

  if (isGameExe && !r.resolved_functions.empty()) {
    const auto it = r.resolved_functions.find(r.fault_module_offset);
    if (it != r.resolved_functions.end()) {
      EvidenceItem e{};
      e.confidence_level = i18n::ConfidenceLevel::kMedium;
      e.confidence = ConfidenceText(lang, e.confidence_level);
      e.title = ctx.en ? L"Game function identified" : L"게임 함수 식별";
      const std::wstring fn = ToWideAscii(it->second);
      e.details = ctx.en
        ? (L"Crash occurred in or near: " + fn)
        : (L"크래시 발생 위치(또는 근처): " + fn);
      r.evidence.push_back(std::move(e));
    }
  }

  if (!r.inferred_mod_name.empty()) {
    EvidenceItem e{};
    e.confidence_level = i18n::ConfidenceLevel::kMedium;
    e.confidence = ConfidenceText(lang, e.confidence_level);
    e.title = en
      ? L"Inferred mod name from MO2 mod path"
      : L"MO2 모드 폴더 경로에서 모드명 추정";
    e.details = en
      ? (L"Detected a \\mods\\<modname>\\ path pattern; inferred '" + r.inferred_mod_name + L"'.")
      : (L"모듈 경로에 \\mods\\<모드명>\\ 패턴이 있어 '" + r.inferred_mod_name + L"' 로 추정했습니다.");
    r.evidence.push_back(std::move(e));
  }

  if ((r.state_flags & skydiag::kState_Loading) != 0u) {
    EvidenceItem e{};
    e.confidence_level = i18n::ConfidenceLevel::kMedium;
    e.confidence = ConfidenceText(lang, e.confidence_level);
    e.title = en
      ? L"Capture appears to have happened during loading"
      : L"크래시 당시 로딩 상태로 추정";
    e.details = en
      ? L"The Loading flag is set in state_flags. (Likely mesh/texture/script init stage)"
      : L"state_flags에 Loading 플래그가 설정되어 있습니다. (메쉬/텍스처/스크립트 초기화 단계일 수 있음)";
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

  if (!r.history_stats.empty()) {
    std::wstring details;
    const std::size_t showN = std::min<std::size_t>(r.history_stats.size(), 3);
    for (std::size_t i = 0; i < showN; ++i) {
      const auto& ms = r.history_stats[i];
      if (ms.module_name.empty()) {
        continue;
      }
      if (!details.empty()) {
        details += L"\n";
      }
      const std::wstring modW = ToWideAscii(ms.module_name);
      if (ctx.en) {
        details += modW + L": " + std::to_wstring(ms.total_appearances) + L"/" +
          std::to_wstring(ms.total_crashes) + L" crashes, top " + std::to_wstring(ms.as_top_suspect) + L"x";
      } else {
        details += modW + L": " + std::to_wstring(ms.total_crashes) + L"회 중 " +
          std::to_wstring(ms.total_appearances) + L"회 등장, 1위 " + std::to_wstring(ms.as_top_suspect) + L"회";
      }
    }
    if (!details.empty()) {
      EvidenceItem e{};
      e.confidence_level = i18n::ConfidenceLevel::kMedium;
      e.confidence = ConfidenceText(lang, e.confidence_level);
      e.title = ctx.en ? L"Crash history pattern" : L"크래시 이력 패턴";
      e.details = details;
      r.evidence.push_back(std::move(e));
    }
  }

  if (r.history_correlation.count > 1) {
    EvidenceItem e{};
    e.confidence_level = i18n::ConfidenceLevel::kHigh;
    e.confidence = ConfidenceText(lang, e.confidence_level);
    e.title = ctx.en ? L"Repeated crash pattern" : L"반복 크래시 패턴";
    wchar_t buf[256]{};
    swprintf_s(buf,
      ctx.en ? L"Same bucket_key matched %zu times (first: %s)"
             : L"동일 패턴이 %zu회 발생 (최초: %s)",
      r.history_correlation.count,
      ToWideAscii(r.history_correlation.first_seen).c_str());
    e.details = buf;
    r.evidence.push_back(std::move(e));
  }
}

}  // namespace skydiag::dump_tool::internal

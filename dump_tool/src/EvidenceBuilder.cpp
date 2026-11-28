#include "EvidenceBuilder.h"

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <string_view>

#include "EvidenceBuilderPrivate.h"
#include "GpuDriverLog.h"
#include "MinidumpUtil.h"
#include "SkyrimDiagShared.h"
#include "Utf.h"
#include "WctTypes.h"

namespace skydiag::dump_tool {

namespace {

// Why the helper left no dump of a second fault (its record's "reason"), as a
// sentence. The identifiers are the helper's (CrashCapture ProcessSecondFault,
// DrainSecondFault).
std::wstring DescribeSecondFaultReason(std::string_view reason, bool en)
{
  struct Text
  {
    std::string_view id;
    const wchar_t* en;
    const wchar_t* ko;
  };
  static constexpr Text kTexts[] = {
    { "no_a_dump_yet", L"its hold ended before this exception's dump was written",
      L"이 예외의 덤프를 쓰기 전에 붙잡아 둘 수 있는 시간이 끝났습니다" },
    { "a_checked_first", L"Tullius was checking whether the game recovers before writing a dump of this exception",
      L"Tullius가 이 예외의 덤프를 쓰기 전에 게임이 회복하는지 확인하던 중이었습니다" },
    { "a_already_captured", L"this exception had already been kept as the crash",
      L"이 예외를 이미 크래시로 확정한 뒤였습니다" },
    { "a_released", L"this exception was being let go as one the game handled",
      L"이 예외를 게임이 처리한 것으로 보고 놓아 주던 중이었습니다" },
    { "a_superseded", L"a newer crash came in and was handled first",
      L"더 새로운 크래시가 들어와 그쪽을 먼저 처리했습니다" },
    { "a_dump_failed", L"this exception's own dump could not be written",
      L"이 예외의 덤프부터 쓰지 못했습니다" },
    { "a_mismatch", L"it was recorded for another exception than this one",
      L"이 예외가 아닌 다른 예외에 대해 적힌 기록이었습니다" },
    { "process_exited", L"the game had already ended", L"게임이 이미 끝난 뒤였습니다" },
    { "hold_expired", L"the plugin's hold time ran out before the dump could start",
      L"덤프를 시작하기 전에 플러그인이 붙잡아 둘 수 있는 시간이 끝났습니다" },
    { "second_fault_dumps_disabled", L"MaxSecondFaultDumps=0 turns these dumps off",
      L"MaxSecondFaultDumps=0으로 이 덤프를 끄도록 설정되어 있습니다" },
    { "snapshot_unavailable", L"the blackbox could not be read consistently",
      L"블랙박스를 일관되게 읽지 못했습니다" },
    { "dump_write_failed", L"writing the dump failed", L"덤프 쓰기에 실패했습니다" },
    { "capture_exception", L"an error occurred while writing it", L"기록하는 중 오류가 났습니다" },
    { "helper_stopped", L"the helper was stopping", L"Helper가 끝나는 중이었습니다" },
  };
  for (const auto& text : kTexts) {
    if (text.id == reason) {
      return en ? text.en : text.ko;
    }
  }
  return en ? L"the reason was not recorded" : L"이유가 기록되지 않았습니다";
}

void AppendSecondFaultEvidence(AnalysisResult& r, i18n::Language lang)
{
  const bool en = lang == i18n::Language::kEnglish;
  if (r.is_second_fault_capture) {
    r.summary_sentence = (en
      ? L"This dump is of an exception that reached the top-level exception filter unhandled while Tullius was checking an earlier exception. "
      : L"이 덤프는 Tullius가 다른 예외를 확인하던 동안 처리되지 않은 채 최상위 예외 필터에 닿은 예외의 것입니다. ") +
      r.summary_sentence;
    if (r.has_blackbox) {
      EvidenceItem item{};
      item.confidence_level = i18n::ConfidenceLevel::kHigh;
      item.confidence = i18n::ConfidenceText(lang, item.confidence_level);
      item.title = en ? L"Blackbox gap before the second fault" : L"두 번째 예외 직전의 블랙박스 공백";
      item.details = en
        ? L"The blackbox did not record ordinary events while the earlier exception held it frozen."
        : L"앞선 예외가 블랙박스를 동결한 동안에는 일반 이벤트가 기록되지 않았습니다.";
      if (!r.second_fault_time_ms) {
        // ParseBlackboxStream found no usable QPC for B.
        item.details += en
          ? L" When this exception happened is not known (its time is missing or before the session start), so the "
            L"times here are measured from the earlier exception's crash marker."
          : L" 이 예외가 난 시각은 알 수 없습니다(시각이 없거나 세션 시작 전). 그래서 여기의 시간은 앞선 예외의 크래시 "
            L"표시를 기준으로 했습니다.";
      }
      r.evidence.push_back(std::move(item));
    }
    return;
  }

  if (!r.incident_second_fault) {
    return;
  }
  const auto& second = *r.incident_second_fault;
  const std::wstring location = second.module_plus_offset.empty()
    ? internal::Hex64(second.address)
    : second.module_plus_offset + (en ? L", from this dump's module list" : L", 이 덤프의 모듈 목록 기준");
  const auto tid = std::to_wstring(second.tid);
  EvidenceItem item{};
  item.confidence_level = i18n::ConfidenceLevel::kHigh;
  item.confidence = i18n::ConfidenceText(lang, item.confidence_level);
  item.title = en ? L"Second exception reached the top-level filter" : L"최상위 예외 필터에 닿은 두 번째 예외";
  item.details = en
    ? L"While Tullius was checking whether the game handled this exception, another exception (" + location +
        L", thread " + tid + L") reached the top-level exception filter unhandled."
    : L"Tullius가 이 예외를 게임이 처리했는지 확인하던 동안 다른 예외(" + location +
        L", 스레드 " + tid + L")가 처리되지 않은 채 최상위 예외 필터에 닿았습니다.";
  if (!second.dump_filename.empty() && second.dump_missing) {
    const auto filename = Utf8ToWide(second.dump_filename);
    const std::wstring gone = en
      ? L"A separate dump of that exception was saved as " + filename +
          L", but it is not in this folder any more (retention may have deleted it)."
      : L"그 예외의 덤프를 " + filename + L"에 따로 남겼지만 지금은 이 폴더에 없습니다(보존 정리로 지워졌을 수 있음).";
    item.details += L" " + gone;
    r.recommendations.push_back(gone);
    r.second_fault_note_last = true;
  } else if (!second.dump_filename.empty()) {
    const auto filename = Utf8ToWide(second.dump_filename);
    item.details += en ? L" A separate dump was saved as " + filename + L"."
                       : L" 그 예외의 덤프를 " + filename + L"에 따로 남겼습니다.";
    // As the lead, the first recommendation already points to that dump
    // (BuildRecommendations).
    if (!r.second_fault_lead) {
      r.recommendations.push_back(en
        ? L"That exception may have ended the game. Open " + filename + L" in the viewer to examine it."
        : L"게임을 끝낸 것은 그 예외일 수 있으니 뷰어에서 " + filename + L"도 열어 보세요.");
      r.second_fault_note_last = true;
    }
  } else {
    const std::wstring reason = DescribeSecondFaultReason(second.reason, en);
    const std::wstring noDump = en ? L"No dump was saved for that exception: " + reason + L"."
                                   : L"그 예외의 덤프는 남기지 못했습니다. " + reason + L".";
    item.details += L" " + noDump;
    r.recommendations.push_back(noDump);
    r.second_fault_note_last = true;
  }
  if (second.resumed) {
    item.details += en ? L" Execution resumed after that exception." : L" 그 예외는 그 뒤 실행이 이어졌습니다.";
  }
  if (second.crash_logger_log_is_second) {
    const auto logFile = std::filesystem::path(r.crash_logger_log_path).filename().wstring();
    item.details += en ? L" The Crash Logger log (" + logFile + L") is of that exception."
                       : L" Crash Logger 로그(" + logFile + L")는 그 예외의 것입니다.";
  }
  if (r.second_fault_lead) {
    r.evidence.insert(r.evidence.begin(), std::move(item));
  } else {
    r.evidence.push_back(std::move(item));
  }
}

void BuildEvidenceAndSummaryImpl(AnalysisResult& r, i18n::Language lang)
{
  r.language = lang;
  const bool en = (lang == i18n::Language::kEnglish);

  r.evidence.clear();
  r.recommendations.clear();
  r.second_fault_lead = false;
  r.second_fault_note_last = false;

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
  const bool isCrashLike = r.is_second_fault_capture ||
    (!r.is_filtered_clean_exit &&
     (nameCrash || (hasException && !likelyHandledExceptionFalsePositive) ||
      (hasCrashEvent && !manualCaptureHint && !handledCppException)));
  const bool nameHangEffective = nameHang && !manualFromWct && !heartbeatSuggestsNotHang;
  // ADR-0035: a slow shutdown was judged a freeze by the analyzer
  // (DetectSlowShutdown), so the result's flags say so too.
  const bool isHangLike =
    !r.is_second_fault_capture && !r.is_filtered_clean_exit &&
    (nameHangEffective || hasHangEvent || wctSuggestsHang || r.quit_before_freeze.detected);
  const bool isSnapshotLike = !isCrashLike && !isHangLike;
  const bool isManualCapture =
    !r.is_second_fault_capture && !r.is_filtered_clean_exit &&
    (manualCaptureHint || (nameHang && isSnapshotLike));

  r.is_crash_like = isCrashLike;
  r.is_hang_like = isHangLike;
  r.is_snapshot_like = isSnapshotLike;
  r.is_manual_capture = isManualCapture;
  // Use the finalized capture kind after Analyzer.cpp checked the location mismatch.
  r.crash_logger_later_crash = internal::CrashLoggerShowsLaterCrash(r, nameHang);

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
  // Detected only for a freeze with no exception (Analyzer.cpp), so never with a forced close.
  ctx.isSlowShutdown = r.quit_before_freeze.detected && !ctx.isForcedClose;
  ctx.isCrashLike = isCrashLike;
  ctx.isHangLike = isHangLike;
  ctx.isSnapshotLike = isSnapshotLike;
  ctx.isManualCapture = isManualCapture;
  // A known cause with its own fix or a hook race leads the quick answer and
  // the next action; the summary sentence must not lead with the save then.
  const bool strongerLead =
    (r.signature_match.has_value() && r.signature_match->scope == "root_cause" &&
     !r.signature_match->recommendations.empty()) ||
    r.hook_stub_race.detected;
  ctx.isDamagedSaveSuspected = isCrashLike && !ctx.isForcedClose && !ctx.isFilteredCleanExit && !strongerLead &&
                               internal::SaveLoadSuggestsDamagedSave(r.save_load);
  if (isCrashLike && !ctx.isForcedClose && !ctx.isFilteredCleanExit) {
    if (const auto last = LastGpuDriverProblemBefore(r.gpu_driver_log, r.dump_capture_unix);
        last && last->seconds_before_capture <= kGpuDriverErrorNearCrashSec) {
      ctx.gpuDriverErrorSecBeforeCrash = last->seconds_before_capture;
      ctx.gpuDriverErrorName = last->name;
    }
  }
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
  ctx.weakFaultLocationSuspect = internal::IsWeakFaultLocationOnlySuspect(r, ctx);
  ctx.weakFaultLocationTopCandidate = !r.actionable_candidates.empty() &&
    internal::IsWeakFaultLocationActionableCandidate(r, r.actionable_candidates[0], ctx);
  ctx.weakFaultLocationTopCandidateWithStack = ctx.weakFaultLocationTopCandidate &&
    internal::CandidateHasFamily(r.actionable_candidates[0], "actionable_stack");
  if (ctx.weakFaultLocationSuspect && !r.suspects.empty()) {
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
  r.second_fault_lead = internal::SecondFaultEndedGame(r, ctx, nameHang);
  internal::BuildEvidenceItems(r, lang, ctx);
  r.quick_answer = internal::BuildQuickAnswer(r, lang, ctx);
  if (r.is_second_fault_capture) {
    internal::MarkSecondFaultDumpAnswer(r.quick_answer, en);
  }
  internal::BuildRecommendations(r, lang, ctx);
  r.summary_sentence = internal::BuildSummarySentence(r, lang, ctx);
  if (r.earlier_exception.detected && ctx.isCrashLike && !ctx.isFilteredCleanExit) {
    // ADR-0039: what happened first on the crashing thread, before what the
    // reported exception alone says.
    r.summary_sentence = (en ? L"[Crash while handling an exception] " : L"[예외 처리 중 크래시] ") +
      earlier_exception::DescribeTechnical(r.earlier_exception, r.exc_code, en) + L" " + r.summary_sentence;
  }
  if (ctx.isForcedClose) {
    // The detail stays for mod authors; the player reads first that this is
    // no crash in play.
    r.summary_sentence = internal::ForcedCloseLead(r.forced_close, en) + L" " + r.summary_sentence;
  } else if (ctx.isSlowShutdown) {
    // ADR-0035: the freeze analysis stays below; the player reads first that
    // the game was quitting.
    r.summary_sentence = internal::SlowShutdownLead(r.quit_before_freeze, en) + L" " + r.summary_sentence;
  } else if (ctx.isDamagedSaveSuspected) {
    // ADR-0025: what the repeats say comes before what this one dump says.
    r.summary_sentence = internal::DamagedSaveLead(r.save_load, en) + L" " + r.summary_sentence;
  }
  if (r.second_fault_lead || r.crash_logger_later_crash) {
    r.summary_sentence = (en ? L"[Different crash from dump] " : L"[덤프와 다른 크래시] ") +
      (r.second_fault_lead ? internal::SecondFaultLeadDetails(r, en) : internal::CrashLoggerLaterCrashDetails(r, en)) +
      (en ? L" The analysis below is of the earlier exception in this dump. "
          : L" 아래는 이 덤프에 찍힌 앞선 예외의 분석입니다. ") +
      r.summary_sentence;
  }
  AppendSecondFaultEvidence(r, lang);
}

}  // namespace

void BuildEvidenceAndSummary(AnalysisResult& r, i18n::Language lang)
{
  BuildEvidenceAndSummaryImpl(r, lang);
}

}  // namespace skydiag::dump_tool

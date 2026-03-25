#include "OutputWriterPipeline.h"

#include "OutputWriterInternals.h"
#include "Utf.h"
#include "WctTypes.h"

#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace skydiag::dump_tool {

using skydiag::dump_tool::internal::output_writer::JoinList;
using skydiag::dump_tool::internal::output_writer::MaybeRedactPath;

std::string BuildReportText(
  const AnalysisResult& r,
  const nlohmann::json& summary,
  bool redactPaths)
{
  const bool en = (r.language == i18n::Language::kEnglish);
  const auto stripRecommendationTag = [](const std::wstring& recommendation) -> std::wstring {
    if (recommendation.empty() || recommendation.front() != L'[') {
      return recommendation;
    }

    const auto end = recommendation.find(L']');
    if (end == std::wstring::npos || end + 1 >= recommendation.size()) {
      return recommendation;
    }

    std::size_t start = end + 1;
    while (start < recommendation.size() && iswspace(recommendation[start])) {
      ++start;
    }
    return recommendation.substr(start);
  };
  const auto buildCrashLoggerReadingPath = [&]() -> std::wstring {
    if (!r.crash_logger_direct_fault_module.empty()) {
      return en
        ? (L"Crash Logger frame first (direct DLL fault): " + r.crash_logger_direct_fault_module)
        : (L"Crash Logger frame 우선 (direct DLL fault): " + r.crash_logger_direct_fault_module);
    }
    if (!r.crash_logger_first_actionable_probable_module.empty()) {
      return en
        ? (L"Crash Logger frame first (first actionable probable DLL frame): " + r.crash_logger_first_actionable_probable_module)
        : (L"Crash Logger frame 우선 (첫 actionable probable DLL frame): " + r.crash_logger_first_actionable_probable_module);
    }
    if (!r.crash_logger_probable_streak_module.empty() && r.crash_logger_probable_streak_length > 0u) {
      return en
        ? (L"Crash Logger frame first (probable frame streak x" + std::to_wstring(r.crash_logger_probable_streak_length) + L"): " +
           r.crash_logger_probable_streak_module)
        : (L"Crash Logger frame 우선 (probable frame streak x" + std::to_wstring(r.crash_logger_probable_streak_length) + L"): " +
           r.crash_logger_probable_streak_module);
    }
    if (!r.crash_logger_object_refs.empty()) {
      const auto& topRef = r.crash_logger_object_refs.front();
      if (!topRef.esp_name.empty()) {
        return en
          ? (L"Crash Logger object ref: " + topRef.esp_name)
          : (L"Crash Logger 오브젝트 참조: " + topRef.esp_name);
      }
    }
    return {};
  };

  std::ostringstream rpt;
  rpt << (en ? "SkyrimDiag Report\n" : "SkyrimDiag 리포트\n");
  rpt << (en ? "Dump: " : "덤프: ") << WideToUtf8(MaybeRedactPath(r.dump_path, redactPaths)) << "\n";
  rpt << (en ? "Summary: " : "결론: ") << WideToUtf8(r.summary_sentence) << "\n";
  rpt << (en ? "PathRedactionApplied: " : "경로 마스킹 적용: ") << (redactPaths ? "1" : "0") << "\n";
  rpt << (en ? "OnlineSymbolSourceAllowed: " : "온라인 심볼 소스 허용: ") << (r.online_symbol_source_allowed ? "1" : "0") << "\n";
  rpt << (en ? "OnlineSymbolSourceUsed: " : "온라인 심볼 소스 사용: ") << (r.online_symbol_source_used ? "1" : "0") << "\n";
  rpt << (en ? "DbgHelpPath: " : "DbgHelp 경로: ") << WideToUtf8(MaybeRedactPath(r.dbghelp_path, redactPaths)) << "\n";
  rpt << (en ? "DbgHelpVersion: " : "DbgHelp 버전: ") << WideToUtf8(r.dbghelp_version) << "\n";
  rpt << (en ? "DIAAvailable: " : "DIA 사용 가능: ") << (r.msdia_available ? "1" : "0") << "\n";
  rpt << (en ? "DIAPath: " : "DIA 경로: ") << WideToUtf8(MaybeRedactPath(r.msdia_path, redactPaths)) << "\n";
  rpt << (en ? "SymbolCacheReady: " : "심볼 캐시 준비: ") << (r.symbol_cache_ready ? "1" : "0") << "\n";
  rpt << (en ? "SymbolRuntimeDegraded: " : "심볼 런타임 저하: ") << (r.symbol_runtime_degraded ? "1" : "0") << "\n";
  if (summary.contains("incident") && summary["incident"].is_object()) {
    const auto& inc = summary["incident"];
    if (inc.contains("incident_id") && inc["incident_id"].is_string()) {
      rpt << (en ? "IncidentId: " : "IncidentId: ") << inc["incident_id"].get<std::string>() << "\n";
    }
    if (inc.contains("capture_kind") && inc["capture_kind"].is_string()) {
      rpt << (en ? "CaptureKind: " : "CaptureKind: ") << inc["capture_kind"].get<std::string>() << "\n";
    }
    if (inc.contains("capture_profile") && inc["capture_profile"].is_object()) {
      const auto& captureProfile = inc["capture_profile"];
      if (captureProfile.contains("base_mode") && captureProfile["base_mode"].is_string()) {
        rpt << (en ? "CaptureProfileBaseMode: " : "CaptureProfileBaseMode: ")
            << captureProfile["base_mode"].get<std::string>() << "\n";
      }
      if (captureProfile.contains("include_code_segments") && captureProfile["include_code_segments"].is_boolean()) {
        rpt << (en ? "CaptureProfileCodeSegments: " : "CaptureProfileCodeSegments: ")
            << (captureProfile["include_code_segments"].get<bool>() ? "1" : "0") << "\n";
      }
      if (captureProfile.contains("include_process_thread_data") &&
          captureProfile["include_process_thread_data"].is_boolean()) {
        rpt << (en ? "CaptureProfileProcessThreadData: " : "CaptureProfileProcessThreadData: ")
            << (captureProfile["include_process_thread_data"].get<bool>() ? "1" : "0") << "\n";
      }
      if (captureProfile.contains("include_full_memory_info") &&
          captureProfile["include_full_memory_info"].is_boolean()) {
        rpt << (en ? "CaptureProfileFullMemoryInfo: " : "CaptureProfileFullMemoryInfo: ")
            << (captureProfile["include_full_memory_info"].get<bool>() ? "1" : "0") << "\n";
      }
      if (captureProfile.contains("include_module_headers") && captureProfile["include_module_headers"].is_boolean()) {
        rpt << (en ? "CaptureProfileModuleHeaders: " : "CaptureProfileModuleHeaders: ")
            << (captureProfile["include_module_headers"].get<bool>() ? "1" : "0") << "\n";
      }
      if (captureProfile.contains("include_indirect_memory") && captureProfile["include_indirect_memory"].is_boolean()) {
        rpt << (en ? "CaptureProfileIndirectMemory: " : "CaptureProfileIndirectMemory: ")
            << (captureProfile["include_indirect_memory"].get<bool>() ? "1" : "0") << "\n";
      }
      if (captureProfile.contains("ignore_inaccessible_memory") &&
          captureProfile["ignore_inaccessible_memory"].is_boolean()) {
        rpt << (en ? "CaptureProfileIgnoreInaccessibleMemory: " : "CaptureProfileIgnoreInaccessibleMemory: ")
            << (captureProfile["ignore_inaccessible_memory"].get<bool>() ? "1" : "0") << "\n";
      }
      if (captureProfile.contains("include_full_memory") && captureProfile["include_full_memory"].is_boolean()) {
        rpt << (en ? "CaptureProfileFullMemory: " : "CaptureProfileFullMemory: ")
            << (captureProfile["include_full_memory"].get<bool>() ? "1" : "0") << "\n";
      }
    }
    if (inc.contains("recapture_evaluation") && inc["recapture_evaluation"].is_object()) {
      const auto& recapture = inc["recapture_evaluation"];
      if (recapture.contains("triggered") && recapture["triggered"].is_boolean()) {
        rpt << (en ? "RecaptureTriggered: " : "RecaptureTriggered: ")
            << (recapture["triggered"].get<bool>() ? "1" : "0") << "\n";
      }
      if (recapture.contains("target_profile") && recapture["target_profile"].is_string()) {
        rpt << (en ? "RecaptureTargetProfile: " : "RecaptureTargetProfile: ")
            << recapture["target_profile"].get<std::string>() << "\n";
      }
      if (recapture.contains("reasons") && recapture["reasons"].is_array()) {
        std::vector<std::wstring> reasons;
        for (const auto& reason : recapture["reasons"]) {
          if (reason.is_string()) {
            reasons.push_back(Utf8ToWide(reason.get<std::string>()));
          }
        }
        rpt << (en ? "RecaptureReasons: " : "RecaptureReasons: ")
            << WideToUtf8(JoinList(reasons, reasons.size(), L", ")) << "\n";
      }
      if (recapture.contains("escalation_level") && recapture["escalation_level"].is_number_unsigned()) {
        rpt << (en ? "RecaptureEscalationLevel: " : "RecaptureEscalationLevel: ")
            << recapture["escalation_level"].get<std::uint32_t>() << "\n";
      }
    }
    if (inc.contains("manifest_path") && inc["manifest_path"].is_string()) {
      rpt << (en ? "IncidentManifest: " : "Incident manifest: ") << inc["manifest_path"].get<std::string>() << "\n";
    }
    if (inc.contains("artifacts") && inc["artifacts"].is_object()) {
      const auto& art = inc["artifacts"];
      if (art.contains("etw") && art["etw"].is_string() && !art["etw"].get<std::string>().empty()) {
        rpt << (en ? "ETW: " : "ETW: ") << art["etw"].get<std::string>() << "\n";
      }
    }
  }
  if (!r.crash_bucket_key.empty()) {
    rpt << (en ? "CrashBucketKey: " : "크래시 버킷 키: ") << WideToUtf8(r.crash_bucket_key) << "\n";
  }
  if (r.has_plugin_scan) {
    rpt << (en ? "PluginScan: 1" : "플러그인 스캔: 1") << "\n";
  }
  if (!r.missing_masters.empty()) {
    rpt << (en ? "MissingMasters: " : "누락 마스터: ")
        << WideToUtf8(JoinList(r.missing_masters, 8, L", ")) << "\n";
  }
  if (r.needs_bees && r.is_crash_like) {
    rpt << (en ? "NeedsBEES: 1" : "BEES 필요: 1") << "\n";
  }
  if (!r.plugin_diagnostics.empty()) {
    std::vector<std::wstring> diagIds;
    diagIds.reserve(r.plugin_diagnostics.size());
    for (const auto& d : r.plugin_diagnostics) {
      diagIds.push_back(Utf8ToWide(d.rule_id));
    }
    rpt << (en ? "PluginRules: " : "플러그인 규칙: ")
        << WideToUtf8(JoinList(diagIds, 8, L", ")) << "\n";
  }
  rpt << "\n";
  rpt << (en ? "ExceptionCode: 0x" : "ExceptionCode: 0x") << std::hex << r.exc_code << std::dec << "\n";
  rpt << (en ? "ExceptionAddress: 0x" : "ExceptionAddress: 0x") << std::hex << r.exc_addr << std::dec << "\n";
  rpt << (en ? "ThreadId: " : "ThreadId: ") << r.exc_tid << "\n";
  rpt << (en ? "Module+Offset: " : "Module+Offset: ") << WideToUtf8(r.fault_module_plus_offset) << "\n";
  if (!r.inferred_mod_name.empty()) {
    rpt << (en ? "InferredMod: " : "추정 모드: ") << WideToUtf8(r.inferred_mod_name) << "\n";
  }
  if (!r.crash_logger_log_path.empty()) {
    rpt << (en ? "CrashLoggerLog: " : "Crash Logger 로그: ") << WideToUtf8(MaybeRedactPath(r.crash_logger_log_path, redactPaths)) << "\n";
  }
  if (!r.crash_logger_version.empty()) {
    rpt << (en ? "CrashLoggerVersion: " : "Crash Logger 버전: ") << WideToUtf8(r.crash_logger_version) << "\n";
  }
  if (!r.crash_logger_top_modules.empty()) {
    rpt << (en ? "CrashLoggerTopModules: " : "Crash Logger 상위 모듈: ")
        << WideToUtf8(JoinList(r.crash_logger_top_modules, 6, L", ")) << "\n";
  }
  if (!r.crash_logger_direct_fault_module.empty()) {
    rpt << (en ? "CrashLoggerDirectFaultModule: " : "Crash Logger direct fault 모듈: ")
        << WideToUtf8(r.crash_logger_direct_fault_module) << "\n";
  }
  if (!r.crash_logger_first_actionable_probable_module.empty()) {
    rpt << (en ? "CrashLoggerFirstActionableProbableModule: " : "Crash Logger 첫 actionable probable 모듈: ")
        << WideToUtf8(r.crash_logger_first_actionable_probable_module) << "\n";
  }
  if (!r.crash_logger_probable_streak_module.empty()) {
    rpt << (en ? "CrashLoggerProbableStreakModule: " : "Crash Logger probable streak 모듈: ")
        << WideToUtf8(r.crash_logger_probable_streak_module)
        << " x" << r.crash_logger_probable_streak_length << "\n";
  }
  if (r.crash_logger_frame_signal_strength > 0u) {
    rpt << (en ? "CrashLoggerFrameSignalStrength: " : "Crash Logger frame 신호 강도: ")
        << r.crash_logger_frame_signal_strength << "\n";
  }
  if (!r.crash_logger_cpp_exception_type.empty() ||
      !r.crash_logger_cpp_exception_info.empty() ||
      !r.crash_logger_cpp_exception_throw_location.empty() ||
      !r.crash_logger_cpp_exception_module.empty()) {
    rpt << (en ? "CrashLoggerCppExceptionType: " : "Crash Logger C++ 예외 Type: ") << WideToUtf8(r.crash_logger_cpp_exception_type) << "\n";
    rpt << (en ? "CrashLoggerCppExceptionInfo: " : "Crash Logger C++ 예외 Info: ") << WideToUtf8(r.crash_logger_cpp_exception_info) << "\n";
    rpt << (en ? "CrashLoggerCppExceptionThrowLocation: " : "Crash Logger C++ 예외 Throw Location: ") << WideToUtf8(r.crash_logger_cpp_exception_throw_location) << "\n";
    rpt << (en ? "CrashLoggerCppExceptionModule: " : "Crash Logger C++ 예외 Module: ") << WideToUtf8(r.crash_logger_cpp_exception_module) << "\n";
  }
  if (!r.crash_logger_object_refs.empty()) {
    rpt << (en ? "CrashLoggerObjectRefs: " : "Crash Logger 오브젝트 참조: ");
    bool first = true;
    for (const auto& ref : r.crash_logger_object_refs) {
      if (!first) rpt << ", ";
      rpt << WideToUtf8(ref.esp_name);
      if (!ref.form_id.empty()) {
        rpt << " [" << WideToUtf8(ref.form_id) << "]";
      }
      if (!ref.best_object_type.empty()) {
        rpt << " (" << WideToUtf8(ref.best_object_type) << ")";
      }
      first = false;
    }
    rpt << "\n";
  }
  const auto crashLoggerReadingPath = buildCrashLoggerReadingPath();
  if (!crashLoggerReadingPath.empty()) {
    rpt << (en ? "CrashLoggerReadingPath: " : "CrashLoggerReadingPath: ")
        << WideToUtf8(crashLoggerReadingPath) << "\n";
  }
  if (!r.recommendations.empty()) {
    rpt << (en ? "NextAction: " : "NextAction: ")
        << WideToUtf8(stripRecommendationTag(r.recommendations.front())) << "\n";
  }
  rpt << (en ? "StateFlags: " : "StateFlags: ") << r.state_flags << "\n";
  rpt << (en ? "HasBlackbox: " : "HasBlackbox: ") << (r.has_blackbox ? "1" : "0") << "\n";
  rpt << (en ? "HasWCT: " : "HasWCT: ") << (r.has_wct ? "1" : "0") << "\n";
  rpt << (en ? "Suspects: " : "후보 개수: ") << r.suspects.size() << "\n";
  rpt << (en ? "SuspectsFromStackwalk: " : "콜스택 기반 후보: ") << (r.suspects_from_stackwalk ? "1" : "0") << "\n";
  if (r.freeze_analysis.has_analysis) {
    const auto freezeWct = internal::TryParseWctFreezeSummary(r.wct_json_utf8);
    rpt << (en ? "FreezeAnalysis: " : "FreezeAnalysis: ")
        << r.freeze_analysis.state_id
        << " confidence=" << WideToUtf8(r.freeze_analysis.confidence)
        << " support_quality=" << r.freeze_analysis.support_quality
        << "\n";
    rpt << "  wct capture_passes=" << (freezeWct ? freezeWct->capture_passes : 0u)
        << " cycle_consensus=" << ((freezeWct && freezeWct->cycle_consensus) ? "1" : "0")
        << " repeated_cycle_thread_count=" << (freezeWct ? freezeWct->repeated_cycle_tids.size() : 0u)
        << " consistent_loading_signal=" << ((freezeWct && freezeWct->consistent_loading_signal) ? "1" : "0")
        << " longest_wait_tid_consensus=" << ((freezeWct && freezeWct->longest_wait_tid_consensus) ? "1" : "0")
        << "\n";
    rpt << "  blackbox loading_window="
        << (r.freeze_analysis.blackbox_context.loading_window ? "1" : "0")
        << " module churn=" << r.freeze_analysis.blackbox_context.module_churn_score
        << " thread churn=" << r.freeze_analysis.blackbox_context.thread_churn_score
        << "\n";
    if (!r.freeze_analysis.blackbox_context.recent_non_system_modules.empty()) {
      rpt << "  blackbox recent_non_system_modules="
          << WideToUtf8(JoinList(r.freeze_analysis.blackbox_context.recent_non_system_modules, 4, L", "))
          << "\n";
    }
    rpt << "  first_chance recent=" << r.freeze_analysis.first_chance_context.recent_count
        << " loading_window=" << r.freeze_analysis.first_chance_context.loading_window_count
        << " repeated=" << r.freeze_analysis.first_chance_context.repeated_signature_count
        << "\n";
    if (r.freeze_analysis.first_chance_context.repeated_signature_count > 0u) {
      rpt << "  repeated suspicious first-chance exceptions were observed before capture\n";
    }
    if (!r.freeze_analysis.first_chance_context.recent_non_system_modules.empty()) {
      rpt << "  first_chance recent_non_system_modules="
          << WideToUtf8(JoinList(r.freeze_analysis.first_chance_context.recent_non_system_modules, 4, L", "))
          << "\n";
    }
    for (const auto& reason : r.freeze_analysis.primary_reasons) {
      rpt << "  - " << WideToUtf8(reason) << "\n";
    }
    for (const auto& candidate : r.freeze_analysis.related_candidates) {
      rpt << "  * " << WideToUtf8(candidate.display_name)
          << " [" << WideToUtf8(candidate.confidence) << "]\n";
    }
  }
  if (!r.actionable_candidates.empty()) {
    rpt << (en ? "\nActionable candidates:\n" : "\n행동 우선 후보:\n");
    for (const auto& candidate : r.actionable_candidates) {
      rpt << "  * " << WideToUtf8(candidate.primary_identifier.empty() ? candidate.display_name : candidate.primary_identifier);
      if (!candidate.secondary_label.empty()) {
        rpt << " (" << WideToUtf8(candidate.secondary_label) << ")";
      }
      rpt << " [" << WideToUtf8(candidate.confidence) << "]\n";
    }
  }
  rpt << (en ? "\nEvidence:\n" : "\n근거:\n");
  for (const auto& e : r.evidence) {
    rpt << "- [" << WideToUtf8(e.confidence) << "] " << WideToUtf8(e.title) << "\n";
    rpt << "  " << WideToUtf8(e.details) << "\n";
  }
  if (!r.stackwalk_primary_frames.empty()) {
    rpt << (en ? "\nCallstack (primary, tid=" : "\n콜스택(대표, tid=") << r.stackwalk_primary_tid << "):\n";
    for (const auto& f : r.stackwalk_primary_frames) {
      rpt << "  " << WideToUtf8(f) << "\n";
    }
  }
  if (!r.suspects.empty()) {
    rpt << (en ? "\nSuspects (" : "\n후보(") << (r.suspects_from_stackwalk ? "callstack" : "stack scan") << "):\n";
    for (const auto& s : r.suspects) {
      rpt << "- [" << WideToUtf8(s.confidence) << "] " << WideToUtf8(s.module_filename);
      if (!s.inferred_mod_name.empty()) {
        rpt << " (" << WideToUtf8(s.inferred_mod_name) << ")";
      }
      rpt << " score=" << s.score << "\n";
      rpt << "  " << WideToUtf8(s.reason) << "\n";
      if (!s.module_path.empty()) {
        rpt << "  path=" << WideToUtf8(MaybeRedactPath(s.module_path, redactPaths)) << "\n";
      }
    }
  }
  if (!r.resources.empty()) {
    rpt << (en ? "\nRecent resources (.nif/.hkx/.tri):\n" : "\n최근 리소스(.nif/.hkx/.tri):\n");
    for (const auto& rr : r.resources) {
      rpt << "- t_ms=" << rr.t_ms << " tid=" << rr.tid << " [" << WideToUtf8(rr.kind) << "] "
          << WideToUtf8(MaybeRedactPath(rr.path, redactPaths));
      if (!rr.providers.empty()) {
        rpt << " providers=" << WideToUtf8(JoinList(rr.providers, 10, L", "));
      }
      if (rr.is_conflict) {
        rpt << " (conflict)";
      }
      rpt << "\n";
    }
  }
  rpt << (en ? "\nRecommendations (checklist):\n" : "\n권장 조치(체크리스트):\n");
  for (const auto& s : r.recommendations) {
    rpt << "- " << WideToUtf8(s) << "\n";
  }
  rpt << (en ? "\nLast events (most recent last):\n" : "\n최근 이벤트(최신이 마지막):\n");
  for (const auto& ev : r.events) {
    rpt << "[" << ev.i << "] t_ms=" << ev.t_ms << " tid=" << ev.tid << " " << WideToUtf8(ev.type_name);
    if (!ev.detail.empty()) {
      rpt << " | " << WideToUtf8(ev.detail);
    }
    rpt << " a=" << ev.a << " b=" << ev.b << " c=" << ev.c << " d=" << ev.d << "\n";
  }

  if (!r.diagnostics.empty()) {
    rpt << (en ? "\nDiagnostics:\n" : "\n진단 로그:\n");
    for (const auto& d : r.diagnostics) {
      rpt << "  " << WideToUtf8(d) << "\n";
    }
  }

  return rpt.str();
}

}  // namespace skydiag::dump_tool

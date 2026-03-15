#include "EvidenceBuilderEvidencePipeline.h"

#include <algorithm>
#include <filesystem>
#include <vector>

#include "MinidumpUtil.h"
#include "SkyrimDiagShared.h"

namespace skydiag::dump_tool::internal {

void BuildCrashLoggerEvidence(AnalysisResult& r, i18n::Language lang, const EvidenceBuildContext& ctx)
{
  const bool en = ctx.en;

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

  if (!r.crash_logger_object_refs.empty()) {
    EvidenceItem e{};
    e.confidence_level = (r.crash_logger_object_refs[0].relevance_score >= 14)
      ? i18n::ConfidenceLevel::kMedium
      : i18n::ConfidenceLevel::kLow;
    e.confidence = ConfidenceText(lang, e.confidence_level);
    e.title = en
      ? L"Crash Logger: referenced game plugins (ESP/ESM)"
      : L"Crash Logger: 참조된 게임 플러그인(ESP/ESM)";

    std::wstring detail;
    std::size_t shown = 0;
    for (const auto& ref : r.crash_logger_object_refs) {
      if (shown >= 4) break;
      if (!detail.empty()) detail += L", ";
      detail += ref.esp_name;
      if (!ref.form_id.empty()) {
        detail += L" [" + ref.form_id + L"]";
      }
      if (!ref.best_object_type.empty()) {
        detail += L" (" + ref.best_object_type + L")";
      }
      if (!ref.object_name.empty()) {
        detail += L" \"" + ref.object_name + L"\"";
      }
      shown++;
    }
    e.details = detail;
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
}

void BuildSuspectEvidence(AnalysisResult& r, i18n::Language lang, const EvidenceBuildContext& ctx)
{
  const bool en = ctx.en;

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
    const SuspectItem* selectedTop = &r.suspects[0];
    const bool topIsVictimish = !IsActionableSuspect(r.suspects[0]);
    if (topIsVictimish) {
      for (const auto& s : r.suspects) {
        if (IsActionableSuspect(s)) {
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
}

void BuildResourceEvidence(AnalysisResult& r, i18n::Language lang, const EvidenceBuildContext& ctx)
{
  if (r.resources.empty()) return;

  const bool en = ctx.en;
  const bool isCrashLike = ctx.isCrashLike;
  const bool isHangLike = ctx.isHangLike;

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

  {
    EvidenceItem e{};
    e.confidence_level = i18n::ConfidenceLevel::kMedium;
    e.confidence = ConfidenceText(lang, e.confidence_level);
    e.title = en
      ? L"Recent resource loads (mesh/anim)"
      : L"최근 로드된 리소스(메쉬/애니) 기록";
    e.details = JoinList(recent, 4, L", ");
    r.evidence.push_back(std::move(e));
  }

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

void BuildModuleClassificationEvidence(AnalysisResult& r, i18n::Language lang, const EvidenceBuildContext& ctx)
{
  const bool en = ctx.en;
  const bool hasModule = ctx.hasModule;
  const bool isSystem = ctx.isSystem;
  const bool isGameExe = ctx.isGameExe;

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
}

}  // namespace skydiag::dump_tool::internal

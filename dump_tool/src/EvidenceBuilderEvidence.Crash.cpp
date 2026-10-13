#include "EvidenceBuilderEvidencePipeline.h"

#include <algorithm>
#include <filesystem>
#include <vector>

#include "DumpObjects.h"
#include "MinidumpUtil.h"
#include "SkyrimDiagShared.h"

namespace skydiag::dump_tool::internal {

void BuildCrashLoggerEvidence(AnalysisResult& r, i18n::Language lang, const EvidenceBuildContext& ctx)
{
  const bool en = ctx.en;

  if (!r.crash_logger_log_path.empty()) {
    EvidenceItem e{};
    e.confidence_level = r.crash_logger_pairing_ambiguous
      ? i18n::ConfidenceLevel::kLow
      : i18n::ConfidenceLevel::kMedium;
    e.confidence = ConfidenceText(lang, e.confidence_level);
    e.title = en
      ? L"Crash Logger SSE/AE log auto-detected"
      : L"Crash Logger SSE/AE 로그를 자동으로 찾음";
    e.details = std::filesystem::path(r.crash_logger_log_path).filename().wstring();
    if (!r.crash_logger_version.empty()) {
      e.details += L" (" + r.crash_logger_version + L")";
    }
    e.details += en
      ? (L" | pairing delta=" + std::to_wstring(r.crash_logger_pairing_time_delta_ms) +
         L"ms, eligible=" + std::to_wstring(r.crash_logger_pairing_eligible_candidate_count))
      : (L" | 쌍짓기 시간차=" + std::to_wstring(r.crash_logger_pairing_time_delta_ms) +
         L"ms, 유효 후보=" + std::to_wstring(r.crash_logger_pairing_eligible_candidate_count));
    if (r.crash_logger_pairing_ambiguous) {
      e.details += en
        ? L" | ambiguous: nearby competing log exists; CrashLogger clues are downgraded."
        : L" | 모호함: 가까운 경쟁 로그가 있어 CrashLogger 단서를 낮게 평가합니다.";
    }
    r.evidence.push_back(std::move(e));
  }

  if (!r.crash_logger_exception_type.empty()) {
    // The exception Crash Logger reported; when the dump's own exception is a
    // secondary fault (inside CrashLogger), this is the original one.
    EvidenceItem e{};
    e.confidence_level = r.crash_logger_pairing_ambiguous ? i18n::ConfidenceLevel::kLow : i18n::ConfidenceLevel::kMedium;
    e.confidence = ConfidenceText(lang, e.confidence_level);
    e.title = en ? L"Crash Logger: reported exception" : L"Crash Logger가 기록한 예외";
    e.details = r.crash_logger_exception_type;
    if (!r.crash_logger_exception_location.empty()) {
      e.details += L" at " + r.crash_logger_exception_location;
    }
    if (!r.crash_logger_exception_instruction.empty()) {
      e.details += L" (" + r.crash_logger_exception_instruction + L")";
    }
    r.evidence.push_back(std::move(e));
  }

  if (!r.crash_logger_top_modules.empty()) {
    EvidenceItem e{};
    e.confidence_level = r.crash_logger_pairing_ambiguous
      ? i18n::ConfidenceLevel::kLow
      : i18n::ConfidenceLevel::kMedium;
    e.confidence = ConfidenceText(lang, e.confidence_level);
    e.title = en
      ? L"Crash Logger: top callstack modules"
      : L"Crash Logger 콜스택 상위 모듈";
    e.details = JoinList(r.crash_logger_top_modules, 4, L", ");
    r.evidence.push_back(std::move(e));
  }

  if (!r.crash_logger_object_refs.empty()) {
    EvidenceItem e{};
    e.confidence_level = (!r.crash_logger_pairing_ambiguous && r.crash_logger_object_refs[0].relevance_score >= 14)
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

  if (!r.crash_logger_unattributed_objects.empty()) {
    // Spawned actors and other forms created at runtime have no plugin to
    // blame, but they say what the game was handling.
    EvidenceItem e{};
    e.confidence_level = i18n::ConfidenceLevel::kLow;
    e.confidence = ConfidenceText(lang, e.confidence_level);
    e.title = en ? L"Crash Logger: relevant objects without a plugin (created at runtime)"
                 : L"Crash Logger: 플러그인이 표시되지 않은 관련 오브젝트(실행 중 생성)";
    std::vector<std::wstring> parts;
    for (const auto& object : r.crash_logger_unattributed_objects) {
      if (parts.size() >= 4u) {
        break;
      }
      std::wstring part = object.best_location + L": " + object.best_object_type;
      if (!object.object_name.empty()) {
        part += L" \"" + object.object_name + L"\"";
      }
      if (!object.form_id.empty()) {
        part += L" [" + object.form_id + L"]";
      }
      parts.push_back(std::move(part));
    }
    e.details = JoinList(parts, parts.size(), L" | ");
    r.evidence.push_back(std::move(e));
  }

  if (!r.dump_objects.empty()) {
    // ADR-0010: read from the dump itself, so it works without a Crash Logger
    // log. Context for the reader, not a cause on its own.
    EvidenceItem e{};
    e.confidence_level = i18n::ConfidenceLevel::kLow;
    e.confidence = ConfidenceText(lang, e.confidence_level);
    e.title = en
      ? L"Objects the crash registers and stack pointed at (read from the dump)"
      : L"크래시 당시 레지스터·스택이 가리킨 오브젝트(덤프에서 읽음)";
    std::vector<std::wstring> parts;
    for (const auto& object : r.dump_objects) {
      if (parts.size() >= 5u) {
        break;
      }
      parts.push_back(DescribeDumpObject(object));
    }
    e.details = JoinList(parts, parts.size(), L" | ");
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
    e.confidence_level = r.crash_logger_pairing_ambiguous
      ? i18n::ConfidenceLevel::kLow
      : i18n::ConfidenceLevel::kMedium;
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
  const ActionableCandidate* topCandidate = !r.actionable_candidates.empty() ? &r.actionable_candidates[0] : nullptr;
  const bool weakFaultLocationOnly =
    IsWeakFaultLocationOnlySuspect(r, ctx) ||
    (topCandidate && IsWeakFaultLocationActionableCandidate(r, *topCandidate, ctx));

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

  if (!r.fault_unloaded_module.empty()) {
    EvidenceItem e{};
    e.confidence_level = i18n::ConfidenceLevel::kMedium;
    e.confidence = ConfidenceText(lang, e.confidence_level);
    e.title = en ? L"Crashed in an unloaded DLL" : L"이미 언로드된 DLL 자리에서 크래시";
    e.details = en
      ? (L"The exception address lies where " + r.fault_unloaded_module +
         L" was loaded before it was unloaded. Code still called it afterwards, for example a hook or callback "
         L"it left behind (an SKSE plugin that failed to load is unloaded by SKSE).")
      : (L"예외 주소가 " + r.fault_unloaded_module +
         L"이(가) 언로드되기 전에 있던 자리입니다. 그 DLL이 내려간 뒤에도 남은 훅이나 콜백을 누군가 불렀다는 뜻입니다"
         L"(로드에 실패한 SKSE 플러그인은 SKSE가 언로드합니다).");
    r.evidence.push_back(std::move(e));
  }

  if (r.hook_stub_race.detected) {
    const auto& race = r.hook_stub_race;
    wchar_t stub[32]{};
    wchar_t target[32]{};
    swprintf_s(stub, L"0x%llx", static_cast<unsigned long long>(race.stub_address));
    swprintf_s(target, L"0x%llx", static_cast<unsigned long long>(race.target_offset));
    const std::wstring callSite = race.call_site.empty()
      ? std::wstring{}
      : (en ? (L" The game call at " + race.call_site + L" was patched to enter it.")
            : (L" 게임의 " + race.call_site + L" 호출이 이 코드로 가도록 바뀌어 있었습니다."));
    EvidenceItem e{};
    e.confidence_level = i18n::ConfidenceLevel::kHigh;
    e.confidence = ConfidenceText(lang, e.confidence_level);
    e.title = en ? L"Crash in a hook being installed" : L"설치 중인 후킹에서 크래시";
    e.details = en
      ? (L"The CPU ran an int3 at " + std::wstring(stub) + L", outside every module, but the dump holds a finished "
         L"jump there to " + race.target_module + L"+" + target + L": the jump was written right after the crash."
         + callSite)
      : (L"CPU는 어느 모듈에도 속하지 않은 " + std::wstring(stub) + L"에서 int3를 실행했지만, 덤프의 그 자리에는 " +
         race.target_module + L"+" + target + L"(으)로 가는 완성된 점프 코드가 있습니다. 크래시 직후에 점프 코드가 써졌다는 뜻입니다." +
         callSite);
    r.evidence.push_back(std::move(e));
  }

  if (!r.stackwalk_truncation.empty()) {
    wchar_t addr[32]{};
    swprintf_s(addr, L"0x%llx", static_cast<unsigned long long>(r.stackwalk_truncation_address));
    const std::wstring depth = std::to_wstring(r.stackwalk_truncation_depth);
    std::wstring why;
    if (r.stackwalk_truncation == "no_unwind_info") {
      const std::wstring module = r.stackwalk_truncation_module.empty() ? std::wstring(addr) : r.stackwalk_truncation_module;
      why = en ? (module + L" has no unwind data for that frame, so its caller cannot be found")
               : (module + L" 프레임에 스택 해제 정보가 없어 그 호출자를 찾을 수 없습니다");
      // ADR-0011: a Windows DLL's unwind data comes from the dump (v0.2.61+)
      // or from a local file of the same build, which a Windows update replaces.
      if (minidump::IsSystemishModule(r.stackwalk_truncation_module)) {
        why += en ? L" (typical of a dump from before v0.2.61 opened after a Windows update; newer dumps carry it)"
                  : L"(v0.2.61 이전 덤프를 Windows 업데이트 뒤에 열 때 흔합니다. 새 덤프는 이 정보를 담습니다)";
      }
    } else if (r.stackwalk_truncation == "non_code_address") {
      why = en ? (L"the next return address " + std::wstring(addr) +
                  L" points at data, not code, so the stack was overwritten after the fault")
               : (L"다음 리턴 주소 " + std::wstring(addr) + L"가 코드가 아닌 데이터를 가리켜, 크래시 뒤 스택이 덮어쓰인 것으로 보입니다");
    } else if (!r.stackwalk_truncation_unloaded_module.empty()) {
      why = en ? (L"the next return address " + std::wstring(addr) + L" lies in " +
                  r.stackwalk_truncation_unloaded_module + L", which was unloaded before the capture")
               : (L"다음 리턴 주소 " + std::wstring(addr) + L"가 캡처 전에 언로드된 " +
                  r.stackwalk_truncation_unloaded_module + L" 자리입니다");
    } else {
      why = en ? (L"the next return address " + std::wstring(addr) + L" is in no loaded module")
               : (L"다음 리턴 주소 " + std::wstring(addr) + L"가 로드된 어떤 모듈에도 속하지 않습니다");
    }
    EvidenceItem e{};
    e.confidence_level = i18n::ConfidenceLevel::kMedium;
    e.confidence = ConfidenceText(lang, e.confidence_level);
    e.title = en ? L"Call stack cut short" : L"콜스택이 중간에 끊김";
    e.details = en
      ? (L"The walk stopped after " + depth + L" frames: " + why +
         L". The frames below, often the mod that started the call, are not visible; a missing or weak suspect "
         L"may be why.")
      : (L"콜스택을 " + depth + L"번째 프레임까지만 풀었습니다. " + why +
         L". 그 아래의 프레임(호출을 시작한 모드인 경우가 많음)은 보이지 않으므로, 후보가 없거나 약하면 이 때문일 수 있습니다.");
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
    if (r.modal_dialog_wait.detected) {
      e.title = en
        ? L"Stack modules on the waiting main thread (not the hang cause)"
        : L"대기 중인 메인 스레드의 스택 모듈 (프리징 원인 아님)";
    } else if (weakFaultLocationOnly) {
      e.title = en
        ? L"Top stack DLL clue (fault-location)"
        : L"콜스택 상위 DLL 단서(예외가 난 위치)";
    } else {
      e.title = en
        ? (r.suspects_from_stackwalk ? L"Top suspect (callstack-based)" : L"Top suspect (stack-scan-based)")
        : (r.suspects_from_stackwalk ? L"콜스택 기반 유력 후보" : L"스택 스캔 기반 유력 후보");
    }

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
    if (weakFaultLocationOnly) {
      e.details += en
        ? L" — current signal is still fault-location only."
        : L" — 지금은 예외가 난 위치라는 단서만 있습니다.";
    }
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
        ? (L"The crash happened in " + fn + L" (" + r.fault_module_plus_offset + L"). "
           L"A game function only shows where it crashed; the cause is usually a mod that called it with bad data.")
        : (L"크래시가 난 게임 함수: " + fn + L" (" + r.fault_module_plus_offset + L"). "
           L"게임 함수는 터진 위치일 뿐이고, 원인은 대개 잘못된 데이터로 이 함수를 부른 모드입니다.");
      r.evidence.push_back(std::move(e));
    }
  }

  if (!r.thread_entry_hook_modules.empty()) {
    std::wstring hooks;
    for (const auto& module : r.thread_entry_hook_modules) {
      hooks += (hooks.empty() ? L"" : L", ") + module;
    }
    EvidenceItem e{};
    e.confidence_level = i18n::ConfidenceLevel::kMedium;
    e.confidence = ConfidenceText(lang, e.confidence_level);
    e.title = ctx.en
      ? L"Thread-entry hooks left out of the suspects"
      : L"게임 스레드 시작 지점의 훅(후보에서 제외)";
    e.details = ctx.en
      ? (hooks + L" only wrap the start of the game thread that crashed: below them is just the game's thread "
                 L"start, and the fault is deep in game code above them. Every stack of that thread goes "
                 L"through them, so they are not suspects for this crash.")
      : (hooks + L"은(는) 크래시가 난 게임 스레드의 시작 지점에 끼어 있을 뿐입니다. 아래에는 게임의 스레드 시작 코드만 "
                 L"있고, 크래시는 그 위의 게임 코드 깊은 곳에서 났습니다. 이 스레드의 모든 콜스택이 이 DLL을 지나가므로 "
                 L"이번 크래시의 후보로 보지 않습니다.");
    r.evidence.push_back(std::move(e));
  }
}

}  // namespace skydiag::dump_tool::internal

#include "EvidenceBuilderInternalsPriv.h"

#include <cwchar>

#include "SkyrimDiagShared.h"

namespace skydiag::dump_tool::internal {

void BuildRecommendations(AnalysisResult& r, i18n::Language lang, const EvidenceBuildContext& ctx)
{
  const bool en = ctx.en;
  const bool isSnapshotLike = ctx.isSnapshotLike;
  const bool isHangLike = ctx.isHangLike;
  const bool isManualCapture = ctx.isManualCapture;
  const bool hasModule = ctx.hasModule;
  const bool isSystem = ctx.isSystem;
  const bool isGameExe = ctx.isGameExe;
  const auto hitch = ctx.hitch;
  const auto& wct = ctx.wct;
  const std::wstring& suspectBasis = ctx.suspectBasis;

  // Recommendations (checklist)
  if (isSnapshotLike) {
    r.recommendations.push_back(en
      ? L"[Snapshot] No exception/crash info is present. This dump alone is not enough to blame a mod."
      : L"[정상/스냅샷] 예외(크래시) 정보가 없습니다. 이 덤프만으로 '어떤 모드가 크래시 원인'인지 판단하기 어렵습니다.");
    r.recommendations.push_back(en
      ? L"[Snapshot] Capture during a real issue for diagnosis: (1) real CTD dump, (2) manual capture during freeze/infinite loading (Ctrl+Shift+F12) or an auto hang dump."
      : L"[정상/스냅샷] 문제 상황에서 캡처해야 진단이 가능합니다: (1) 실제 크래시 덤프, (2) 프리징/무한로딩 중 수동 캡처(Ctrl+Shift+F12) 또는 자동 감지 덤프");
  }

  if (r.exc_code != 0) {
    if (r.exc_code == 0xC0000005u) {
      r.recommendations.push_back(en
        ? L"[Basics] ExceptionCode=0xC0000005 (Access Violation). Often caused by DLL hooks / invalid memory access."
        : L"[기본] ExceptionCode=0xC0000005(접근 위반)입니다. 보통 DLL 후킹/메모리 접근 문제로 발생합니다.");
    } else {
      wchar_t buf[128]{};
      swprintf_s(buf, en ? L"[Basics] ExceptionCode=0x%08X." : L"[기본] ExceptionCode=0x%08X 입니다.", r.exc_code);
      r.recommendations.push_back(buf);
    }

    if (r.exc_code == 0xE06D7363u) {
      r.recommendations.push_back(en
        ? L"[Interpretation] 0xE06D7363 is a common C++ exception (throw) code. It can occur during normal throw/catch."
        : L"[해석] 0xE06D7363은 흔한 C++ 예외(throw) 코드입니다. 정상 동작 중에도 throw/catch로 발생할 수 있습니다.");
      r.recommendations.push_back(en
        ? L"[Interpretation] If the game did not actually crash, this dump may be a handled exception false positive."
        : L"[해석] 게임이 실제로 튕기지 않았다면, 이 덤프는 '실제 CTD'가 아니라 'handled exception 오탐'일 수 있습니다.");
      r.recommendations.push_back(en
        ? L"[Config] Using SkyrimDiag.ini CrashHookMode=1 (fatal only) greatly reduces these false positives."
        : L"[설정] SkyrimDiag.ini의 CrashHookMode=1(치명 예외만)로 두면 이런 오탐을 크게 줄일 수 있습니다.");
    }
  }

  if (ctx.isHookFramework) {
    r.recommendations.push_back(en
      ? L"[Hook framework] This mod extensively hooks the game engine. It may be a victim of memory corruption caused by another mod, not the root cause itself. Check other suspect candidates first."
      : L"[훅 프레임워크] 이 모드는 게임 엔진을 광범위하게 훅합니다. 다른 모드의 메모리 오염으로 인한 피해자일 수 있으며, 이 모드 자체가 원인이 아닐 수 있습니다. 다른 후보 모드를 먼저 점검하세요.");
  }

  if (!r.inferred_mod_name.empty()) {
    r.recommendations.push_back(en
      ? (L"[Top suspect] Reproduce after updating/reinstalling '" + r.inferred_mod_name + L"'.")
      : (L"[유력 후보] '" + r.inferred_mod_name + L"' 모드를 업데이트/재설치 후 재현 여부 확인"));
    r.recommendations.push_back(en
      ? (L"[Top suspect] If it repeats, disable the mod (or its SKSE plugin DLL) and retest: '" + r.inferred_mod_name + L"'.")
      : (L"[유력 후보] 동일 크래시가 반복되면 '" + r.inferred_mod_name + L"' 모드(또는 해당 모드의 SKSE 플러그인 DLL)를 비활성화 후 재현 여부 확인"));
  }

  if (r.inferred_mod_name.empty() && !r.suspects.empty()) {
    const auto& s0 = r.suspects[0];
    if (!s0.inferred_mod_name.empty()) {
      r.recommendations.push_back(en
        ? (L"[Top suspect] " + suspectBasis + L" candidate: reproduce after updating/reinstalling '" + s0.inferred_mod_name + L"'.")
        : (L"[유력 후보] " + suspectBasis + L" 기반 후보: '" + s0.inferred_mod_name + L"' 모드 업데이트/재설치 후 재현 여부 확인"));
      r.recommendations.push_back(en
        ? (L"[Top suspect] If it repeats, disable the mod (or its SKSE plugin DLL) and retest: '" + s0.inferred_mod_name + L"'.")
        : (L"[유력 후보] 동일 문제가 반복되면 '" + s0.inferred_mod_name + L"' 모드(또는 해당 모드의 SKSE 플러그인 DLL)를 비활성화 후 재현 여부 확인"));
    } else if (!s0.module_filename.empty()) {
      r.recommendations.push_back(en
        ? (L"[Top suspect] " + suspectBasis + L" candidate DLL: " + s0.module_filename + L" — check the providing mod first.")
        : (L"[유력 후보] " + suspectBasis + L" 기반 후보 DLL: " + s0.module_filename + L" — 포함된 모드를 우선 점검"));
    }
  }

  if (!r.resources.empty()) {
    bool hasConflict = false;
    for (const auto& rr : r.resources) {
      if (rr.is_conflict) {
        hasConflict = true;
        break;
      }
    }

    r.recommendations.push_back(en
      ? L"[Mesh/Anim] This dump includes recent resource load history (.nif/.hkx/.tri). Check the 'Recent resources' section."
      : L"[메쉬/애니] 이 덤프에는 최근 로드된 리소스(.nif/.hkx/.tri) 기록이 포함되어 있습니다. '최근 로드된 리소스' 항목을 확인하세요.");
    if (hasConflict) {
      r.recommendations.push_back(en
        ? L"[Conflict] If multiple mods provide the same file, conflicts are common. Adjust MO2 priority / disable mods to retest."
        : L"[충돌] 같은 파일을 제공하는 모드가 2개 이상이면 충돌 가능성이 큽니다. MO2에서 우선순위(모드 순서) 조정/비활성화로 재현 여부 확인");
    }
  }

  if (hitch.count > 0) {
    r.recommendations.push_back(en
      ? L"[Performance] PerfHitch events were recorded. Check Events tab (t_ms and hitch(ms)) to see when the stutter happens."
      : L"[성능] PerfHitch 이벤트가 기록되었습니다. 이벤트 탭에서 t_ms와 hitch(ms)를 확인해 '언제 끊기는지' 먼저 파악하세요.");
    if (!r.resources.empty()) {
      r.recommendations.push_back(en
        ? L"[Performance] Check Resources tab for .nif/.hkx/.tri loaded right before/after the hitch, and their providing mods. (Correlation, not proof)"
        : L"[성능] 리소스 탭에서 히치 직전/직후 로드된 .nif/.hkx/.tri 및 제공 모드를 확인하세요. (상관관계 기반, 확정 아님)");
    }
  }

  if (hasModule && !isSystem && !isGameExe) {
    r.recommendations.push_back(en
      ? L"[Top suspect] Verify prerequisites/versions for the mod containing this DLL (SKSE / Address Library / game runtime)."
      : L"[유력 후보] 해당 DLL이 포함된 모드의 선행 모드/요구 버전(SKSE/Address Library/엔진 버전) 충족 여부 확인");
    r.recommendations.push_back(en
      ? L"[Top suspect] Attach this report (*_SkyrimDiagReport.txt) and dump (*.dmp) when reporting to the mod author."
      : L"[유력 후보] 이 리포트 파일(*_SkyrimDiagReport.txt)과 덤프(*.dmp)를 모드 제작자에게 첨부");
  } else if (hasModule && isGameExe) {
    r.recommendations.push_back(en
      ? L"[Check] Crash location is the game executable. Version mismatch (Address Library/SKSE) or hook conflicts are likely."
      : L"[점검] 크래시 위치가 게임 본체(EXE)로 나옵니다. Address Library/ SKSE 버전 불일치 또는 후킹 충돌 가능성이 큽니다.");
    r.recommendations.push_back(en
      ? L"[Check] Disable recently added/updated SKSE plugin DLLs one by one and retest."
      : L"[점검] 최근 추가/업데이트한 SKSE 플러그인(DLL)부터 하나씩 제외하며 재현 여부 확인");
  } else if (hasModule && isSystem) {
    r.recommendations.push_back(en
      ? L"[Check] When a Windows system DLL is shown, the real culprit is often another mod/DLL."
      : L"[점검] Windows 시스템 DLL로 표시될 때는 실제 원인이 다른 모드/DLL인 경우가 많습니다.");
    r.recommendations.push_back(en
      ? L"[Check] Disable recently added/updated SKSE plugin DLLs one by one and retest."
      : L"[점검] 최근 추가/업데이트한 SKSE 플러그인(DLL)부터 하나씩 제외하며 재현 여부 확인");
    r.recommendations.push_back(en
      ? L"[Check] Verify SKSE version, game runtime (AE/SE/VR), and Address Library all match."
      : L"[점검] SKSE 버전/게임 버전(AE/SE/VR)/Address Library 버전이 서로 맞는지 확인");
  } else {
    if (!isSnapshotLike) {
      r.recommendations.push_back(en
        ? L"[Check] Fault module could not be determined. Capturing again with DumpMode=2 (FullMemory) can provide more clues."
        : L"[점검] 덤프에서 fault module을 특정하지 못했습니다. DumpMode를 2(FullMemory)로 올려 다시 캡처하면 단서가 늘 수 있습니다.");
    }
  }

  if ((r.state_flags & skydiag::kState_Loading) != 0u) {
    r.recommendations.push_back(en
      ? L"[Loading] Crashes right after load screens often involve animation/mesh/texture/skeleton/script initialization."
      : L"[로딩 중] 로딩 화면/세이브 로드 직후 크래시는 애니메이션/메쉬/텍스처/스켈레톤/스크립트 초기화 쪽이 흔합니다.");
    r.recommendations.push_back(en
      ? L"[Loading] Check mods affecting that stage first (animations/skeleton/body/physics/precaching)."
      : L"[로딩 중] 해당 시점에 개입하는 모드(애니메이션/스켈레톤/바디/물리/프리캐시)를 우선 점검");
  }

  if (r.has_wct) {
    if (isHangLike) {
      if (wct) {
        if (wct->cycles > 0) {
          r.recommendations.push_back(en
            ? L"[Hang] WCT detected isCycle=true thread(s). Deadlock is likely."
            : L"[프리징] WCT에서 isCycle=true 스레드가 감지되었습니다. 데드락 가능성이 높습니다.");
        } else {
          r.recommendations.push_back(en
            ? L"[Hang] No WCT cycle: possible infinite loop / busy wait."
            : L"[프리징] WCT cycle이 없으면 무한루프/바쁜 대기(busy wait) 가능성도 있습니다.");
        }
      }
      r.recommendations.push_back(en
        ? L"[Hang] If it repeats, use Events tab (just before the freeze) to narrow related mods."
        : L"[프리징] 프리징이 반복되면 문제 상황 직전에 실행된 이벤트(이벤트 탭)를 기준으로 관련 모드를 점검");
    } else if (isManualCapture && isSnapshotLike) {
      if (wct && wct->has_capture && wct->thresholdSec > 0u && wct->secondsSinceHeartbeat < static_cast<double>(wct->thresholdSec)) {
        wchar_t buf[256]{};
        swprintf_s(
          buf,
          en
            ? L"[Manual] At capture time, heartbeatAge=%.1fs < threshold=%us, so it is not considered a hang."
            : L"[수동] 수동 캡처 당시 heartbeatAge=%.1fs < threshold=%us 이므로 '프리징/무한로딩'으로 판단되지 않습니다.",
          wct->secondsSinceHeartbeat,
          wct->thresholdSec);
        r.recommendations.push_back(buf);
      }
      r.recommendations.push_back(en
        ? L"[Manual] Manual captures include WCT. For real freezes/infinite loading, check the WCT tab from a capture taken during the issue."
        : L"[수동] 수동 캡처에는 WCT가 포함됩니다. 실제 프리징/무한로딩 중 캡처한 덤프에서 WCT 탭을 참고하세요.");
    }
  }
}

}  // namespace skydiag::dump_tool::internal


# ADR-0031: 두 번 처리된 같은 예외는 덤프를 먼저 쓰지 않는다

## Status
Accepted (2026-11-11)

## Context
- `CrashHookMode=1`에서 플러그인은 접근 위반(`0xC0000005`) 같은 치명적 예외 코드를 1차 예외(first-chance) 단계에서 기록해 알리고, Helper는 게임이 그 예외를 스스로 처리했는지 확인하기 전에 먼저 크래시 덤프(`MiniDumpWriteDump`)부터 씁니다. 처리되지 않은 진짜 크래시는 게임이 곧바로 종료될 수 있어 덤프를 나중에 쓰면 주소 공간이 사라지기 때문입니다.
- 문제는 `MiniDumpWriteDump`가 실행되는 동안 대상 프로세스의 모든 스레드를 멈춘다는 점입니다. 게임이나 모드가 같은 명령 주소에서 SEH 등으로 예외를 내고 스스로 처리하기를 반복하면, 예외가 날 때마다 게임 전체가 약 1초씩 멈춥니다.
- Nexus 제보(v0.2.73, 현재 `main`도 동일)의 WPR 추적에서 메인 스레드가 1,162ms 동안 멈춘 구간이 `SkyrimDiagHelper.exe`의 `MiniDumpWriteDump` 실행 시각과 일치했습니다.
- 기존 `IsRepeatedRecoveredFault` 완화 규칙은 같은 코드·주소가 10분 안에 두 번 풀렸을 때 세 번째부터 최소 크기(`DumpMode::kMini`) 덤프를 먼저 쓰고, 메인 스레드 예외는 제외했습니다. 그러나 최소 덤프도 `MiniDumpWriteDump`를 호출하므로 스레드 정지는 막지 못했고, 메인 스레드에서 반복되는 예외나 10분 간격 바깥의 반복도 그대로 정지를 일으켰습니다.

## Decision
세션 안에서 같은 예외(`exceptionCode` + `exceptionAddr`)가 이미 두 번(`kRepeatedRecoveredFaultMinCount = 2`) "처리됨"으로 풀렸다면, 세 번째부터는 덤프를 먼저 쓰지 않고 회복 여부를 먼저 확인합니다(`CheckRepeatedRecoveredFault`, `helper/src/CrashCapture.cpp`).

1. **처음 두 번과 처음 보는 예외는 지금과 같습니다.** 덤프를 먼저 쓰고, 하트비트 진행으로 회복이 확인되면 `SkyrimDiag_Released_<ts>.dmp`로 보류한 뒤 `NoteRecoveredFault`로 횟수를 셉니다.
2. **세 번째부터는 먼저 회복을 확인합니다(`FilterFirstChanceException`).**
   - **회복하거나 종료 코드 0으로 정상 종료하면:** 덤프를 쓰지 않습니다(`MiniDumpWriteDump` 호출 없음, 스레드 정지 없음). `kState_Frozen`을 해제하고(`TryClearRecoveredCrashFreeze`), 해제 상태(`releasedCrashSeq`, `releasedOnBriefHeartbeat`)와 `NoteRecoveredFault` 횟수를 갱신합니다. 이전에 보류 중이던 덤프가 있으면 이 시점에 정리합니다(`DiscardReleasedCrashDump`).
   - **회복하지 못했고 게임 프로세스가 아직 살아 있으면:** 그때 설정된 프로필(`configuredDumpProfile`)로 크래시 덤프를 바로 쓰고 기존 크래시 후처리(`ProcessValidCrashDump`)로 넘어갑니다. 최소 덤프를 먼저 쓰고 다시 쓰는 단계(`RewriteCrashDumpAtConfiguredSize`)는 제거합니다.
   - **확인 도중 게임이 비정상 종료해 이미 프로세스가 없으면:** 죽은 프로세스에 `MiniDumpWriteDump`를 시도하지 않고, 종료 경로(`DrainCrashEventBeforeExit`)에서 `SkyrimDiag_PostExitCrashEvidence_*.json`(`committed_fault_dump_write_failed_process_exited`)으로 예외 메타데이터만 남깁니다. 이때 보류 중이던 이전 덤프가 같은 반복 예외의 처리된 프로브에서 나온 것이면 승격하지 않고 지우며, 직전 다른 예외에서 남은 보류 덤프라면 ADR-0028 판정 3대로 구제합니다.
3. **메인 스레드 예외에도 적용하고, 세션 내내 기억합니다.** 메인 스레드 제외를 없애고 10분 유효 시간(`kRepeatedRecoveredFaultCooldownMs`)을 제거해 세션 동안 최근 16개(`kRecoveredFaultHistoryCapacity`) 슬롯에서 기억합니다. 모든 호출이 같은 `KernelBase` 주소를 공유하는 C++ 예외(`0xE06D7363`)와 중단점(`0x80000003`)은 지금처럼 제외합니다.
4. **워커 스레드 예외의 종료 보고 규칙은 유지합니다.** `releasedInThreadDumpWindow`는 켜지 않으며, 하트비트로 풀어 준 뒤 60초 안에 게임이 비정상 종료하면 `ShouldReportReleasedCrashAtExit`에 따라 크래시로 기록합니다(세 번째 이후라 보류 덤프가 없으면 `released_fault_followed_by_abnormal_exit` 메타데이터 기록).

## Alternatives Considered
- **최소 덤프(`kMini`) 유지:** 덤프 파일 크기는 작아지지만 `MiniDumpWriteDump`가 모든 스레드를 멈추는 시간 자체가 스터터 원인이므로 문제를 해결하지 못합니다.
- **첫 번째 발생부터 덤프 전에 회복 확인:** 처음 보는 진짜 CTD가 4\~6초 확인 창 안에 프로세스를 끝내면 가장 흔한 단일 크래시에서 덤프를 잃게 됩니다. 이미 두 번 처리됨이 확인된 코드+주소로 한정하면 일반 크래시의 덤프 우선 수집을 해치지 않습니다.
- **메인 스레드 계속 제외:** 메인 스레드에서 SEH로 감싼 예외가 반복될 때도 똑같이 `MiniDumpWriteDump` 정지가 발생하며, 메인 스레드가 실제로 죽지 않고 계속 돌면 하트비트 진행으로 회복을 바로 판별할 수 있습니다.
- **`releasedInThreadDumpWindow = true` 재사용:** 그 플래그를 켜면 종료 시 Crash Logger의 `crash-*.log`가 있어야만 기록하므로, Crash Logger가 없거나 로그를 남기기 전에 끝난 비정상 종료를 놓칩니다.

## Consequences
- 게임이나 모드가 같은 주소에서 반복해 내고 스스로 처리하는 예외는 처음 두 번 뒤부터 `MiniDumpWriteDump`를 부르지 않아 주기적인 1초 안팎의 멈춤이 사라집니다.
- 회복하지 못한 채 살아 있는 정지형 크래시는 확인 직후 설정된 크기로 한 번만 덤프를 씁니다.
### Limitations
- **감수하는 한계:** 같은 세션에서 이미 두 번 "처리됨"으로 풀렸던 똑같은 코드+주소에서 나중에 실제로 처리되지 않은 진짜 크래시가 나고, 확인 창(약 2\~6초) 안에 게임 프로세스가 곧바로 종료되면 `.dmp` 파일은 남지 않고 `SkyrimDiag_PostExitCrashEvidence_*.json` 예외 정보만 남습니다.
- 서로 다른 17개 이상의 주소가 번갈아 처리된 예외를 내면 16개 링 버퍼(`kRecoveredFaultHistoryCapacity`)에서 오래된 항목이 밀려나 다시 처음 두 번의 덤프를 쓸 수 있습니다.
- 처음 두 번의 발생에서는 여전히 덤프를 먼저 쓰므로 세션 초반 두 번의 멈춤은 남습니다.

## Verification
- 순수 규칙(`crash_capture_filter_logic_tests`): 두 번 처리된 뒤 세션 내내 유지, 다른 주소·코드 미적용, C++ 예외·중단점 제외, 16개 슬롯 순환 교체.
- 가드 테스트(`crash_capture_false_positive_guard_tests`, `crash_capture_refactored_guard_tests`, `plugin_stream_tests`): `CheckRepeatedRecoveredFault` 흐름, `releasedInThreadDumpWindow = false` 고정, `NoteRecoveredFault` 갱신 확인.
- 런타임 테스트(`helper_runtime_smoke_tests`, `helper_false_positive_runtime_tests`): 처음 두 번은 기존처럼 보류 덤프 생성, 세 번째(메인 스레드 포함) 회복 시 덤프 미생성 및 동결 해제, 미회복 생존 시 설정 프로필 덤프 생성, 해제 후 60초 이내 비정상 종료 및 즉시 종료 시 `PostExitCrashEvidence` 기록 확인.
- 실게임 미확인: 반복 예외를 내는 실제 모드 환경에서의 체감 스터터 해소 여부.

## References
- ADR-0017 (`0017-second-fault-while-filtering.md`)
- ADR-0028 (`0028-whole-mod-review-2026-11.md`, 판정 3·4)

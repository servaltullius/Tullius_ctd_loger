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
- **덤프 대신 프로세스 스냅샷(`PssCaptureSnapshot`의 VA 복제)을 떠 두고, 게임이 죽으면 그 스냅샷으로 덤프 쓰기:** 이미 두 번 처리된 주소에서 난 진짜 크래시의 덤프를 잃지 않게 되지만, 없애려던 멈춤이 되살아납니다. 2026-11-13에 메모리를 모두 건드린 시험 프로세스로 쟀습니다(실제 게임에서는 여유 커밋이 모자라 재지 못함).
  - 복제하는 동안 대상 프로세스의 메모리 할당이 멈췄고, 그 시간은 private 1GB당 약 14ms였습니다(4GB 54ms, 8GB 110ms, 12GB 214ms). 프로세스의 첫 스냅샷은 그 두 배였습니다.
  - 복제본은 대상의 private 크기만큼 시스템 커밋을 더 잡습니다(4GB에 3.9GB, 8GB에 7.8GB).
  - 12GB 시험에서는 커밋 여유가 23.6GB였는데도 6번 중 5번이 `ERROR_NO_SYSTEM_RESOURCES`(1450)로 실패했고(원인 미확인), 실패한 시도도 대상을 같은 시간만큼 멈췄습니다.
  - 플러그인 약 4,250개 구성의 게임은 private 22\~24GB입니다. 같은 비율이면 반복되는 예외마다 0.15\~0.3초쯤 멈추고(추정), 그만한 여유 커밋이 있어야 합니다. 그 PC(RAM 64GB, 커밋 한도 103.6GB)에서 게임이 켜져 있을 때의 여유 커밋은 11월 초 수동 캡처 6건에서 22\~33GB로, 필요한 크기를 겨우 넘거나 한 건에서는 모자랐습니다.

## Consequences
- 게임이나 모드가 같은 주소에서 반복해 내고 스스로 처리하는 예외는 처음 두 번 뒤부터 `MiniDumpWriteDump`를 부르지 않아 주기적인 1초 안팎의 멈춤이 사라집니다.
- 회복하지 못한 채 살아 있는 정지형 크래시는 확인 직후 설정된 크기로 한 번만 덤프를 씁니다.
### Limitations
- **감수하는 한계:** 같은 세션에서 이미 두 번 "처리됨"으로 풀렸던 똑같은 코드+주소에서 나중에 실제로 처리되지 않은 진짜 크래시가 나고, 확인 창(약 2\~6초) 안에 게임 프로세스가 곧바로 종료되면 `.dmp` 파일은 남지 않고 `SkyrimDiag_PostExitCrashEvidence_*.json` 예외 정보만 남습니다. 실게임에서 Crash Logger가 있는 구성으로 이 경우를 만들었을 때 게임은 약 3.7초 만에 끝났고 덤프는 남지 않았습니다(Verification의 2026-11-13 확인).
- 서로 다른 17개 이상의 주소가 번갈아 처리된 예외를 내면 16개 링 버퍼(`kRecoveredFaultHistoryCapacity`)에서 오래된 항목이 밀려나 다시 처음 두 번의 덤프를 쓸 수 있습니다.
- 처음 두 번의 발생에서는 여전히 덤프를 먼저 쓰므로 세션 초반 두 번의 멈춤은 남습니다.
- **회복을 확인하는 동안 캡처가 묶입니다.** 확인은 2초 간격으로 하트비트를 보므로 회복하면 약 4초, 회복하지 못하면 6초가 걸리고, 그동안 `kState_Frozen`이 유지됩니다. 게임은 멈추지 않지만 플러그인은 이벤트·리소스 기록을 쉬고, 그 사이에 난 다른 크래시는 기록되지 않습니다(ADR-0017의 한계와 같음). 덤프를 먼저 쓰던 때(덤프 쓰기에 이어 3초, 다시 4초 이상)보다는 짧지만, 같은 예외가 몇 초 간격으로 반복되는 구성에서는 이 구간의 비중이 커집니다. 확인 간격을 줄이는 안은 넣지 않았습니다. 로딩이나 끊김으로 하트비트가 잠깐 늦을 때 "회복하지 못함"으로 보고 덤프를 써서 정지가 되살아날 수 있고, 반복 빈도에 대한 실제 자료도 제보 로그 한 건뿐입니다(2026-11-11, 이 변경 전 빌드: 같은 주소의 접근 위반이 23분 동안 14번, 가장 짧은 간격 9.2초. 덤프를 먼저 쓰던 빌드라 Helper가 한 예외를 확인하는 동안 난 예외는 빠져 있을 수 있습니다).
- **확인 도중 게임이 종료 코드 0으로 끝나면 `SkyrimDiag_CleanExitEvidence_*.json`을 쓰지 않습니다.** 덤프를 먼저 쓰는 경로는 치명적 코드의 예외 뒤 정상 종료에 이 기록을 남기지만, 여기서는 이미 두 번 처리된 예외라 남기지 않습니다.

## Verification
- 순수 규칙(`crash_capture_filter_logic_tests`): 두 번 처리된 뒤 세션 내내 유지, 다른 주소·코드 미적용, C++ 예외·중단점 제외, 16개 슬롯 순환 교체.
- 가드 테스트(`crash_capture_false_positive_guard_tests`, `crash_capture_refactored_guard_tests`, `plugin_stream_tests`): `CheckRepeatedRecoveredFault` 흐름, `releasedInThreadDumpWindow = false` 고정, `NoteRecoveredFault` 갱신 확인.
- 런타임 테스트(`helper_runtime_smoke_tests`, `helper_false_positive_runtime_tests`): 처음 두 번은 기존처럼 보류 덤프 생성, 세 번째(메인 스레드 포함) 회복 시 덤프 미생성 및 동결 해제, 미회복 생존 시 설정 프로필 덤프 생성, 해제 후 60초 이내 비정상 종료 및 즉시 종료 시 `PostExitCrashEvidence` 기록 확인.
- 실게임 확인(2026-11-13, 공개된 v0.2.76-rc1 zip, 게임 1.6.1170, 개발용 `tools/crash_tester`의 Ctrl+Shift+Insert): 읽을 수 없는 메모리를 엔진에 넘겨 `SkyrimSE.exe+0xCC90CF`(Address Library ID 68233+0xF)에서 나는 접근 위반을 CrashTester.dll이 스스로 잡기를 15초 간격으로 6번 반복했습니다. 스레드는 네 개였습니다(게임 창 스레드 1번, 다른 게임 스레드 2개에서 1번씩, CrashTester.dll 스레드 3번).
  - Helper 로그에는 여섯 번 모두 같은 코드와 주소로 기록됐습니다. 첫 번째와 두 번째는 덤프를 쓰고 보류했고, 세 번째부터 여섯 번째까지는 "checking whether the game recovers before writing a crash dump"에 이어 "No dump written: this repeated exception was handled again and the game kept running"으로 끝났습니다. 세 번째가 게임 창 스레드에서 난 것입니다.
  - 테스터가 잰, 다음 예외까지 자기 스레드가 가장 오래 멈춘 시간은 첫 번째 1,717ms, 두 번째 1,392ms, 세 번째부터 다섯 번째까지 각각 6ms였습니다(재는 간격 5ms). 여섯 번째는 15초가 차기 전에 게임을 종료해 값이 없습니다.
  - 두 번째의 보류 덤프는 세 번째 예외가 왔을 때 지워졌고, 게임을 종료 코드 0으로 끝냈을 때 마지막 예외를 크래시로 다시 보고하지 않았습니다. 출력 폴더에는 덤프도 사고 기록도 남지 않았습니다.
- 실게임 확인(2026-11-13, 같은 구성, `tools/crash_tester`의 Ctrl+Shift+Delete): 같은 접근 위반을 15초 간격으로 두 번 잡은 뒤, 세 번째는 게임 스레드에서 잡지 않고 두었습니다. Limitations의 첫 항목이 그대로 일어났습니다.
  - Helper는 세 번째에서 덤프를 먼저 쓰지 않고 회복을 확인했습니다. 그 사이 Crash Logger 1.27이 자기 로그를 쓰고 게임이 종료 코드 1로 끝났습니다. 예외에서 종료까지 약 3.7초였고, Helper가 회복하지 못했다고 판정해 덤프를 쓰는 시점(6초)보다 먼저였습니다.
  - 남은 것은 `SkyrimDiag_PostExitCrashEvidence_*.json`(`committed_fault_dump_write_failed_process_exited`, 예외 코드·주소·스레드)과 WER LocalDumps 안내 파일입니다. 덤프와 리포트는 없고 `crash_history.json`에도 들어가지 않았습니다. Crash Logger의 로그는 같은 주소로 남았습니다.
  - 두 번째 예외의 보류 덤프는 이 크래시의 덤프로 승격되지 않고 지워졌습니다.
- 실게임 미확인: 제보 환경(다른 모드가 내는 예외, 더 잦은 반복)에서의 체감, 세 번째 이후 회복하지 못한 채 6초 넘게 살아 있을 때의 덤프(Decision 2의 둘째 갈래), 하트비트가 계속 진행된 뒤의 비정상 종료 기록.

## References
- ADR-0017 (`0017-second-fault-while-filtering.md`)
- ADR-0028 (`0028-whole-mod-review-2026-11.md`, 판정 3·4)

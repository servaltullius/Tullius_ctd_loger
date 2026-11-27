# ADR-0038: 기록된 크래시 없이 끝난 게임은 Windows가 적은 오류 위치를 Helper 로그에 옮긴다

## Status
Accepted (2026-11-27, 2판). 사용자가 ADR-0037과 함께 고른 방향입니다. 1판은 Gemini 4 Neon 설계 검토에서 보류였고(Codex 사용 한도로 Neon), 이 판은 그 지적을 반영했습니다. 구현했고 자동 테스트와 이 PC의 실제 이벤트로 확인했습니다(아래 "확인한 것"). 실게임의 새 종료는 미확인입니다.

## Context
- 게임이 0이 아닌 종료 코드로 끝났는데 플러그인이 크래시를 기록하지 못했으면, Helper는 종료 처리(`HelperMain.Process.cpp`의 `exitCode != 0 && !crashCaptured.latched` 갈래)에서 상황에 따라 다른 줄을 쓰고, 마지막 갈래에서는 WER LocalDumps 안내 파일을 씁니다(`EnableWerDumpFallbackHint=1`). 그때 Helper 로그에는 종료 코드 말고 단서가 없습니다.
- 그런 종료가 생기는 경우:
  - 우리보다 앞에 등록한 핸들러가 스택 오버플로에서 스택을 다 쓴 경우(ADR-0037의 계기).
  - 예외 컨텍스트가 커서 Windows가 어떤 핸들러도 부르지 못하는 호스트(ADR-0032 Decision 5).
  - 핸들러를 거치지 않는 fail-fast(`__fastfail`, 코드 `0xC0000409`): 벡터 핸들러도 최상위 필터도 불리지 않습니다.
- 이런 크래시도 Windows 오류 보고는 Application 로그에 이벤트 1000(공급자 `Application Error`)을 남깁니다. 2026-11-27 F9(PID 32884)의 이벤트 1000은 18:00:15에 기록됐고, Helper가 게임 종료를 본 것은 18:00:28이었습니다. 그 사이 게임은 스레드 하나만 남은 채였습니다(Helper 로그 "Hang detected while the game is closing (1 thread left…)"). 이벤트는 프로세스가 끝나기 전에 기록되었습니다.
- 그 이벤트의 XML(Windows 11 24H2, 이 PC): `<EventData>` 안에 `<Data Name='AppName'>SkyrimSE.exe</Data>`처럼 이름 붙은 값이 순서대로 있습니다: `AppName`, `AppVersion`, `AppTimeStamp`, `ModuleName`(ntdll.dll), `ModuleVersion`(10.0.26100.9974), `ModuleTimeStamp`, `ExceptionCode`(`c0000005`, 접두 없는 16진수), `FaultingOffset`(`0000000000055ae0`, 접두 없는 64비트 16진수), `ProcessId`(`0x8074`), `ProcessCreationTime`(`0x1dd7e4ce612c54a`, FILETIME), `AppPath`, `ModulePath`, `IntegratorReportId`, `PackageFullName`, `PackageRelativeAppId`.
- Windows 10에서는 같은 이벤트의 `<Data>`에 `Name`이 없고 같은 순서의 위치로만 들어 있다는 지적이 있습니다(확인하지 못함. 이 PC는 Windows 11).
- 이 위치는 프로세스가 끝난 곳이지 문제가 시작된 곳이 아닐 수 있습니다. 그 사례에서 ntdll의 위치는 힙 함수 첫머리였고, 넘친 스택을 다 쓴 핸들러의 다음 호출로 추정됩니다.
- Helper에는 이미 Windows 이벤트 로그를 읽는 코드가 있습니다(`helper/src/GpuDriverEvents.cpp`, ADR-0026: `EvtQuery`, `EvtRenderEventXml`, XML 도우미, 읽기 시간 예산과 건수 상한).

## Decision
1. **대상 갈래.** `exitCode != 0 && !crashCaptured.latched`에서 다음 셋에만 한 줄을 더합니다. 그 갈래가 지금 쓰는 줄과 안내 파일은 그대로 두고 그 뒤에 씁니다.
   - 종료 중 멈춤으로 본 갈래(`hangSkippedAsClosingThisEpisode && exitCode < 0xC0000000`).
   - 응답하지 않다가 끝난 갈래(`IsCloseOfUnresponsiveGame`). 다만 Windows가 응답 없음으로 닫은 경우(`kExitCodeClosedAsNotResponding`)는 빼냅니다. 그 경우는 이벤트 1002라 다루지 않습니다.
   - 기록된 크래시 없는 비정상 종료 갈래(마지막 `else`). `EnableWerDumpFallbackHint`가 0이어도 줄은 씁니다(그 설정은 안내 파일을 쓸지 정함).
   - 빼는 갈래: 덤프 쓰기에 실패한 크래시가 있는 갈래(`dumpWriteFailedCrashSeq != 0`), 종료 뒤 기록이 있는 갈래(`crashOnRecord`). 이미 기록된 크래시가 있습니다.
2. **이 게임의 이벤트만 씁니다.**
   - 조건: 이벤트의 `ProcessId`가 게임 PID와 같고, `ProcessCreationTime`이 게임 프로세스의 생성 시각(`GetProcessTimes`의 FILETIME, 64비트)과 같을 때. PID만으로는 재사용된 PID의 다른 프로세스를 잡을 수 있습니다. 생성 시각을 얻지 못했으면(0) 찾지 않습니다.
   - 값 읽기: 렌더링한 XML(`EvtRenderEventXml`, `GpuDriverEvents.cpp`의 도우미를 함께 씀)에서 `<Data>`를 순서대로 읽고, `Name`이 있으면 이름으로, 없으면 위 순서의 위치로 필드를 정합니다.
   - 수: `ProcessId`, `ProcessCreationTime`, `ExceptionCode`, `FaultingOffset`은 모두 16진수로 읽습니다(`0x`/`0X` 접두는 있어도 없어도 됨, 64비트, 앞의 0 허용). 10진수로 읽지 않습니다(`8074`가 16진수인 표기를 10진수로 읽으면 다른 PID와 맞아 버림). 읽을 수 없는 값이면 그 이벤트는 쓰지 않습니다.
3. **찾는 방법과 때.**
   - 질의: Application 채널, `Provider[@Name='Application Error'] and EventID=1000`, 최근 10분, 최신부터(`EvtQueryReverseDirection`). 한 번 찾을 때 최대 200건, 읽기 시간 500ms, `EvtNext` 시간 제한을 둡니다(`GpuDriverEvents.cpp`와 같은 방식). `EvtNext`는 한 번에 1건씩 받습니다. 여러 건을 달라고 하면 이벤트 로그 서비스가 그만큼 채우려고 오래된 이벤트까지 훑을 수 있기 때문입니다. `EvtNext`가 끝(`ERROR_NO_MORE_ITEMS`)이나 시간 제한(`ERROR_TIMEOUT`)이 아닌 오류로 실패하면 질의 실패와 같이 다룹니다.
   - 종료를 본 때 한 번 찾습니다. 못 찾았고 종료 코드가 NTSTATUS 오류(`0xC0000000` 이상)이면, 1초 뒤 한 번 더 찾습니다(이벤트가 늦게 기록되는 경우를 위한 여유, 총 지연 최대 약 2초). 그 밖의 종료 코드(`exit(1)`, Crash Logger의 종료 코드 1 등)는 다시 찾지 않습니다.
   - `EvtQuery` 자체가 실패하면(이벤트 로그 서비스 꺼짐, 권한, Wine) 다시 찾지 않고 넘어갑니다.
   - 이 지연은 Helper의 종료 처리를 늦춥니다. 그 갈래 뒤에 프리징 덤프의 뷰어(`LaunchDeferredViewersAfterExit`)가 있으면 그만큼 늦게 뜹니다.
4. **줄의 내용(영어, Helper 로그의 다른 줄과 같이).** Windows가 적었다는 것, 예외 코드, 모듈 이름과 버전, 오프셋, 그리고 "프로세스가 끝난 위치이지 원인이 아닐 수 있다"는 말.
   - 예: `Windows Error Reporting recorded this exit (Application log, event 1000): exception 0xc0000005 in ntdll.dll 10.0.26100.9974 at offset 0x55ae0. That is where the process ended, not necessarily where the problem began.`
   - 표기: 예외 코드는 `0x` + 8자리 소문자 16진수, 오프셋은 `0x` + 앞의 0을 뺀 소문자 16진수. 모듈 이름은 경로 구분자 뒤의 이름만, 제어 문자는 뺍니다(`&#13;` 같은 숫자 문자 참조로 들어온 것도. `GpuDriverEvents`는 지금처럼 공백으로 바꿈). 버전이 비었거나 `0.0.0.0`이면 버전을 뺍니다. 모듈 이름이 비었으면 `unknown module`로 씁니다. 경로(`AppPath`, `ModulePath`), 컴퓨터 이름, 사용자 SID는 옮기지 않습니다.
5. **못 찾으면 조용히 넘어갑니다.** 이벤트가 없음, 질의 실패, 값이 빠진 이벤트는 모두 줄을 쓰지 않습니다. 조회 중 예외가 나도 줄 없이 넘어가고, 그 뒤의 종료 처리(ETW 정리, 뷰어, 세션 로그 사본)는 그대로 합니다.
   - 조회는 테스트에서 바꿔 끼울 수 있습니다(`SetWerExitEventLineLookupOverrideForTests`). 제품 기본값은 실제 Application 로그 조회입니다.
6. **플러그인, 분석기, 뷰어, 공유 메모리, 설정은 바꾸지 않습니다.** 새 설정도 없습니다.

## 완료 조건과 검증
1. 이벤트 XML에서 필드를 읽고, PID·생성 시각으로 이 게임의 것을 고르고, 줄을 만드는 부분은 Windows API 없이 시험할 수 있는 함수로 둔다.
   - 검증(단위 테스트): 위 2026-11-27 이벤트 XML(컴퓨터 이름, SID, 경로는 가짜 값)로 정확한 줄. 같은 값을 `Name` 없는 위치 기반 `<Data>`로 바꾼 XML도 같은 줄. PID는 같고 생성 시각이 다른 이벤트, PID가 다른 이벤트, 16진수만으로 된 PID 표기(`8074`를 10진수로 읽지 않음), `0x` 유무, 64비트 오프셋(모듈 밖의 큰 주소), 작은 예외 코드(`0x00000001`), 빈 버전과 `0.0.0.0`, 경로나 제어 문자가 섞인 모듈 이름, 값이 빠진 이벤트, 생성 시각 0.
2. Windows에서 이벤트를 읽는 부분은 실패해도 Helper 종료 처리를 막지 않고, 질의가 실패하면 다시 찾지 않는다.
   - 검증: 질의 실패를 흉내 낸 경로에서 줄이 없고 지연이 없다는 테스트. 실제 `HandleProcessExitTick`을 돌리는 런타임 테스트로, 대상 세 갈래에서 조회가 불리고 줄이 남으며(`EnableWerDumpFallbackHint=0`에서도), 대상이 아닌 갈래(종료 코드 0, 잡아 둔 크래시, `crashOnRecord`, `dumpWriteFailedCrashSeq`, `0xCFFFFFFF`)에서는 불리지 않는다. 조회가 예외를 던져도 종료 처리가 끝까지 간다. 기존 Helper 런타임 테스트는 시작할 때 가짜 조회를 설치해 실제 이벤트 로그를 읽거나 기다리지 않는다(기대값은 그대로). `GpuDriverEvents`의 기존 테스트 통과. Windows 테스트 전체 통과, Linux CI.
3. 실제 이벤트로 한 번 확인한다: 개발 PC의 Application 로그에 남은 2026-11-27 18:00:15 이벤트를 읽는 수동 확인(자동 테스트에는 넣지 않음. 로그가 돌면 사라지고 CI에는 없음). 실게임의 새 종료에서 줄이 남는 것은 사용자 확인 전까지 미확인. Windows 10의 위치 기반 XML은 실제 이벤트로 확인하지 못함으로 남긴다.
4. 문서: `README.md`와 `docs/README_KO.md`의 출력 위치 설명에 한 줄, `docs/adr/README.md` 목록. CHANGELOG와 릴리즈 노트는 다음 릴리즈를 낼 때 쓴다.

## 확인한 것 (2026-11-27, Windows 11 이 PC)
- 단위 테스트(`skydiag_wer_exit_event_tests`, Linux에서도 빌드·실행): 2026-11-27 이벤트 XML(가짜 컴퓨터 이름·SID·경로)과 같은 값의 위치 기반 XML이 같은 줄을 냅니다. 큰따옴표 `Name="…"`, 순서가 다른 이름 붙은 값, 9개 미만의 위치 기반 값, PID·생성 시각 불일치, 접두 없는 16진수 PID, 생성 시각 0, 64비트 오프셋, 작은 예외 코드, 빈 버전과 `0.0.0.0`, 경로·제어 문자·숫자 문자 참조가 섞인 모듈 이름, 값이 빠진 이벤트, 질의 실패와 다시 찾기 규칙을 확인합니다.
- 런타임 테스트(`skydiag_wer_exit_runtime_tests`): 위 완료 조건 2의 갈래와 예외 경우. 조회 블록을 일부러 마지막 `else` 안으로 옮기면 종료 중 멈춤 갈래의 테스트가 실패했습니다.
- 실제 이벤트: 이 PC의 Application 로그에 남은 2026-11-27 18:00:15 이벤트(PID `0x8074`)를 테스트 실행 파일의 `--manual-check`로 읽어, Decision 4의 예와 같은 줄이 나왔습니다(조회 1ms).
- 기존 Helper 런타임 테스트는 가짜 조회를 설치한 뒤 오히려 빨라졌습니다(`helper_hang_runtime_tests` 37.8초 → 28.5초).
- Windows 테스트 전체 통과, Linux(WSL, CI와 같은 구성) 테스트 전체 통과.

## 한계
- Windows 오류 보고가 꺼져 있거나 이벤트를 남기지 않는 종료(`TerminateProcess`로 끝냄, 응답 없음으로 닫힘)에서는 줄이 없습니다.
- 줄의 위치는 프로세스가 끝난 곳입니다. 스택 오버플로를 다 쓴 다른 핸들러처럼, 원인과 다른 모듈(ntdll 등)을 가리킬 수 있습니다. 줄에 그렇게 적습니다.
- 이벤트가 종료 뒤 2초보다 늦게 기록되면 줄이 없습니다.

## Alternatives Considered
- **`EvtCreateRenderContext`로 이름 붙은 값 읽기(1판).** 값에 이름이 없는 Windows에서는 모든 값이 비어 조용히 실패합니다. XML을 읽어 이름과 위치를 함께 봅니다.
- **WER 보고서 파일(`ReportArchive`의 `Report.wer`)을 읽기.** 더 많은 값이 있지만 폴더 위치와 보관이 설정에 따라 다르고, 이벤트 1000에 필요한 값이 다 있습니다.
- **분석기·리포트에 넣기.** 덤프가 없는 종료라 리포트가 만들어지지 않습니다. Helper 로그가 그 종료의 유일한 기록입니다.

## References
- ADR-0026(그래픽 드라이버 이벤트 읽기), ADR-0032 Decision 5, Addendum 2026-11-27; ADR-0037
- `helper/src/HelperMain.Process.cpp` (종료 처리와 WER 안내), `helper/src/CrashCapture.cpp` (`WriteWerFallbackHint`), `helper/src/GpuDriverEvents.cpp`

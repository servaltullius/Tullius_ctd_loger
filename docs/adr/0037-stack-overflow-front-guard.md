# ADR-0037: 스택 오버플로는 맨 앞의 가드가 기록하고 덤프가 끝날 때까지 붙잡는다

## Status
Accepted (2026-11-27, 2판). 사용자가 방향(가드와 ADR-0038의 WER 줄, 스레드 스택 여유는 건드리지 않음)을 골랐습니다. 1판은 Gemini 4 Neon 설계 검토에서 보류였고(Codex 사용 한도로 Neon), 이 판은 그 지적을 반영했습니다(아래 "1판에서 바뀐 것"). 구현했고 자동 테스트로 확인했습니다(아래 "확인한 것"). 실게임은 미확인입니다.

## Context
- 2026-11-27 v0.2.80-rc3의 Ctrl+Shift+F9(크래시 테스터, 메인 스레드에서 끝없는 재귀)는 덤프도 Crash Logger 로그도 남기지 않았습니다(ADR-0032 Addendum 2026-11-27).
  - 확인한 것: 업스케일러 모드에 딸린 프레임 생성 DLL(`nvngx_dlssg.dll`)이 `AddVectoredExceptionHandler(1, …)`로 핸들러를 맨 앞에 등록합니다. 플러그인은 SKSE 플러그인을 불러올 때 맨 앞에 등록하므로, 그 뒤에 등록한 이 핸들러가 플러그인의 핸들러보다 먼저 불립니다(같은 날 Ctrl+Shift+Delete에서 그 DLL의 미니덤프가 끝난 뒤에야 Helper가 덤프를 시작함).
  - 그 DLL은 플러그인이 로드되고 약 13초 뒤에 올라왔습니다(2026-11-27 20:00 세션: `SkyrimDiag: loaded` 20:00:42, 그 DLL의 로그 시작 20:00:55). `kDataLoaded`(`SkyrimDiag: data loaded`)는 20:02:34였습니다. 19:53 세션도 같은 순서였습니다.
  - 추정: 그 핸들러가 넘친 스택에 남은 여유를 다 써서 게임이 끝났습니다. Windows 이벤트 1000은 ntdll.dll+0x55ae0의 `0xC0000005`를 적었습니다.
- 스택 오버플로에서 핸들러가 쓸 수 있는 스택: 이 PC에서 핸들러가 불릴 때 7,552\~9,744바이트(ADR-0032 Addendum). 보통 호스트에서 예외 컨텍스트를 뺀 나머지는 8,320\~10,496바이트지만(`tests/plugin_crash_handler_runtime_tests.cpp`의 측정 주석) Windows 디스패처의 프레임을 빼기 전 값입니다. 이 ADR의 기준은 7,552바이트입니다.
- 지금의 스택 오버플로 경로(ADR-0032 Decision 1·2): `VectoredHandler`가 1차 예외에서 크래시 기록을 쓰고 크래시 이벤트를 켠 뒤 바로 다음 핸들러로 넘깁니다. 최상위 필터는 스택 오버플로를 알리지도 붙잡지도 않습니다. Helper는 크래시 이벤트를 받으면 덤프를 먼저 쓰고(약 1.4초), 크래시 덤프 쓰기가 끝날 때마다 `DUMP_SETTLED`를 켭니다. Helper에는 스택 오버플로만 따로 다루는 곳이 없습니다(`helper`, `shared`, `common`에 `0xC00000FD`를 다루는 코드 없음, 대소문자 무시).
- 그래서 우리보다 앞에 선 핸들러가 넘친 스택을 다 쓰면, 우리 핸들러는 불리지도 않고 게임이 끝납니다. 기록이 된다 해도 Helper가 덤프를 시작하기 전에 프로세스가 끝날 수 있습니다.
- `DUMP_SETTLED`는 Helper가 만들고(`CreateEventW`, 수동 리셋, 처음 꺼짐) 플러그인은 붙잡을 때마다 열고 닫습니다. 열 수 있다는 것이 곧 Helper가 살아 있다는 뜻입니다(ADR-0032 Decision 2, 테스트 `HelperSettle::kNoEvent`). 플러그인이 핸들을 들고 있으면 Helper가 죽어도 이벤트가 남아 이 계약이 깨집니다.
- SKSE 메시지 중 `kDataLoaded`는 메인 스레드가 아니라 로더 스레드에서 옵니다(`PluginMain.cpp`의 ADR-0022 주석, `tests/plugin_runtime_guard_tests.cpp`).

## Decision
1. **가드: 스택 오버플로만 다루는 벡터 핸들러를 하나 더 둡니다(`StackOverflowFrontGuard`).**
   - `InstallCrashHandler`가 `VectoredHandler`를 등록한 다음 `AddVectoredExceptionHandler(1, …)`로 등록합니다. 그래서 처음부터 `VectoredHandler`보다 앞에 서고, 다시 올려도(Decision 4) 늘 `VectoredHandler`보다 앞입니다(`InstallCrashHandler`는 플러그인을 불러올 때 한 번만 불림. `VectoredHandler`의 건너뛰기는 위치가 아니라 가드가 설치됐는지로 판단). `CrashHookMode`가 1과 2일 때만 둡니다(스택 오버플로를 1차 예외에서 기록하는 두 모드). 등록에 실패하면 지금과 같습니다.
   - 들어오면 가드 안의 스레드 수 카운터를 하나 늘리고, 나갈 때 줄입니다(Decision 4가 실행 중인 등록을 지우지 않으려는 것). 예외 하나에 원자 연산 두 번이고, 예외 전달 자체의 비용에 비하면 작습니다. 그 밖에 코드가 `EXCEPTION_STACK_OVERFLOW`가 아니면 공유 메모리도 다른 상태도 읽거나 쓰지 않고 `EXCEPTION_CONTINUE_SEARCH`를 돌려줍니다. 다른 코드의 순서는 바꾸지 않습니다. 앞자리 핸들러가 고쳐서 이어 가게 하던 예외를 우리가 새로 기록하게 되기 때문입니다.
2. **가드가 스택 오버플로를 지금과 같은 방법으로 기록하고, `VectoredHandler`는 가드가 있으면 스택 오버플로를 건너뜁니다.**
   - 가드는 `VectoredHandler`의 본문(지금 `VectoredHandler`가 하는 일 그대로)을 부릅니다. 기록 내용(v4 크래시 기록, 이벤트 페이로드)은 바뀌지 않습니다. 등록되는 `VectoredHandler`는 "가드가 설치돼 있고 코드가 스택 오버플로면 바로 `EXCEPTION_CONTINUE_SEARCH`"를 맨 앞에 더하고 나머지는 본문을 그대로 부릅니다. 가드는 늘 `VectoredHandler`보다 앞이므로, 가드가 설치돼 있으면 스택 오버플로를 기록할 쪽은 가드뿐입니다.
   - 따로 중복 방지 표를 두지 않습니다. 다시 올리는 순간(Decision 4) 가드가 체인에 두 번 있어 같은 예외에 두 번 불리면, 두 번째의 `TryPublishCrashRecord`는 첫 번째가 쥔 `kState_Frozen`에 막혀 실패하고, 기록하지 못했으므로 붙잡지도 않습니다. 스택 오버플로에서는 실패한 기록의 표시(`NoteFaultRecorded(false)`)가 쓰이는 곳이 없습니다(최상위 필터와 보조 덤프가 스택 오버플로를 다루지 않음). 그 사이 Helper가 덤프 실패로 `Frozen`을 풀었다면 같은 예외가 한 번 더 기록될 수 있는데, 두 등록이 함께 있는 것은 몇 마이크로초뿐입니다.
3. **이 호출이 크래시 기록을 쓴 경우에만, 가드는 Helper가 덤프를 끝낼 때까지 스레드를 붙잡습니다.**
   - "이 호출이 썼다": 본문을 부르기 전과 뒤의 `crash_seq`가 다르고, 기록이 이 스레드·이 코드·이 주소이며 `kState_Frozen`이 잡혀 있을 때(ADR-0032 필터의 `matchedFrozen`과 같은 판단).
   - 기다리는 대상은 `DUMP_SETTLED`입니다. 필터처럼 그때 `OpenEventW`로 열고 기다린 뒤 닫습니다. 열 수 없으면(Helper 없음, 죽음, 예전 Helper) 붙잡지 않습니다. 넘친 스택에서 `OpenEventW`와 `WaitForSingleObject`가 쓰는 스택은 완료 조건 2로 잽니다.
   - 상한은 `min(UnhandledCrashHoldMs, 5000)`입니다(기본 5000, 0이면 붙잡지 않음). 5초를 넘기지 않는 것은, Helper가 덤프보다 확인을 먼저 하는 경로에서 하트비트 확인이 끝나기 전에 풀기 위해서입니다. 스택 오버플로가 그 경로에 드는 것은 ADR-0031의 반복 예외뿐입니다(Crash Logger 스레드 덤프 단축키 뒤 1분의 경로는 C++ 예외와 중단점만 해당, `IsInCrashLoggerThreadDumpWindow`). 그 확인은 2초 간격으로 최대 네 번이고 하트비트가 두 번 전진해야 회복으로 보며, 세 번 연속 전진이 없으면 6초에 끝냅니다(`CrashCapture.cpp`의 `kHeartbeatCheckIntervalMs`, `kMaxHeartbeatChecks`, `kRequiredHeartbeatAdvances`). 5초에 풀면 하트비트가 다시 전진할 여유가 1초 남습니다. 메인 스레드를 그보다 오래 붙잡으면 가드가 멈춘 하트비트를 만들어, 처리될 예외도 Helper가 크래시로 판정합니다.
   - 최상위 필터의 "처리되지 않음" 신호는 켜지 않습니다. 이 시점에는 처리될지 모르기 때문입니다.
   - 다른 사고가 `kState_Frozen`을 쥐고 있어 기록하지 못했으면 붙잡지 않습니다(지금과 같음. 스택 오버플로는 ADR-0036의 보조 덤프 후보도 아님).
   - 붙잡기가 끝나면 다음 핸들러로 넘깁니다. 앞자리 핸들러가 남은 스택을 다 써서 프로세스가 끝나도 그 전에 덤프가 써져 있습니다. Helper는 그 뒤의 종료 코드(예: `0xC0000005`)를 지금처럼 0이 아닌 종료로 보고 크래시로 남깁니다(`ClassifyExitCodeVerdictWithContext`).
4. **가드를 다시 맨 앞에 올립니다.**
   - 때: SKSE 메시지 `kPostLoad`, `kPostPostLoad`, `kInputLoaded`, `kDataLoaded`, `kPostLoadGame`, `kNewGame`. 메시지를 받는 스레드는 정해져 있지 않습니다(`kDataLoaded`는 로더 스레드). 근거가 된 DLL은 `kDataLoaded`보다 1분 반 앞에 올라왔으므로 `kDataLoaded`에서 잡히고, 그보다 이른 메시지는 그 앞의 빈틈을 줄입니다(그 DLL보다 앞인지 뒤인지는 확인하지 못함).
   - 가드가 설치돼 있을 때만 합니다(`CrashHookMode` 1·2이고 `InstallCrashHandler`가 가드를 등록함). `CrashHookMode=0`에서는 아무것도 하지 않습니다.
   - 방법: 잠금(`std::mutex`, 예외 경로가 아님) 안에서, 가드 안의 스레드 수가 0일 때만 새로 `AddVectoredExceptionHandler(1, 가드)`를 하고 그다음 앞의 등록을 `RemoveVectoredExceptionHandler`로 지웁니다. 0이 아니면 `SwitchToThread`를 하며 여덟 번까지 다시 보고(다른 예외가 가드를 잠깐 지나가는 중일 수 있음), 그래도 0이 아니면 건너뜁니다. 붙잡힌 스레드는 몇 초 동안 가드 안에 있을 수 있기 때문입니다. 새 등록에 실패하면 앞의 등록을 그대로 둡니다.
   - 카운터를 본 뒤 지우기 전에 다른 스레드가 앞의 등록으로 들어오는 틈은 남습니다. `RemoveVectoredExceptionHandler`가 실행 중인 항목을 어떻게 다루는지는 문서에 없습니다(Windows와 Wine의 ntdll은 항목을 실행하는 동안 참조를 세는 것으로 알려져 있으나 확인하지 못함). 그래서 실행 중인 항목을 지우는 자식 프로세스 테스트로 확인합니다(완료 조건 4). 실패하면 이 설계를 다시 정합니다.
   - `SkyrimDiag.log`: `kDataLoaded`에서 한 번, 다시 올렸는지(또는 건너뛴 이유, 등록 실패)를 적습니다. 그 밖의 때는 실패만 적습니다.
5. **Helper, 분석기, 뷰어, 공유 메모리 배치는 바꾸지 않습니다.** 프로토콜 v4 그대로입니다. Helper가 예전 버전이면 `DUMP_SETTLED`가 없어 붙잡지 않으므로 지금과 같습니다.

## 완료 조건과 검증
1. **실패하는 테스트를 먼저 만든다.** 자식 프로세스 테스트에서 `InstallCrashHandler` 뒤에, 스택 오버플로를 받으면 큰 지역 배열(64KB 이상, 모든 페이지를 건드림)을 쓰는 다른 핸들러를 `AddVectoredExceptionHandler(1, …)`로 등록하고, 메인 스레드에서 스택 오버플로를 낸다. 스택 여유를 늘리는 호스트(AMX, `KeepStackForExceptionContext`)에서도 그 핸들러가 남은 스택을 넘도록 크기를 정한다.
   - 검증: 지금 코드에서 크래시 기록과 크래시 이벤트가 오지 않아 테스트가 실패한다. 재현되지 않으면 이 설계를 멈추고 원인부터 다시 본다.
2. **다시 맨 앞에 올린 가드가 기록하고 붙잡는다.** 같은 자식에서 다른 핸들러를 등록한 뒤 가드를 다시 맨 앞에 올리면, 크래시 기록(`0xC00000FD`)과 이벤트가 오고, 부모(Helper 역할)가 `DUMP_SETTLED`를 켤 때까지 자식이 살아 있으며, 켠 뒤 다른 핸들러가 불린다.
   - 스택: 가드에 들어올 때의 RSP부터 가드와 그 아래 호출(`OpenEventW`, `WaitForSingleObject`)이 실제로 건드린 가장 깊은 곳까지를 잰다(새로 커밋된 스택 페이지는 0으로 채워지므로, 스택 한계에서 위로 처음 0이 아닌 바이트로 잼). 7,552바이트보다 충분히 작아야 한다(4,096바이트 이하). 잰 값은 테스트 출력과 보고에 남긴다.
3. **다른 경우.**
   - Helper가 끝내 켜지 않으면 상한에서 풀린다. 상한 0, 이벤트 없음(Helper 없음)이면 붙잡지 않는다. `UnhandledCrashHoldMs`가 5000보다 커도 가드는 5초에서 푼다.
   - 스택 오버플로가 아닌 코드에서 가드는 공유 메모리를 건드리지 않는다.
   - 가드가 두 번 등록된 채 스택 오버플로가 나도 기록은 한 번이다.
   - `kState_Frozen`을 다른 사고가 쥔 상태의 스택 오버플로는 기록도 붙잡기도 하지 않는다.
   - 같은 스레드에서 스택 오버플로를 처리하고(`__except`, `_resetstkoflw`) 다시 스택 오버플로를 내면 두 번째도 기록된다(첫 사고가 풀린 뒤).
   - `CrashHookMode=0`에서 다시 올리는 함수는 아무것도 하지 않고, `CrashHookMode=2`에서도 가드가 동작한다.
   - Helper가 사라진 뒤(이벤트를 닫음) ADR-0032 필터와 가드가 붙잡지 않는다(플러그인이 이벤트를 붙들지 않음).
   - 기존 `TestUnhandledStackOverflowIsNotHeld`의 확인(최상위 필터가 스택 오버플로를 알리지 않음, 필터 아래의 필터가 프로세스를 끝냄)은 그대로 둔다.
   - `tests/plugin_runtime_guard_tests.cpp`의 `VectoredHandler` 소스 검사는 새 구조에 맞게 고치고, 가드가 힙, `SetUnhandledExceptionFilter`, `RecordCloseRequestAtCrash`, `GetModuleHandleW`, 로그를 쓰지 않는다는 소스 검사를 더한다.
   - 검증: Windows 테스트 전체 통과(지금 129개 + 새 테스트), Linux CI.
4. **실행 중인 항목 지우기.** 한 스레드가 가드 안에서 붙잡혀 있는 동안 다른 스레드가 가드를 다시 올리고(앞의 등록을 지움), 붙잡힌 스레드가 풀려 다음 핸들러로 넘어가도 프로세스가 무사하다. 이 경우 카운터 때문에 지우지 않아야 하므로, 테스트는 카운터를 거치지 않고 지우는 경로(테스트 전용 진입점)로 실제 ntdll의 동작을 본다.
   - 검증: 자식 프로세스 테스트(Windows). Wine은 확인하지 못함으로 남긴다.
5. **문서.** ADR-0032 Decision 1의 "스택 오버플로는 붙잡지 않는다"에 이 ADR을 가리키는 줄을 더하고, `docs/adr/README.md` 목록에 넣는다. CHANGELOG와 릴리즈 노트는 다음 릴리즈를 낼 때 쓴다.
6. **실게임(사용자 확인, 그 전까지 미확인).** 프레임 생성 DLL을 켠 같은 구성에서 Ctrl+Shift+F9를 누르면 `0xC00000FD` 크래시 덤프와 리포트가 남는다. F3, F5, PageDown, Delete는 전처럼 기록된다.

## 확인한 것 (2026-11-27, 자동 테스트, Windows 11 이 PC)
- 실패 먼저: 64KB 넘게 스택을 쓰는 다른 핸들러(2KB 프레임 48단계)를 `InstallCrashHandler` 뒤 맨 앞에 등록하고 스택 오버플로를 내자, 가드 구현 전 코드에서는 크래시 기록이 오지 않고(`crash_seq=0`) 자식이 `0xC0000005`로 끝났습니다. 2026-11-27 실게임 F9와 같은 모습입니다. 한 번에 96KB 지역 배열을 쓰면 `__chkstk`의 첫 탐침이 접근 위반으로 끝나 재현이 되지 않아, 프레임을 나눴습니다.
- 가드를 다시 올린 뒤: 기록(`0xC00000FD`)과 크래시 이벤트가 오고, 부모가 `DUMP_SETTLED`를 켤 때까지 자식이 살아 있으며 다른 핸들러는 그 뒤에 불렸습니다. 스택 여유를 늘린 경로(`-kept-back`)도 같았습니다.
- 넘친 스택에서 가드와 그 아래 호출(`OpenEventW`, `WaitForSingleObject`)이 쓴 스택: 1,032바이트(두 경로 모두).
- 실행 중인 등록 지우기: 가드 안에서 붙잡힌 스레드가 있는 동안 보통 다시 올리기는 건너뛰었고(`kSkippedBusy`), 카운터를 무시하고 앞의 등록을 지운 뒤 풀어 줘도 프로세스는 다음 핸들러와 최상위 필터까지 무사히 진행했습니다. Wine은 확인하지 못했습니다.
- 대조 실행: 같은 핸들러를 등록하고 가드를 다시 올리지 않으면 기록이 없고(`crash_seq=0`) 자식이 그 핸들러 때문에 끝나는 것을 스위트에 남겼습니다. 가드를 다시 올린 경우에도 풀린 뒤 그 핸들러가 남은 스택을 넘겨 프로세스를 끝내는 것(필터 아래까지 가지 않음)을 확인합니다.
- 두 번 등록: 상한 1.5초에 Helper가 답하지 않게 두고, 크래시부터 종료까지가 1.2초 이상 2.6초 미만인지 확인합니다(두 번째 등록이 다시 붙잡으면 약 3초). 기록은 한 번입니다.
- `VectoredHandler`의 건너뛰기: 가드가 붙잡는 동안 부모가 `kState_Frozen`을 풀고 크래시 이벤트를 끈 뒤 풀어 줘도 기록이 다시 생기지 않았습니다.
- Helper가 사라진 뒤: 부모가 `DUMP_SETTLED`를 끄고 닫은 뒤의 스택 오버플로(가드)와 처리되지 않은 접근 위반(ADR-0032 필터)이 붙잡히지 않았습니다.
- 일부러 망가뜨려 본 것: 가드에서 `CloseHandle`을 빼면(플러그인이 이벤트를 붙듦) Helper가 사라진 뒤의 테스트가, `VectoredHandler`의 건너뛰기를 빼면 건너뛰기 테스트가 실패했습니다.
- 그 밖에 완료 조건 3의 나머지 경우(상한, 상한 0, Helper 없음, 상한 5초 묶음, 스택 오버플로가 아닌 예외, `Frozen` 점유 중, 처리 뒤 두 번째 스택 오버플로, `CrashHookMode` 0과 2)를 자식 프로세스 테스트로 확인했습니다. Windows 테스트 129개 전체 통과, Linux(WSL, CI와 같은 구성) 테스트 전체 통과.
- 스택 사용량은 RelWithDebInfo 빌드, Crash Logger가 없는 자식에서 잰 값입니다. 다음 핸들러 안에서 훑어 재므로 가드와 그 핸들러가 쓴 것 중 깊은 쪽입니다.

## 한계
- Helper가 덤프보다 확인을 먼저 하는 경로(ADR-0031의 반복 예외)에서는 Helper가 하트비트 확인 뒤에 덤프를 쓰므로 가드가 먼저 풀립니다. `HeartbeatIntervalMs`를 1초보다 크게 두었거나 풀린 직후 1초 넘게 끊기면, 처리될 스택 오버플로를 Helper가 크래시로 판정할 수 있습니다. 그때도 크래시 기록은 있으므로 종료 뒤의 기록(`SkyrimDiag_PostExitCrashEvidence_*.json`)은 남습니다. 그 경로에서 Helper가 "처리됨"으로 풀어 줄 때는 `DUMP_SETTLED`를 켜지 않으므로, 처리될 스택 오버플로라도 가드가 상한(최대 5초)까지 그 스레드를 붙잡습니다. 같은 주소에서 처리된 스택 오버플로가 세 번째로 나는 일은 드뭅니다.
- 마지막으로 다시 올린 뒤에 맨 앞에 등록한 핸들러는 다음 때까지 가드보다 앞에 섭니다.
- 앞자리 핸들러가 스택 오버플로가 아닌 예외에서 프로세스를 끝내는 경우(자기 덤프를 쓰고 `TerminateProcess`)는 바뀌지 않습니다.
- 붙잡힌 뒤 앞자리 핸들러가 게임을 끝내면 Crash Logger 로그는 여전히 남지 않습니다. 스레드 스택 여유(`SetThreadStackGuarantee`)는 사용자 결정으로 건드리지 않습니다(ADR-0032 Decision 5와 같음).
- 예외 컨텍스트가 커서 Windows가 어떤 핸들러도 부르지 못하는 호스트(ADR-0032 Decision 5의 AMX)는 바뀌지 않습니다. 그때의 단서는 ADR-0038의 WER 줄입니다.
- 게임이 스택 오버플로를 스스로 처리하는 드문 경우, 덤프를 먼저 쓰는 지금 경로와 같이 덤프 한 번(약 1.4초 멈춤)이 들고, 그 시간만큼 스레드가 붙잡힙니다.
- 가드의 등록은 플러그인이 끝날 때까지 남습니다(플러그인은 내려가지 않음).
- Crash Logger가 보관된 C++ 예외나 중단점을 보고하는 중에 스택 오버플로가 나서, 본문이 그 보관 예외를 기록한 경우에는 기록이 이 스택 오버플로의 것이 아니므로 가드가 붙잡지 않습니다(드묾).
- 가드 안에서 다른 예외가 나고 그 예외가 가드 바깥의 `__except`까지 되감기면 카운터가 줄지 않아, 그 뒤의 다시 올리기는 늘 건너뜁니다(드묾).

## Alternatives Considered
- **`VectoredHandler` 전체를 맨 앞에 다시 올리기.** 모든 치명적 코드에서 앞자리 핸들러보다 먼저 서게 되어, 앞자리 핸들러가 고쳐서 이어 가게 하던 예외(지금은 우리가 보지 못함)를 새로 기록합니다. 그때마다 Helper가 덤프를 먼저 써 게임이 1초 남짓 멈춥니다(ADR-0031이 줄인 멈춤). 스택 오버플로만 앞에 세웁니다.
- **`DUMP_SETTLED` 핸들을 미리 열어 두기(1판).** 넘친 스택에서 `OpenEventW`를 부르지 않아도 되지만, 플러그인이 핸들을 쥐면 Helper가 죽어도 이벤트가 남아 ADR-0032 필터까지 Helper 없이 붙잡게 되고, 켜진 채 죽은 이벤트를 다시 뜬 Helper가 그대로 받습니다.
- **같은 예외를 두 번 다루지 않는 표(1판).** 표를 언제 비우는지에 따라 같은 자리에서 다시 난 스택 오버플로를 놓치거나 두 번 기록합니다. `kState_Frozen`이 같은 일을 이미 합니다.
- **일정 간격으로 다시 올리기.** 가장 넓게 잡히지만 세션 내내 등록을 바꾸게 됩니다. 확인한 사례에는 SKSE 메시지 때로 충분합니다.
- **DLL 로드 알림(`LdrRegisterDllNotification`) 뒤에 다시 올리기.** 핸들러를 DLL을 불러온 뒤에 등록하는 모듈이 있어 로드 시점이 등록 시점이 아닙니다.
- **게임 메인 스레드의 스택 여유 늘리기(`SetThreadStackGuarantee`).** Crash Logger 로그까지 남을 수 있지만 메인 스레드가 쓸 수 있는 스택이 그만큼 줄어듭니다. 사용자가 이번에는 넣지 않기로 했습니다.
- **Helper가 "처리됨"으로 풀 때도 `DUMP_SETTLED`를 켜기.** 확인이 먼저인 경로의 붙잡기를 줄이지만 Helper를 바꿔야 하고, 그 경로는 스택 오버플로에서 드뭅니다.

## 1판에서 바뀐 것 (Gemini 4 Neon 설계 검토, 2026-11-27)
- `DUMP_SETTLED`를 미리 열어 두지 않고 그때 엽니다(Helper 생존 계약).
- 중복 방지 표를 없애고, `VectoredHandler`가 가드가 있으면 스택 오버플로를 건너뜁니다.
- 붙잡기 상한을 5초로 묶었습니다(확인이 먼저인 경로의 가짜 크래시).
- `kDataLoaded`가 로더 스레드라는 것을 바로잡고, 다시 올리기에 잠금과 `CrashHookMode` 확인을 더했습니다. 다시 올리는 때를 더 이른 SKSE 메시지까지 넓혔습니다.
- 카운터를 코드 검사 앞에 두고, "지우지 않고 남기기" 대안을 뺐습니다(실행 중 지우기는 테스트로 확인).
- 스택 기준을 7,552바이트로 고치고, 실제로 건드린 깊이로 재게 했습니다. AMX 러너, 기존 소스 검사, Helper가 사라진 경우의 테스트를 더했습니다.

## References
- ADR-0032 (Decision 1·2·5, Addendum 2026-11-27), ADR-0031(확인이 먼저인 경로), ADR-0036(보조 덤프)
- `plugin/src/CrashHandler.cpp` (`VectoredHandler`, `InstallCrashHandler`, 필터 붙잡기), `plugin/src/PluginMain.cpp` (`OnSkseMessage`)
- `helper/src/CrashCapture.cpp` (`DUMP_SETTLED`, `ClassifyExitCodeVerdictWithContext`, `CheckRepeatedRecoveredFault`), `helper/src/ProcessAttach.cpp`
- `tests/plugin_crash_handler_runtime_tests.cpp` (`TestUnhandledStackOverflowIsNotHeld`, 스택 여유 측정 주석), `tests/plugin_runtime_guard_tests.cpp`

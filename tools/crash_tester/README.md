# Crash tester (개발용, 배포하지 않음)

원인을 미리 아는 CTD와 프리징을 실제 게임에서 일으켜, 로거의 분석이 그 원인을 짚는지 확인하는 SKSE 플러그인입니다. 릴리즈 zip에는 들어가지 않습니다.

## 빌드

```
cmake -S . -B build-ct -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSKYDIAG_CRASH_TESTER=ON -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build-ct --target CrashTester SkyrimCrashGuardStub
```

결과는 `build-ct/crash-tester/`에 생깁니다. 실제 모드처럼 PDB 없이 DLL만 설치합니다.

- `CrashTester.dll`: 단축키로 크래시·프리징을 일으킵니다.
- `SkyrimCrashGuard.dll`: 로드만 되고 아무것도 하지 않습니다. 문제 모듈 경고(ADR-0013)가 파일 이름으로 뜨는지 확인할 때만 켭니다.

## 단축키 (SKSE task로 실행)

작업은 SKSE `AddTask`로 실행되므로, 1.6.1170에서는 창·렌더를 맡는 첫 스레드가 아니라 게임의 작업 스레드(`BSJobs::JobThread`)에서 돌고, 첫 스레드는 매 프레임 그 작업을 기다립니다.

| 키 | 일어나는 일 | 기대하는 분석 |
|---|---|---|
| Ctrl+Shift+F1 | 조준한 레퍼런스(없으면 플레이어)를 RCX에 둔 채 CrashTester.dll 안에서 접근 위반 | 예외 모듈과 후보가 CrashTester.dll, 덤프 오브젝트 RCX에 그 레퍼런스의 이름과 기반 오브젝트 |
| Ctrl+Shift+F2 | CrashTester.dll 안에서 `int 3` | `0x80000003`, 모드가 스스로 멈춘 것이라는 해석, 후보 CrashTester.dll |
| Ctrl+Shift+F3 | CrashTester.dll에서 처리되지 않는 C++ 예외. 그동안 다른 스레드가 잡히는 C++ 예외를 계속 던짐 | C++ 예외(`0xE06D7363`), Crash Logger의 C++ 예외 정보와 대조. 다른 스레드의 잡힌 예외 때문에 크래시가 빠지지 않음(리뷰 #5) |
| Ctrl+Shift+F4 | 작업 스레드를 CrashTester.dll 안의 `Sleep`으로 45초 붙잡음(다시 누를 수 있음) | 자동 프리징 캡처, 막힌 작업 스레드의 CrashTester.dll을 후보로, 두 번째는 같은 `HANG1-` 버킷 |
| Ctrl+Shift+F5 | 게임 작업 스레드에서 잡히는 접근 위반. Helper가 첫 덤프 쓰기를 마친 뒤 2초가 지나면 CrashTester.dll 스레드에서 처리되지 않는 접근 위반 | 첫 예외에 기존 덤프와 판정이 유지되고, 두 번째 예외의 보조 덤프와 JSON이 남음(ADR-0036) |
| Ctrl+Shift+F6 | CrashTester.dll이 읽을 수 없는 메모리를 레퍼런스로 엔진의 `TESObjectREFR::GetDisplayFullName`에 넘김 | 예외 모듈 SkyrimSE.exe, 콜스택으로 CrashTester.dll을 후보로 |
| Ctrl+Shift+F7 | 저널 메뉴를 열어, 게임이 부르는 CrashTester.dll의 메뉴 이벤트 콜백 안에서 접근 위반 | 게임이 부른 플러그인 콜백의 크래시. 후보 CrashTester.dll, 그 아래는 게임의 이벤트 처리 프레임 |
| Ctrl+Shift+F8 | CrashTester.dll이 직접 만든 스레드에서 접근 위반 | 아래에 게임 프레임이 없는 스레드. 후보 CrashTester.dll |
| Ctrl+Shift+F9 | CrashTester.dll 안의 무한 재귀 | `0xC00000FD`(스택 오버플로), 후보 CrashTester.dll |
| Ctrl+Shift+F10 | 작업 스레드가 CrashTester.dll 안에서 45초 동안 CPU를 돌림 | 대기가 아니라 바쁘게 도는 프리징. 막힌 작업 스레드의 CrashTester.dll을 후보로 |
| Ctrl+Shift+F11 | 저널 메뉴를 열어, 메뉴 이벤트 콜백이 45초 동안 붙잡음 | 게임이 부른 플러그인 콜백의 프리징. 후보 CrashTester.dll |
| Ctrl+Shift+End | CrashTester.dll이 비어 있는(null) 함수 포인터를 `call`로 부름(설정되지 않은 콜백, vtable 칸, 후킹의 원래 함수를 부르는 경우) | `null_jump` 판정("비어 있는(null) 함수 포인터로 함수를 부르다가 튕겼습니다"). v0.2.76-rc3부터 콜스택이 `0x0`, `CrashTester.dll!CallEmptyFunctionPointer`, 게임의 작업 처리 프레임 순으로 이어지고 Crash Logger의 `PROBABLE CALL STACK`과 앞부분 순서가 같음(ADR-0019). 호출이 `call rax`(rax=0)로 빌드되므로 근거에 "호출한 곳은 CrashTester.dll+…(call rax)입니다"가 나옴. 후보 CrashTester.dll. 그 이전 분석기에서는 콜스택이 비고 스택 스캔으로만 짚음 |
| Ctrl+Shift+Home | CrashTester.dll이 폼 포인터 자리에 FormID `0x00012EB6`(Skyrim.esm의 철 방패, ArmorIronShield)을 넣고 폼 종류를 읽음(빌드 결과 `movzx ebx, byte ptr [rax+0x1a]`, rax=0x12EB6, 2026-11-17 디스어셈블리로 확인). 세이브를 불러오는 중에 인벤토리를 읽은 모드에서 실제로 본 모양 | `bad_pointer` 판정(rax=0x12EB6)에 더해, 근거 문장이 "이 값은 이 게임에 있는 폼 …의 FormID와 같습니다"로 끝나며 그 폼이 `TESObjectARMO "철 방패" (FormID 0x00012EB6, Skyrim.esm …)`로 나오고, 빠른 답의 어떻게 튕겼나가 "폼의 번호(FormID)를 포인터처럼 쓰다가 튕긴 것으로 보입니다", 리포트의 예외 명령어 줄 끝에 `(rax 값은 이 폼의 FormID: …)`가 붙음(ADR-0033). 이 조회를 하는 Helper로 캡처한 덤프에서만 나옴. 후보 CrashTester.dll |
| Ctrl+Shift+PageUp | CrashTester.dll이 C++ 예외(`std::runtime_error`)를 던지고 스스로 `catch`로 잡은 뒤, 그 `catch` 블록 안에서 접근 위반을 일으킴 | 진짜 크래시는 `0xC0000005`(접근 위반, 후보 CrashTester.dll). 현재(`IsCrashLoggerReportingKeptException`)는 `catch` 실행 중 스택 위에 남은 C++ 예외의 `CONTEXT`와 `CrashLogger.dll` 벡터 핸들러의 잔재 주소가 겹치면, 이미 잡힌 C++ 예외(`0xE06D7363`)로 오기록되고 ADR-0032 처리되지 않은 예외 신호도 켜지지 않는지 확인하는 용도. 고친 뒤에는 `0xC0000005`(후보 CrashTester.dll, 처리되지 않은 예외 신호 포함)로 기록돼야 함 |
| Ctrl+Shift+PageDown | F5와 같은 두 접근 위반을 CrashTester.dll의 같은 워커 스레드에서 서로 다른 주소에 일으킴 | 첫 덤프와 보조 덤프의 예외 스레드는 같고 주소는 다름(ADR-0036) |
| Ctrl+Shift+Insert | SkyrimSE.exe 안의 같은 주소에서 나는 접근 위반을 CrashTester.dll이 스스로 잡기를 15초 간격으로 6번 반복(홀수 번째는 게임의 작업 스레드, 짝수 번째는 CrashTester.dll 스레드). 크래시가 아니며, 끝난 뒤 다시 누를 수 있음 | 처음 두 번만 덤프를 쓰고, 세 번째부터는 덤프 없이 지나감(ADR-0031). 아래 "반복되는 처리된 예외" 참고 |
| Ctrl+Shift+Delete | 위와 같은 접근 위반을 15초 간격으로 두 번 잡은 뒤, 세 번째는 게임의 작업 스레드에서 잡지 않고 둠(크래시) | 덤프를 먼저 쓰지 않게 된 주소에서 난 진짜 크래시가 기록됨(ADR-0031 Decision 2). 아래 "두 번 처리된 뒤의 진짜 크래시" 참고 |

크래시 단축키(F1·F2·F3·F5~F9, End, Home, PageUp, PageDown, Delete)는 게임 한 번에 하나만 동작합니다. 결과는 `scripts/accuracy_benchmark.py`로 정확도 측정 세트에 넣습니다(DEVELOPMENT의 Accuracy Benchmark). 테스트용 세이브에서 쓰고, 끝나면 MO2에서 두 모드를 끄세요.

## 확인 중 들어온 두 번째 예외 (Ctrl+Shift+F5, Ctrl+Shift+PageDown)

Crash Logger를 켜고 `CrashHookMode=1`, `MaxSecondFaultDumps`가 1 이상인 세션에서 하나씩 시험합니다. 테스터는 첫 번째 접근 위반의 `crash_seq`가 게시된 뒤 `DUMP_SETTLED`가 켜졌는지 확인하고 2초를 셉니다. 신호를 확인하지 못하거나 그사이 첫 사고가 해제되면 `CrashTester.log`에 이유를 적고 두 번째 예외를 일으키지 않습니다.

`SkyrimDiag_SecondFault_*.dmp`의 예외 주소와 스레드는 같은 이름의 JSON과 Crash Logger 로그에 나온 두 번째 접근 위반과 일치해야 합니다. 일반 크래시 덤프는 첫 번째 접근 위반을 담고 자동 뷰어도 그 덤프를 엽니다. 일반 덤프의 리포트에 두 번째 접근 위반 항목이 나오며, 보조 덤프는 뷰어에서 따로 엽니다.

## 반복되는 처리된 예외 (Ctrl+Shift+Insert)

게임이나 모드가 같은 주소에서 예외를 내고 스스로 처리하기를 되풀이하는 상황을 만듭니다(넥서스의 주기적 끊김 제보: 한 SkyrimSE.exe 주소에서 23분 동안 14번, 여섯 스레드). 읽을 수 없는 메모리를 레퍼런스로 엔진의 `TESObjectREFR::GetDisplayFullName`에 넘기고, 엔진이 그 메모리를 처음 읽을 때 나는 접근 위반을 CrashTester.dll이 `__except`로 잡습니다. 그래서 예외 주소는 SkyrimSE.exe 안이고 어느 스레드에서 내도 같습니다. `SkyrimDiag.ini`의 `CrashHookMode=1`(기본값)이어야 합니다.

간격을 두는 이유: Helper가 한 예외를 확인하는 동안(덤프를 먼저 쓰는 처음 두 번은 덤프 쓰기에 이어 7초 이상, 세 번째부터는 약 4초) 플러그인은 다른 예외를 기록하지 않습니다. 간격이 그보다 짧으면 일부는 Helper 로그에 남지 않습니다.

확인할 것:

- `SkyrimDiagHelper.log`: 여섯 번 모두 `Crash event signaled (exception_code=0xC0000005, exception_addr=...`가 같은 주소로 찍힙니다.
  - 첫 번째와 두 번째는 덤프를 쓰고 `Keeping the released dump until the game exits or outlives this exception: ...`이 이어집니다.
  - 세 번째부터는 `This exception (same code and address) was already released as handled at least 2 times in this session; checking whether the game recovers before writing a crash dump.`와 `No dump written: this repeated exception was handled again and the game kept running.`이 이어지고, 덤프 파일은 새로 생기지 않습니다.
- `CrashTester.log`(`Documents\My Games\Skyrim Special Edition\SKSE\`): 매번 `handled fault N of 6 on ... caught=true`가 찍히고, 다음 예외 전까지 CrashTester.dll 스레드가 가장 오래 멈춘 시간이 `the longest pause in the 15 s after it was ... ms`로 남습니다. 덤프를 쓰는 동안에는 게임의 모든 스레드가 멈추므로, 첫 번째와 두 번째는 덤프를 쓴 시간만큼 길고 세 번째부터는 수십 ms 이하여야 합니다.
- 화면: 첫 번째와 두 번째에만 끊김이 보이고 세 번째부터는 보이지 않습니다.

시험이 끝나면 게임을 메뉴로 정상 종료합니다.

## 두 번 처리된 뒤의 진짜 크래시 (Ctrl+Shift+Delete)

같은 접근 위반을 두 번 잡아 Helper가 "두 번 처리된 예외"로 기억하게 한 뒤, 세 번째는 `__except`가 잡지 않습니다. 같은 주소에서 난 예외지만 이번에는 게임이 죽습니다. Helper는 덤프를 먼저 쓰지 않고 회복부터 확인하므로, 그 확인 뒤에 무엇이 남는지를 봅니다. 세 번째는 게임의 작업 스레드에서 내므로 게임 루프가 멈추고 하트비트도 멈춥니다.

`SkyrimDiagHelper.log`에서 세 번째 `Crash event signaled` 뒤에 `checking whether the game recovers before writing a crash dump`가 찍힙니다.

플러그인이 처리되지 않은 예외를 Helper에 알리는 빌드(ADR-0032, Helper 로그 첫머리에 `Unhandled-exception signal from the plugin: on`)에서는 곧바로 다음이 이어져야 합니다.

- `The game did not handle this exception: it reached the process's top-level exception filter ...`
- `The game did not handle this repeated exception; writing the crash dump now.`
- `SkyrimDiag_Crash_*.dmp`와 그 리포트(예외 모듈 SkyrimSE.exe, 콜스택으로 CrashTester.dll을 후보로), 그리고 평소처럼 Crash Logger의 로그.

그 신호가 없는 빌드(v0.2.76-rc1까지)에서는 게임이 얼마나 빨리 종료되느냐에 따라 아래 셋 중 하나입니다. 셋 다 ADR-0031이 정한 동작입니다.

| 그때 게임의 상태 | Helper 로그 | 남는 것 |
|---|---|---|
| 회복하지 못한 채 살아 있음(약 6초 뒤) | `The game did not recover from this repeated exception; writing the crash dump now.` | `SkyrimDiag_Crash_*.dmp`와 그 리포트. 예외 모듈 SkyrimSE.exe, 콜스택으로 CrashTester.dll을 후보로 |
| 확인하는 동안 이미 종료됨 | `The game exited after this repeated exception, before a crash dump could be written.` | 덤프 없이 `SkyrimDiag_PostExitCrashEvidence_*.json`(예외 코드와 주소). 두 번째 예외의 보류 덤프는 이 크래시의 덤프로 쓰지 않고 지움 |
| 확인하는 동안 하트비트가 계속 진행됨 | `No dump written: this repeated exception was handled again and the game kept running.` | 풀어 준 뒤 60초 안에 비정상 종료하면 `SkyrimDiag_PostExitCrashEvidence_*.json` |

이 시나리오는 최상위 예외 필터도 확인합니다. 어떤 프레임도 처리하지 않은 예외는 프로세스가 끝나기 전에 `SetUnhandledExceptionFilter`로 등록된 필터 하나에 넘어가는데, 그 자리는 다른 플러그인도 가져갑니다. CrashTester.dll은 자기 필터를 올려 두고 `CrashTester.log`에 다음을 남깁니다.

- `top-level filter before the scenario: ...`: 시나리오 시작 때 그 자리에 있던 필터의 모듈.
- `after handled fault N ...`: 처리된 예외가 지나가는 동안 다른 모듈이 그 자리를 다시 가져갔는지.
- `top-level filter reached ... ms after the fault ...; the filter under it is ...`: 세 번째 예외에서 벡터 핸들러 체인의 마지막에 등록한 핸들러가 올린 필터가 실제로 불렸는지와, 그 아래에 있던 필터의 모듈. 이 줄이 없으면 불리지 않은 것입니다. ADR-0032의 플러그인에서는 그 아래 필터가 `SkyrimDiag.dll`이어야 합니다(플러그인이 치명적 코드의 예외마다 자리를 되찾으므로).

`CrashTester.ini`의 `UnhandledHoldMs`(기본 0)를 주면 필터가 그 시간만큼 스레드를 붙잡은 뒤 아래 필터를 부릅니다. Helper가 회복하지 못했다고 판정하는 6초보다 길게 주면(예: 10000) 위 표의 첫째 줄, 곧 살아 있는 게임에서 쓰는 덤프를 확인할 수 있습니다.

버그로 볼 것: 세 번째 예외가 어디에도 기록되지 않는 경우, 두 번째 예외의 보류 덤프(`SkyrimDiag_Released_*.dmp`)가 이 크래시의 덤프로 승격되는 경우, 세 번째에서 회복 확인 없이 덤프부터 쓰는 경우, 신호가 있는 빌드인데 덤프가 남지 않는 경우.

## 무인 실행

`Documents\My Games\Skyrim Special Edition\SKSE\CrashTester.ini`(MO2 가상 폴더가 아닌 실제 폴더라 실행 중에 바꿔도 됨):

```
[CrashTester]
AutoScenario=1      ; 1: 프리징 2번 뒤 가장 가까운 NPC로 F1, 2: F2, 3: F3, 4: F6, 5: F7, 6: F8, 7: F9, 8: F10, 9: F11, 10: F5, 11: Insert, 12: Delete, 13: End, 14: Home, 15: PageUp, 16: PageDown, 0: 단축키만
SaveName=<세이브 파일 이름, .ess 제외>
StartDelaySec=20
RepeatCount=6         ; Insert 시나리오가 예외를 내는 횟수(1~50). 단축키에도 적용
RepeatIntervalSec=15  ; Insert와 Delete 시나리오에서 예외 사이의 간격(5~120초). 단축키에도 적용
UnhandledHoldMs=0     ; Delete 시나리오의 최상위 필터가 아래 필터를 부르기 전에 기다리는 시간(0~60000ms)
```

`RepeatCount`, `RepeatIntervalSec`, `UnhandledHoldMs`는 게임을 켤 때 읽습니다. 메인 메뉴에서 그 세이브를 불러오고, 로딩 20초 뒤 시나리오를 실행합니다. MO2가 켜져 있으면 `ModOrganizer.exe -p "<프로필>" "moshortcut://:SKSE"`로 실행할 수 있습니다. 프리징 감지는 게임 창이 앞에 있어야 동작합니다.

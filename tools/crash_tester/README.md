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
| Ctrl+Shift+F5 | CrashTester.dll이 스스로 잡는 접근 위반, 2초 뒤 CrashTester.dll 스레드에서 진짜 접근 위반 | 남은 한계(ADR-0017): 진짜 크래시는 기록되지 않고, 첫 예외가 크래시로 남음. 고친 뒤에는 두 번째 크래시가 잡혀야 함 |
| Ctrl+Shift+F6 | CrashTester.dll이 읽을 수 없는 메모리를 레퍼런스로 엔진의 `TESObjectREFR::GetDisplayFullName`에 넘김 | 예외 모듈 SkyrimSE.exe, 콜스택으로 CrashTester.dll을 후보로 |
| Ctrl+Shift+F7 | 저널 메뉴를 열어, 게임이 부르는 CrashTester.dll의 메뉴 이벤트 콜백 안에서 접근 위반 | 게임이 부른 플러그인 콜백의 크래시. 후보 CrashTester.dll, 그 아래는 게임의 이벤트 처리 프레임 |
| Ctrl+Shift+F8 | CrashTester.dll이 직접 만든 스레드에서 접근 위반 | 아래에 게임 프레임이 없는 스레드. 후보 CrashTester.dll |
| Ctrl+Shift+F9 | CrashTester.dll 안의 무한 재귀 | `0xC00000FD`(스택 오버플로), 후보 CrashTester.dll |
| Ctrl+Shift+F10 | 작업 스레드가 CrashTester.dll 안에서 45초 동안 CPU를 돌림 | 대기가 아니라 바쁘게 도는 프리징. 막힌 작업 스레드의 CrashTester.dll을 후보로 |
| Ctrl+Shift+F11 | 저널 메뉴를 열어, 메뉴 이벤트 콜백이 45초 동안 붙잡음 | 게임이 부른 플러그인 콜백의 프리징. 후보 CrashTester.dll |
| Ctrl+Shift+Insert | SkyrimSE.exe 안의 같은 주소에서 나는 접근 위반을 CrashTester.dll이 스스로 잡기를 15초 간격으로 6번 반복(홀수 번째는 게임의 작업 스레드, 짝수 번째는 CrashTester.dll 스레드). 크래시가 아니며, 끝난 뒤 다시 누를 수 있음 | 처음 두 번만 덤프를 쓰고, 세 번째부터는 덤프 없이 지나감(ADR-0031). 아래 "반복되는 처리된 예외" 참고 |

크래시 단축키(F1·F2·F3·F5~F9)는 게임 한 번에 하나만 동작합니다. 결과는 `scripts/accuracy_benchmark.py`로 정확도 측정 세트에 넣습니다(DEVELOPMENT의 Accuracy Benchmark). 테스트용 세이브에서 쓰고, 끝나면 MO2에서 두 모드를 끄세요.

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

## 무인 실행

`Documents\My Games\Skyrim Special Edition\SKSE\CrashTester.ini`(MO2 가상 폴더가 아닌 실제 폴더라 실행 중에 바꿔도 됨):

```
[CrashTester]
AutoScenario=1      ; 1: 프리징 2번 뒤 가장 가까운 NPC로 F1, 2: F2, 3: F3, 4: F6, 5: F7, 6: F8, 7: F9, 8: F10, 9: F11, 10: F5, 11: Insert, 0: 단축키만
SaveName=<세이브 파일 이름, .ess 제외>
StartDelaySec=20
RepeatCount=6         ; Insert 시나리오가 예외를 내는 횟수(1~50). 단축키에도 적용
RepeatIntervalSec=15  ; 그 사이의 간격(5~120초). 단축키에도 적용
```

`RepeatCount`와 `RepeatIntervalSec`는 게임을 켤 때 읽습니다. 메인 메뉴에서 그 세이브를 불러오고, 로딩 20초 뒤 시나리오를 실행합니다. MO2가 켜져 있으면 `ModOrganizer.exe -p "<프로필>" "moshortcut://:SKSE"`로 실행할 수 있습니다. 프리징 감지는 게임 창이 앞에 있어야 동작합니다.

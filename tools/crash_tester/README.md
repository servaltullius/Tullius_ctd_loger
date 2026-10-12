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

## 단축키 (게임 메인 스레드에서 실행)

| 키 | 일어나는 일 | 기대하는 분석 |
|---|---|---|
| Ctrl+Shift+F1 | 조준한 레퍼런스(없으면 플레이어)를 RCX에 둔 채 CrashTester.dll 안에서 접근 위반 | 예외 모듈과 후보가 CrashTester.dll, 덤프 오브젝트 RCX에 그 레퍼런스의 이름과 기반 오브젝트 |
| Ctrl+Shift+F2 | CrashTester.dll 안에서 `int 3` | `0x80000003`, 모드가 스스로 멈춘 것이라는 해석, 후보 CrashTester.dll |
| Ctrl+Shift+F3 | CrashTester.dll에서 처리되지 않는 C++ 예외 | C++ 예외(`0xE06D7363`), Crash Logger의 C++ 예외 정보와 대조 |
| Ctrl+Shift+F4 | 메인 스레드를 CrashTester.dll 안의 `Sleep`으로 45초 붙잡음(다시 누를 수 있음) | 자동 프리징 캡처, 플러그인 안의 대기, 두 번째는 같은 `HANG1-` 버킷 |
| Ctrl+Shift+F6 | CrashTester.dll이 읽을 수 없는 메모리를 레퍼런스로 엔진의 `TESObjectREFR::GetDisplayFullName`에 넘김 | 예외 모듈 SkyrimSE.exe, 콜스택으로 CrashTester.dll을 후보로 |

크래시 단축키는 게임 한 번에 하나만 동작합니다. 테스트용 세이브에서 쓰고, 끝나면 MO2에서 두 모드를 끄세요.

## 무인 실행

`Documents\My Games\Skyrim Special Edition\SKSE\CrashTester.ini`(MO2 가상 폴더가 아닌 실제 폴더라 실행 중에 바꿔도 됨):

```
[CrashTester]
AutoScenario=1      ; 1: 프리징 2번 뒤 가장 가까운 NPC로 F1, 2: F2, 3: F3, 4: F6, 0: 단축키만
SaveName=<세이브 파일 이름, .ess 제외>
StartDelaySec=20
```

메인 메뉴에서 그 세이브를 불러오고, 로딩 20초 뒤 시나리오를 실행합니다. MO2가 켜져 있으면 `ModOrganizer.exe -p "<프로필>" "moshortcut://:SKSE"`로 실행할 수 있습니다. 프리징 감지는 게임 창이 앞에 있어야 동작합니다.

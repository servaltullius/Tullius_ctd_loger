# SkyrimDiag MVP A 설계/구현 메모 (2026-01-28)

## 목표(이번 MVP 범위)

- **플러그인(DLL)**: 게임 프로세스 내부에서 *저부하 Blackbox 링버퍼*와 *메인스레드 heartbeat*를 기록하고, 크래시 시점에는 **passive(예외를 먹지 않는) 크래시 마킹**만 수행한다.
- **헬퍼(EXE)**: 외부 프로세스에서 *hang 감지* → **WCT(Wait Chain Traversal) 캡처 + 미니덤프 생성**을 수행하고, 덤프 안에 blackbox/WCT를 **user stream**으로 함께 넣는다.

## 컴포넌트/데이터 흐름

1. `SkyrimDiag.dll`
   - 커널 오브젝트 생성:
     - `Local\SkyrimDiag_{PID}_SHM` (공유메모리)
     - `Local\SkyrimDiag_{PID}_CRASH` (크래시 신호 이벤트)
   - `shared/SkyrimDiagShared.h`의 `SharedLayout`를 메모리 매핑하여 **블랙박스 이벤트**를 `events[]`에 기록한다.
   - 메인스레드 heartbeat는 **주소 후킹 없이**: 백그라운드 스레드가 `SKSE::TaskInterface::AddTask()`로 *주기적으로 메인스레드에서* `last_heartbeat_qpc`를 갱신한다.
   - 크래시 마킹은 `AddVectoredExceptionHandler`로 예외 정보를 `SharedHeader::crash`에 복사하고, `CRASH` 이벤트만 `SetEvent()` 후 `EXCEPTION_CONTINUE_SEARCH`로 반환한다.
   - MO2 사용성을 위해, 기본 설정에서는 플러그인이 Helper를 `--pid <SkyrimPID>`로 자동 실행한다(옵션으로 비활성 가능).

2. `SkyrimDiagHelper.exe`
   - `SkyrimSE.exe/SkyrimVR.exe` 등을 찾고, 각 PID에 대해 `..._SHM` 매핑이 열리면 attach한다.
   - 루프에서:
     - `CRASH` 이벤트 신호 시: 크래시 덤프 생성
     - heartbeat가 임계치(인게임/로딩) 이상 멈추면: **WCT 캡처(JSON)** + **hang 덤프 생성**
   - `MiniDumpWriteDump` 호출 시:
     - user stream #1: `SharedLayout` 스냅샷(blackbox)
     - user stream #2: WCT JSON (hang 케이스)

## 구현 파일

- 공유 계약: `shared/SkyrimDiagShared.h`, `shared/SkyrimDiagProtocol.h`
- 플러그인: `plugin/src/*`
- 헬퍼: `helper/src/*`
- 기본 설정: `dist/SkyrimDiag.ini`, `dist/SkyrimDiagHelper.ini`

## 제한/다음 단계

- Analyzer(덤프/user stream 파싱 → 원인 후보 리포트)는 이번 MVP에 포함하지 않음.
- 로드오더(ESM/ESP/ESL), SKSE DLL 목록 수집은 아직 미구현(다음 단계로 확장 가능).
- 이벤트 sink는 현재 메뉴/로딩 위주이며, cell/월드 이동/리소스 로드 등은 확장 필요.
- 모드팩별 로딩 시간이 크게 달라서, Helper는 **최근 로딩 시간 학습 기반(Adaptive) 로딩 임계치**를 제공하고, **수동 즉시 캡처(핫키)** 로 “무한로딩인데 오래 기다려야 하는 문제”를 완화한다.
- 실게임 검증을 위해(옵션), 플러그인에 “의도적 crash/hang” 테스트 핫키를 제공할 수 있다(기본 OFF).

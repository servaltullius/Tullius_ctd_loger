# ADR-0001: Out-of-proc Helper + Shared-Memory Blackbox

## Status
Accepted

## Context
SkyrimDiag의 목표는 “일반 사용자도 WinDbg 없이” CTD/프리징/무한로딩을 진단할 수 있도록, MO2 기반 모드팩 환경에서 **쉽게 설치/실행**되고 **오버헤드가 낮은** 진단 자료를 남기는 것입니다.

제약/요구:
- 게임 프로세스에 “무거운” 분석 로직을 상주시켜 FPS/로딩에 영향을 주면 안 됨
- 크래시 직후에도 가능한 한 높은 확률로 덤프 + 보조 정보(WCT/이벤트/리소스)를 남겨야 함
- 결과는 유저가 읽기 쉬운 텍스트/JSON + GUI 뷰어로 제공 (WinDbg 강제 금지)
- MO2 모드팩(단일 프로필이 많음)에서 “overwrite” 위치에 자연스럽게 산출물이 쌓이도록 하는 것이 UX에 유리

## Decision
다음 구조를 채택합니다:
- **SKSE 플러그인(DLL)**은 “게임 내부에서” 최소한의 진단 신호만 기록:
  - 공유 메모리(blackbox ringbuffer)에 이벤트/상태/리소스 로드(선택) 기록
  - 크래시 시 VEH로 “크래시 마크”만 남기고 예외를 소비하지 않음
- **Helper(EXE)**는 “게임 외부(out-of-proc)”에서:
  - 공유 메모리/이벤트를 모니터링하여 크래시/행 감지 시 MiniDumpWriteDump 수행
  - 프리징/무한로딩 진단을 위해 WCT(Wait Chain) 캡처 후 덤프에 user stream으로 삽입
  - 필요 시 DumpTool을 자동 실행하여 사람이 읽는 보고서를 즉시 생성
- **DumpTool(EXE)**는 덤프를 오프라인으로 분석/뷰어 UI 제공 (게임 프로세스에 상주하지 않음)

## Consequences

### Positive
- 게임 프로세스 오버헤드 최소화 (핵심 분석은 외부 프로세스에서 수행)
- 크래시/프리징에 대한 데이터 수집 파이프라인이 명확하고 디버깅이 쉬움
- MO2 친화적(모드로 설치, overwrite에 결과 생성)
- WinDbg 없이도 기본 분석 + 체크리스트 제공 가능

### Negative
- Helper 프로세스가 별도로 존재해야 함(안티바이러스/권한/차단 이슈 가능)
- out-of-proc 구조 특성상 “완벽한 원인 모드 식별”은 불가능하고, 결과는 확률/근거 기반으로 제시해야 함
- 크래시 타이밍에 따라 WCT/추가 정보 캡처가 실패할 수 있음

### Neutral
- “프로토콜(SharedLayout)” 버전 관리가 중요해짐 (전/후방 호환 고려 필요)

## Alternatives Considered
- **플러그인 내부에서 모든 것을 처리(인-프로세스 크래시 로거 방식)**:
  - 장점: 단일 바이너리, 즉시 콜스택/모듈 정보 수집 가능
  - 단점: 게임 안정성/오버헤드/호환성 부담이 커짐, 유지보수 난이도 상승
- **Crash Logger SSE/AE 로그만 파싱**:
  - 장점: 구현 단순
  - 단점: “원인 모드를 못 찾는다”는 문제를 근본적으로 해결하기 어려움

## References
- `docs/plans/2026-01-28-skyrimdiag-mvp-a-design.md`
- `docs/plans/2026-01-28-packaging-and-test-harness-plan.md`
- `doc/1.툴리우스_ctd_로거_개발명세서.md`


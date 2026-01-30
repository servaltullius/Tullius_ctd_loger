# ADR-0002: PerfHitch = Heartbeat Task Lateness (Best-effort)

## Status
Accepted

## Context
유저가 체감하는 “프레임 드랍/끊김/버벅임”은 원인이 다양합니다:
- 메인 스레드 스톨(스크립트/IO/자원 로드/락 대기 등)
- GPU 바운드(렌더링 병목)
- 드라이버/오버레이/외부 프로그램 영향

SkyrimDiag는 **게임 내부에서 무거운 프로파일링(스택 워크, ETW, GPU 계측)**을 상시 수행하기 어렵습니다.
그러나 “끊기는 시점”을 덤프/이벤트/리소스 로드 기록과 함께 남기면, 모드팩 환경에서 원인 후보를 좁히는 데 도움이 됩니다.

## Decision
“PerfHitch”는 아래 신호로 정의합니다:
- 플러그인이 백그라운드 스레드에서 일정 주기로 SKSE Task를 메인 스레드에 스케줄링한다(Heartbeat).
- 메인 스레드가 스톨되면 해당 Task 실행이 늦어지며, 그 **지연 시간(ms)**을 hitch로 기록한다.
- 필요 시 리소스 로드 기록(.nif/.hkx/.tri)과 시간 상관분석으로 “의심 모드”를 추정한다(확정 아님).

설정값은 INI로 튜닝 가능:
- `EnablePerfHitchLog`
- `PerfHitchThresholdMs`
- `PerfHitchCooldownMs`

## Consequences

### Positive
- 구현이 단순하고 오버헤드가 매우 낮음
- “언제 끊기는지” 타임라인 확보에 유용
- 리소스 로드/이벤트와 결합해 실전 진단 흐름에 잘 맞음

### Negative
- FPS 자체를 측정하지 않음(특히 GPU 바운드 저FPS는 감지가 약할 수 있음)
- 스케줄링 지연은 원인 단서일 뿐, 원인 모드를 확정할 수 없음
- Task 스케줄링 주기/OS 스케줄링에 따른 노이즈가 존재할 수 있음

### Neutral
- “best-effort” 신호로서 UI/리포트에서 항상 신뢰도(낮음/중간)를 명확히 표시해야 함

## Alternatives Considered
- **Present hook / GPU 타이밍 기반 FPS/프레임타임 계측**:
  - 장점: FPS/프레임타임을 직접 측정 가능
  - 단점: 오버레이/드라이버/호환성/안티치트/충돌 리스크 증가
- **ETW 기반(Windows Performance Recorder/Analyzer 계열)**:
  - 장점: 고정밀 분석
  - 단점: 일반 유저 UX에 부적합(설치/권한/학습 비용 높음)

## References
- `plugin/src/Heartbeat.cpp`
- `dump_tool/src/Analyzer.cpp`
- `dist/SkyrimDiag.ini`


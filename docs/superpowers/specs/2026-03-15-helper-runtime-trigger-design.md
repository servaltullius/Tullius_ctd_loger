# Helper Runtime Trigger Design

**Date:** 2026-03-15

**Goal:** 게임을 켜지 않고 synthetic runtime harness로 CTD·프리징 시 helper/dump tool이 실제로 발동하는지, 정상 종료에서는 오작동하지 않는지 검증한다.

## Context

최근 릴리즈 후보 기준으로 빌드, 패키징, schema, report 출력 검증은 충분히 쌓였다. 하지만 실제 배포 판단에서는 다음 질문이 남는다.

- CTD가 났을 때 helper가 capture 경로를 타는가
- 프리징/무한로딩 경계에서 suppression과 capture가 올바르게 동작하는가
- 정상 종료나 handled exception인데 dump tool이 오작동하지 않는가

현재 저장소에는 이 질문을 일부만 다루는 테스트가 있다.

- source-guard: crash/hang 경로에 필요한 호출 순서와 API 사용
- pure logic: `HangSuppression`, naming, candidate consensus
- config/source-guard: auto-open, recapture, preflight 키 존재

하지만 아직 없는 것은 **게임을 띄우지 않고 helper state machine 핵심 경로를 직접 밀어보는 synthetic runtime test**다.

## Decision

### 1. 분석 품질이 아니라 runtime trigger/false-positive 방지를 검증한다

이번 라운드의 질문은 “결과가 얼마나 정확한가”가 아니다.

검증 대상은:

- CTD면 capture 경로가 실제로 작동하는가
- 프리징 조건이면 suppression/capture 경계가 맞는가
- 정상 종료면 popup/dump 보존이 억제되는가

즉 helper runtime 동작 자체를 검증한다.

### 2. 게임 없이 synthetic helper integration test를 추가한다

실제 Skyrim이나 모드팩은 띄우지 않는다.

대신 테스트에서 다음을 synthetic하게 구성한다.

- `AttachedProcess`
- shared memory header/state
- crash event / process handle에 대응하는 test double 또는 controllable handle
- 임시 output directory

이 상태에서 helper 내부 함수들을 직접 호출해 경로를 검증한다.

대상 함수 예시:

- `HandleCrashEventTick`
- `HandleProcessExitTick`
- `CleanupCrashArtifactsAfterZeroExit`
- `SuppressHangAndLogIfNeeded`
- hang capture guard/execute 경계

### 3. 기존 분리된 내부 경계를 활용하고, 새 프레임워크는 도입하지 않는다

이번 라운드는 기존 C++ 테스트 패턴을 그대로 따른다.

예상 파일:

- `tests/helper_runtime_smoke_tests.cpp`
- `tests/helper_false_positive_runtime_tests.cpp`
- 필요 시 `tests/helper_hang_runtime_tests.cpp`

테스트 스타일:

- 기존 `assert(...)`
- 임시 디렉터리 사용
- helper 내부 함수 직접 호출
- 파일/log side effect 확인

새 외부 의존성, 새 테스트 프레임워크는 넣지 않는다.

### 4. 1차 시나리오는 세 그룹으로 제한한다

#### CTD path

- crash event가 들어오면 capture 경로 진입
- strong crash evidence가 있으면 crash viewer/deferred 경로 유지
- `exit_code=0`이어도 strong crash면 보존

#### 정상 종료 / false positive 방지

- `exit_code=0` + weak/handled crash면 cleanup
- deferred crash viewer 억제
- normal exit path에서 crash auto-open이 실행되지 않음

#### freeze / hang path

- heartbeat 미초기화면 auto hang capture 억제
- background / foreground grace / responsive window에서는 suppression
- 실제 hang 조건에서는 suppression을 통과

### 5. side effect는 파일과 상태를 기준으로 검증한다

다음 같은 관찰 가능한 결과를 본다.

- dump/report/incident path queue 상태
- helper log 문자열
- pending viewer path 유지/삭제 여부
- artifact cleanup 여부
- hang suppression state flag

즉 “함수가 호출됐다”보다 “외부에 남는 결과가 맞는가”를 우선 본다.

## Architecture

### Files

- Create: `tests/helper_runtime_smoke_tests.cpp`
- Create: `tests/helper_false_positive_runtime_tests.cpp`
- Create: `tests/helper_hang_runtime_tests.cpp`
- Modify: `tests/CMakeLists.txt`
- Modify: `docs/DEVELOPMENT.md`
- Create: `docs/superpowers/specs/2026-03-15-helper-runtime-trigger-design.md`
- Create: `docs/superpowers/plans/2026-03-15-helper-runtime-trigger.md`

### Test layout

#### `helper_runtime_smoke_tests.cpp`

책임:

- crash event -> capture path 진입
- strong crash evidence 유지

#### `helper_false_positive_runtime_tests.cpp`

책임:

- zero-exit cleanup
- deferred crash viewer suppression
- handled/weak crash false positive 방지

#### `helper_hang_runtime_tests.cpp`

책임:

- heartbeat not initialized suppression
- background / grace suppression
- actionable hang path 통과

### Internal access strategy

가능하면 이미 `internal` namespace로 분리된 함수들을 사용한다.

새 테스트 편의 함수가 필요하면:

- production contract를 바꾸지 않고
- test-only helper 수준에서 최소 추가

즉 테스트를 위해 public API를 새로 만들지는 않는다.

## Risks And Mitigations

### 1. Windows handle/process 의존성이 테스트를 flaky하게 만들 수 있음

대응:

- 실제 외부 프로세스 실행은 피한다
- controllable synthetic state와 temp dir 위주로 테스트한다

### 2. internal 함수 직접 호출이 구현 세부에 너무 묶일 수 있음

대응:

- 시나리오 단위로 필요한 최소 함수만 사용
- 호출 자체보다 side effect를 검증

### 3. scope가 분석 품질 테스트와 혼동될 수 있음

대응:

- 이번 문서와 테스트 이름에서 runtime trigger/false-positive만 명시
- candidate accuracy comparison은 별도 과제로 유지

## Non-goals

- 실제 Skyrim 실행
- live hook/PSS snapshot의 현장 재현
- WinUI UI 자동화
- 분석 결과 품질 회귀
- GitHub CI 필수 게이트 편입

## Acceptance Criteria

- 게임 없이 helper runtime 핵심 경로를 검증하는 synthetic 테스트가 추가된다.
- CTD path, 정상 종료 false positive 방지, freeze suppression/capture 경계가 각각 테스트된다.
- 테스트는 관찰 가능한 state/file/log side effect를 기준으로 pass/fail을 판단한다.
- `docs/DEVELOPMENT.md`에 이 테스트의 목적과 실행 방법이 문서화된다.

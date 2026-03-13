# Recapture Policy Loop Design

**Date:** 2026-03-13

**Goal:** crash와 freeze/hang/manual 분석에서 반복되는 약한 해석을 helper 재수집 정책에 연결해, 더 적절한 capture profile로 자동 재수집하도록 만든다.

## Context

현재 엔진은 다음 신호를 이미 갖고 있다.

- crash candidate consensus
- `first_chance_context`
- `symbol_runtime_degraded`
- `freeze_analysis`
- `PSS snapshot` opt-in spike
- `blackbox module/thread churn`

하지만 helper의 recapture 정책은 아직 주로 `unknown fault module`과 일부 crash 약점에 집중되어 있다. 즉 분석기는 더 똑똑해졌는데, 더 나은 dump를 다시 수집하는 폐쇄 루프는 그 수준까지 올라오지 못했다.

이 단계의 목표는 새 분석 약점을 helper recapture decision에 연결하는 것이다.

## Decision

### 1. recapture 평가는 공통, escalation은 kind별로 분리한다

재수집 정책은 `crash`와 `freeze`를 따로 구현하지 않고, 먼저 공통적으로 `재수집이 필요한가`를 평가한다.

공통 평가 입력:

- bucket 반복 여부
- candidate conflict
- reference clue only
- stackwalk degraded
- symbol/runtime degraded
- first-chance strong but candidate weak
- `freeze_analysis.state_id`
- `freeze_analysis.support_quality`

그 다음 실제 escalation은 capture kind별로 나눈다.

- crash
  - 먼저 richer crash profile
  - 반복되면 FullMemory
- freeze/hang/manual
  - 먼저 richer snapshot/PSS profile
  - `snapshot_fallback`이 반복될 때만 더 높은 단계 고려

핵심은 트리거는 공유하지만, 어떤 profile로 올릴지는 사고 유형에 맞게 다르게 선택하는 것이다.

### 2. 단일 incident보다 반복된 약점에만 반응한다

이번 정책은 aggressive하지 않게 시작한다.

기본 원칙:

- 단일 incident 한 번만으로는 recapture 금지
- 동일 bucket 또는 동일 약점이 반복될 때만 recapture 허용
- 이미 strong classification인 상태는 recapture 대상에서 제외

예:

- `deadlock_likely`는 기본적으로 recapture 불필요
- `freeze_ambiguous`가 반복될 때만 recapture 후보
- `reference_clue_only`나 `candidate_conflict`가 반복될 때만 crash recapture 후보

### 3. crash trigger는 \"분석이 약한 CTD\"에 집중한다

crash recapture trigger:

- unknown fault module 반복
- candidate conflict 반복
- reference clue only 반복
- symbol/runtime degraded + weak stack 반복
- first-chance strong but candidate가 `related/reference_clue`에 머무르는 경우 반복

핵심은 이미 strong non-system DLL culprit인 crash는 다시 캡처하지 않는다는 점이다.

### 4. freeze trigger는 \"설명력 부족한 freeze\"에 집중한다

freeze recapture trigger:

- `freeze_ambiguous` 반복
- `loader_stall_likely`인데 support quality가 `snapshot_fallback` 또는 `live_process`
- first-chance/blackbox는 강하지만 related candidate가 계속 약함

기본 제외:

- `deadlock_likely`
- `loader_stall_likely` + `snapshot_backed`

즉 이미 freeze 상태 판정이 강하고 capture quality도 충분하면 recapture를 다시 걸지 않는다.

### 5. manifest에는 reason과 target profile을 함께 남긴다

incident manifest는 이제 단순 `capture_profile`만이 아니라, recapture decision 문맥도 남겨야 한다.

필요 필드:

- `recapture_evaluation.kind`
- `recapture_evaluation.reasons[]`
- `recapture_evaluation.triggered`
- `recapture_evaluation.target_profile`
- `recapture_evaluation.escalation_level`

이 정보는 helper debug뿐 아니라 이후 analyzer/report가 “왜 이 dump가 다시 캡처됐는지” 설명하는 근거가 된다.

### 6. 이번 단계는 helper/manifest 중심으로 제한한다

이번 단계에서 직접 바꾸는 범위:

- helper recapture decision model
- pending crash / freeze recapture integration
- incident manifest reason 기록

이번 단계에서 제외:

- WinUI 변경
- analyzer UI wording 대폭 변경
- first-chance/blackbox 신호 자체의 추가 수집

## Architecture

### Policy model

새 decision model은 최소 아래 수준을 가진다.

- `RecaptureKind`
  - `Crash`
  - `Freeze`
- `RecaptureReason`
  - `unknown_fault_module`
  - `candidate_conflict`
  - `reference_clue_only`
  - `stackwalk_degraded`
  - `symbol_runtime_degraded`
  - `first_chance_candidate_weak`
  - `freeze_ambiguous`
  - `freeze_snapshot_fallback`
  - `freeze_candidate_weak`
- `target_profile`
  - `crash_richer`
  - `crash_full`
  - `freeze_snapshot_richer`

### Integration points

- `helper/include/SkyrimDiagHelper/CrashRecapturePolicy.h`
- `helper/src/PendingCrashAnalysis.cpp`
- `helper/src/IncidentManifest.cpp`

freeze recapture integration이 별도 pending task를 쓰지 않더라도, 정책 함수와 manifest reason 구조는 공통으로 유지한다.

## Risks And Mitigations

### 1. over-recapture

위험:

- 새 trigger가 많아지면 dump가 너무 많이 생길 수 있다.

대응:

- bucket 반복 또는 동일 약점 반복을 기본 조건으로 둔다.
- strong classification은 recapture 대상에서 제외한다.

### 2. crash/freeze escalation 혼선

위험:

- freeze에 crash-style full memory escalation이 섞일 수 있다.

대응:

- `target_profile`을 kind별로 분리한다.
- freeze는 richer snapshot/PSS까지만 1차 허용한다.

### 3. manifest만 복잡해지고 소비가 약함

위험:

- helper는 기록하지만 analyzer/report가 안 쓰면 반쪽이 된다.

대응:

- 이번 단계는 manifest reason 기록까지,
- 다음 단계에서 analyzer/report 소비를 별도 작업으로 잇는다.

## Non-goals

- WinUI recapture 시각화
- deadlock strong case 재설계
- PSS default-on 전환
- generic CrashDumps/WER 정책 변경

## Acceptance Criteria

- recapture decision이 crash/freeze 공통 평가 모델을 가진다.
- crash recapture trigger가 unknown fault module 외의 약점도 반영한다.
- freeze ambiguous / weak-support freeze가 recapture trigger가 될 수 있다.
- escalation은 crash/freeze kind별로 서로 다른 target profile을 고른다.
- incident manifest가 recapture reason과 target profile을 기록한다.

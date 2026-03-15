# WinUI Recapture Context Design

**Date:** 2026-03-13

**Goal:** `incident.recapture_evaluation`을 WinUI에서 explanation layer로 노출해, 사용자가 왜 이 덤프가 richer/full/snapshot recapture 경로에 들어갔는지 바로 이해할 수 있게 한다.

## Context

현재 dump tool backend는 다음 정보를 summary JSON에 이미 기록한다.

- `incident.recapture_evaluation.triggered`
- `incident.recapture_evaluation.kind`
- `incident.recapture_evaluation.target_profile`
- `incident.recapture_evaluation.reasons[]`
- `incident.recapture_evaluation.escalation_level`

하지만 WinUI는 아직 이 정보를 직접 소비하지 않는다.

- 상단 KPI/판단 카드는 원인 판정에 집중한다.
- recommendation은 텍스트 결과를 간접적으로만 보여준다.
- evidence 영역에는 recapture 맥락 전용 항목이 없다.

이 단계 목표는 `recapture_evaluation`을 WinUI의 **설명 계층**으로만 노출하는 것이다.

## Decision

### 1. 상단 판단 카드는 건드리지 않는다

`recapture_evaluation`은 root-cause 신호가 아니라 capture policy 설명이다.

따라서 이번 단계에서는:

- `QuickPrimaryValue`
- `QuickConfidenceValue`
- 상단 요약 카드

를 바꾸지 않는다.

이렇게 해야 원인 판정과 재수집 정책이 섞이지 않는다.

### 2. 노출 위치는 Evidence + Recommendations로 제한한다

이번 단계 변경 위치:

- `AnalysisSummary`
- `MainWindowViewModel`
- `MainWindow.xaml`
- `MainWindow.xaml.cs`

표시 위치는 두 군데다.

- `Recommended Next Steps`
- `Evidence`

즉 사용자가 상세 해석을 보는 흐름 안에서만 recapture 맥락을 읽게 한다.

### 3. WinUI는 recapture context를 별도 카드로 보여준다

recommendation 텍스트 안에만 흩어져 있으면 사용자가 왜 `crash_richer`, `crash_full`, `freeze_snapshot_richer`가 선택됐는지 놓치기 쉽다.

따라서 triage 패널에 작은 보조 카드 하나를 추가한다.

표시 내용:

- triggered 여부
- target profile
- escalation level
- 이유 요약

예시:

- `Recapture context`
- `target_profile=crash_richer | escalation_level=1 | reasons=candidate_conflict, symbol_runtime_degraded`

이 카드는 evidence 카드와 같은 위계의 설명 카드이며, root-cause card가 아니다.

### 4. recommendation 그룹은 기존 구조를 유지하고 recapture 문구만 선명하게 한다

현재 recommendation UI는 이미 다음 세 그룹으로 나뉜다.

- `Do This Now`
- `Verify Next`
- `Recapture or Compare`

이 구조는 유지한다.

이번 단계에서 할 일은:

- `Recapture or Compare` 그룹이 비어 있지 않을 때 recapture context 카드와 함께 보이게 정리
- recapture profile 의미가 더 명확한 recommendation copy를 우선 노출

즉 새 그룹을 만들지 않고, 이미 있는 `Recapture or Compare` 섹션의 맥락을 강화한다.

### 5. `AnalysisSummary`에 recapture 전용 읽기 모델을 추가한다

지금은 summary JSON에서 recapture metadata를 직접 읽는 모델이 없다.

추가 모델은 이 정도면 충분하다.

- `HasRecaptureEvaluation`
- `RecaptureTriggered`
- `RecaptureKind`
- `RecaptureTargetProfile`
- `RecaptureEscalationLevel`
- `RecaptureReasons`

ViewModel은 이 구조를 받아:

- `ShowRecaptureContext`
- `RecaptureContextTitle`
- `RecaptureContextDetails`

를 계산한다.

### 6. recapture context가 없으면 UI는 완전히 숨긴다

호환성 원칙:

- 구 summary file
- helper 미기록 incident
- non-recapture dump

에서는 recapture context 카드가 나타나지 않아야 한다.

빈 placeholder나 `N/A` 카드는 만들지 않는다.

## Architecture

### Data flow

1. `AnalysisSummary.LoadFromSummaryFile(...)`
   - `incident.recapture_evaluation`을 읽는다.
2. `MainWindowViewModel.PopulateSummary(...)`
   - recapture context 표시용 텍스트를 계산한다.
3. `MainWindow.RenderSummary(...)`
   - recapture context 카드 visibility와 text를 적용한다.

### UI placement

추천 위치는 `Recommendations` 카드 바로 위 또는 카드 내부 상단이다.

이유:

- 사용자가 `다음에 무엇을 해야 하는지` 읽는 흐름과 맞닿아 있다.
- 상단 KPI를 오염시키지 않는다.
- evidence 영역과 recommendation 영역을 함께 강화할 수 있다.

이번 단계에서는 `Recommendations` 카드 상단의 보조 block으로 두는 것이 가장 안전하다.

## Risks And Mitigations

### 1. root-cause 신호처럼 읽힐 위험

대응:

- 제목에 `Recapture context`를 명시
- 상단 KPI/summary verdict는 변경하지 않음

### 2. recommendation UI가 과밀해질 위험

대응:

- 카드 하나만 추가
- 세부 이유는 한 줄 summary로 압축
- 자세한 reason id는 기존 evidence/clipboard 텍스트에 맡김

### 3. old summary file compatibility

대응:

- 파싱 실패 시 기본값
- `HasRecaptureEvaluation == false`면 UI 숨김

## Non-goals

- candidate scoring 변경
- 상단 KPI/판단 카드 변경
- raw data panel 확장
- helper/analyzer/output JSON 변경

## Acceptance Criteria

- summary JSON에 `incident.recapture_evaluation`이 있으면 WinUI가 이를 파싱한다.
- triage/recommendation 영역에 recapture context 카드가 조건부로 노출된다.
- `Recapture or Compare` recommendation 그룹과 의미가 연결된다.
- recapture metadata가 없는 old summary file에서는 UI가 숨겨진다.

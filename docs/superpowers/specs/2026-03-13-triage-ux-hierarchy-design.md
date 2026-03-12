# Triage UX Hierarchy Design

**Date:** 2026-03-13

**Goal:** 정보량은 유지하면서, 사용자가 첫 10초 안에 `CrashLogger 기준 해석 -> 합의 여부 -> 다음 행동` 순서로 읽게 만든다.

## Context

현재 Triage 화면은 actionable candidate 모델로 바뀌었지만, 첫 시선에서 경쟁하는 정보가 너무 많다.

- 상단에 요약 문장, KPI 4개, 후보 리스트가 동시에 강하게 보인다.
- `Fault module`이 여전히 빠르게 노출되어 사용자가 예전 DLL 중심 해석으로 회귀하기 쉽다.
- Recommendations는 평면 리스트라서 "지금 무엇을 먼저 해야 하는가"가 다시 흐려진다.
- `Review Feedback`가 일반 분석 흐름과 같은 레벨로 보여서 사용자 중심 reading path를 방해한다.

## Decision

### 1. 첫 읽기 순서를 강제한다

Triage 상단은 아래 순서로 재배치한다.

1. CrashLogger 기준 카드
2. 근거 합의/상태 카드
3. 다음 행동 카드
4. 대체/충돌 후보 리스트

`Crash Summary` 문장과 `Actionable Candidates`는 유지하되, 첫 판단의 anchor는 `Fault module`이 아니라 `CrashLogger ref`와 `evidence agreement`가 된다.

### 2. Fault module은 판단 기준이 아니라 context로 내린다

`Fault module`은 숨기지 않는다. 대신 `Crash context` 카드로 이동시켜 상단 판단 기준과 분리한다.

### 3. Recommendations는 행동 중심 그룹으로 나눈다

기존 텍스트는 유지하되 UI에서 아래 그룹으로 구조화한다.

- `지금 바로`
- `추가 확인`
- `재수집 / 비교`

단일 `Next action` 카드는 그대로 유지하고, 상세 Recommendations는 grouped section으로 표현한다.

### 4. conflicting 후보는 비교형으로 보여준다

복수 후보 케이스는 문장형 나열 대신, 각 후보별 지지 신호를 분리한 비교 블록으로 표현한다.

### 5. 리뷰어 도구는 별도 섹션으로 낮춘다

`Review Feedback`, `Actual cause`, `Ground truth mod`는 제거하지 않는다. 다만 기본 분석 흐름과 시각적으로 분리하고 기본 접힘 상태를 사용한다.

## Non-goals

- Raw Data 탭 구조 개편
- 분석 엔진 규칙 변경
- Reviewer tool 삭제

## Acceptance Criteria

- 상단에서 `CrashLogger`/candidate 기준 정보가 `Fault module`보다 먼저 보인다.
- `Recommendations`가 최소 3개 행동 그룹으로 나뉜다.
- conflicting 후보는 각 후보별 근거를 독립적으로 읽을 수 있다.
- 리뷰어 입력 섹션은 일반 분석 흐름보다 한 단계 뒤로 배치된다.

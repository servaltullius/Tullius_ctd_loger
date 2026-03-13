# Recapture Evaluation Consumption Design

**Date:** 2026-03-13

**Goal:** helper가 기록한 `recapture_evaluation`을 analyzer 산출물에서 바로 이해할 수 있도록 evidence, recommendation, summary/report 설명 계층에 연결한다.

## Context

helper는 이제 incident manifest에 다음 정보를 남긴다.

- `recapture_evaluation.kind`
- `recapture_evaluation.triggered`
- `recapture_evaluation.reasons[]`
- `recapture_evaluation.target_profile`
- `recapture_evaluation.escalation_level`

하지만 현재 dump tool 소비는 아직 약하다.

- summary JSON/report는 일부만 보여준다.
- evidence/recommendation은 helper가 왜 richer/full/snapshot profile을 선택했는지 설명하지 않는다.
- candidate score와 confidence는 이 정보를 아직 사용하지 않는다.

이 단계 목표는 `recapture_evaluation`을 **설명 강화용 맥락**으로 연결하는 것이다.

## Decision

### 1. 이번 단계에서 `recapture_evaluation`은 점수 신호가 아니다

`recapture_evaluation`은 helper의 “재수집 필요성 판단”이다.  
이것을 candidate score나 confidence에 직접 섞으면, 원인 근거와 정책 근거가 혼합된다.

따라서 이번 단계 원칙:

- candidate score/status는 그대로 유지
- confidence 계산도 그대로 유지
- `recapture_evaluation`은 evidence/recommendation/report 설명에만 사용

### 2. 소비 위치는 세 군데로 제한한다

이번 단계 직접 변경 범위:

- `OutputWriter`
- `EvidenceBuilderEvidence`
- `EvidenceBuilderRecommendations`

필요하면 `EvidenceBuilderSummary`에 아주 얕은 문구 추가는 허용하지만, summary 결론 자체를 바꾸지는 않는다.

### 3. evidence는 “왜 이 dump가 다시 캡처되었는가”를 한 항목으로 설명한다

새 evidence item은 아래를 요약한다.

- recapture triggered 여부
- target profile
- reasons 목록
- escalation level

예:

- crash recapture:
  - `candidate_conflict + symbol_runtime_degraded 때문에 crash_richer로 재수집`
- freeze recapture:
  - `freeze_snapshot_fallback 때문에 freeze_snapshot_richer 평가`

핵심은 이 evidence가 candidate evidence를 대체하지 않고, **capture context**를 설명하는 별도 층이라는 점이다.

### 4. recommendation은 “왜 다음 단계가 그 프로필이었는지”를 설명한다

권장 조치 쪽에서는 helper 정책의 의도를 보여준다.

- `crash_richer`
  - 아직 full memory까지 갈 필요는 없었고, richer crash profile이 먼저 선택됐음을 설명
- `crash_full`
  - 이미 약한 해석이 반복돼 full memory 단계까지 올라간 것임을 설명
- `freeze_snapshot_richer`
  - freeze가 snapshot quality 부족 또는 ambiguity 때문에 richer freeze capture가 필요했음을 설명

추천 문구는 기존 candidate recommendation 뒤에 보조로 붙는 구조가 맞다.

### 5. summary/report는 recapture 이유를 읽기 쉽게 고정 출력한다

summary JSON은 이미 `incident.recapture_evaluation`을 담을 수 있다. 이번 단계에서는:

- `triggered`
- `target_profile`
- `reasons`
- `escalation_level`

이 4개 필드가 항상 정규화된 형태로 내려오게 유지한다.

report text에는 최소 아래를 추가한다.

- `RecaptureTriggered`
- `RecaptureTargetProfile`
- `RecaptureReasons`
- `RecaptureEscalationLevel`

## Architecture

### Data interpretation

`incident.recapture_evaluation`은 analyzer result 외부 입력이므로, helper/runtime quality context로 다룬다.

새 helper function 수준은 이 정도면 충분하다.

- `DescribeRecaptureReason(...)`
- `DescribeRecaptureTargetProfile(...)`
- `BuildRecaptureEvaluationEvidence(...)`

### Integration points

- `dump_tool/src/OutputWriter.cpp`
- `dump_tool/src/EvidenceBuilderEvidence.cpp`
- `dump_tool/src/EvidenceBuilderRecommendations.cpp`
- 필요 시 `tests/output_snapshot_tests.cpp`
- 필요 시 `tests/analysis_engine_runtime_tests.cpp`

## Risks And Mitigations

### 1. 원인과 정책 맥락이 섞일 위험

대응:

- evidence title/details에서 `capture context`임을 분명히 한다.
- candidate score/status는 건드리지 않는다.

### 2. recommendation이 과도하게 길어질 위험

대응:

- recapture recommendation은 target profile당 1개 정도로 제한한다.
- 기존 candidate recommendation과 중복되면 더 짧게 쓴다.

### 3. helper 미기록 incident와의 호환성

대응:

- `recapture_evaluation`이 없으면 기본값을 채워 넣고 추가 evidence/recommendation을 만들지 않는다.

## Non-goals

- candidate confidence 보정
- summary top sentence 재작성
- WinUI 표시 변경
- helper recapture policy 자체 수정

## Acceptance Criteria

- summary JSON/report가 recapture target profile과 reasons를 일관되게 보여준다.
- evidence에 recapture context를 설명하는 항목이 추가된다.
- recommendation에 recapture target profile의 의미가 드러난다.
- candidate score/status/confidence는 기존과 동일하게 유지된다.

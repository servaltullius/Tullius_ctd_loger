# Freeze Candidate Analysis Design

**Date:** 2026-03-13

**Goal:** hang/manual/snapshot 계열 dump에서 `deadlock`, `loader stall`, `ambiguous freeze`를 더 분명히 구분하고, 관련 mod/plugin/dll 후보를 기존 crash 중심 점수와 별도로 보강한다.

## Context

현재 코드베이스는 freeze 관련 신호를 이미 일부 갖고 있다.

- helper는 hang/manual capture에서 WCT JSON을 남긴다.
- phase 2 spike로 opt-in PSS snapshot metadata를 기록하기 시작했다.
- analyzer는 `has_wct`, `wct_json_utf8`, `is_hang_like`, `is_snapshot_like`, `is_manual_capture`를 읽는다.
- evidence/recommendation은 WCT cycle 유무를 문장 수준으로만 반영한다.

하지만 현재 병목은 명확하다.

- freeze 신호가 `상태 판정`으로 정리되지 않는다.
- freeze 신호가 mod/plugin/dll 후보 점수에 체계적으로 연결되지 않는다.
- OutputWriter JSON/report에는 freeze 전용 판정 모델이 없다.

즉 지금은 “WCT가 있다”는 사실만 노출될 뿐, `deadlock likely`, `loader stall likely`, `freeze ambiguous` 같은 구조화된 결과가 없다.

## Decision

### 1. freeze 계열은 crash 계열과 별도 점수 레이어를 둔다

`EXE crash`용 candidate consensus에 freeze 규칙을 억지로 섞지 않는다.

대신 analyzer 안에 별도 `freeze candidate` 집계 레이어를 둔다. 이 레이어는 freeze 계열(`is_hang_like || is_snapshot_like || is_manual_capture`)에서만 동작한다.

핵심 원칙:

- crash top frame 중심이 아니라 `state evidence` 중심으로 판정한다.
- 최종 결과는 `freeze state`와 `actionable candidate`를 분리해서 보여준다.
- 즉 결과는 “상태: deadlock likely / 관련 후보: X.dll, Y.esp” 형태여야 한다.

### 2. 1차 family는 네 개만 둔다

1차 구현은 범위를 작게 유지한다.

- `WCT family`
  - wait cycle 존재
  - blocked thread 수
  - longest wait thread
  - main-thread / candidate-thread involvement
- `Loading family`
  - `capture.isLoading`
  - loading threshold 초과
  - loading state flag
- `Snapshot family`
  - `pss_snapshot_requested`
  - `pss_snapshot_used`
  - `dump_transport`
  - 이 family는 직접 원인 점수보다 confidence/quality 보정에 사용
- `Existing candidate family`
  - actionable DLL
  - object ref
  - provider/resource
  - 반복 bucket 후보

`module churn`, `first-chance`, `thread create/exit`는 다음 단계로 미룬다. 이번 단계에서 범위를 늘리지 않는다.

### 3. freeze 상태 분류는 네 개만 둔다

1차 상태 분류:

- `deadlock_likely`
- `loader_stall_likely`
- `freeze_candidate`
- `freeze_ambiguous`

의도는 단순하다.

- `WCT cycle`이 강하면 `deadlock_likely`
- `loading + freeze context`가 강하면 `loader_stall_likely`
- freeze 신호는 있지만 단정적이지 않으면 `freeze_candidate`
- 신호가 약하거나 상충하면 `freeze_ambiguous`

이 단계에서는 무리하게 더 세분화하지 않는다.

### 4. snapshot은 원인 family가 아니라 품질 family로 둔다

PSS snapshot은 “원인을 가리키는 신호”가 아니다.

따라서:

- `pss_snapshot_used=true`는 `support_quality` 보강
- live-process fallback은 `capture_quality_degraded` 힌트
- snapshot 사용 여부만으로 `deadlock`이나 `loader stall`을 판정하지 않는다

이 원칙을 깨면 snapshot 사용 여부가 과대평가돼 오탐이 늘어난다.

### 5. OutputWriter는 freeze 모델을 구조화해 노출한다

이번 단계 범위는 UI가 아니라 analyzer + OutputWriter까지다.

따라서 summary JSON/report에 최소 아래 구조를 추가한다.

- `freeze_analysis.state_id`
- `freeze_analysis.confidence`
- `freeze_analysis.support_quality`
- `freeze_analysis.primary_reasons[]`
- `freeze_analysis.related_candidates[]`

기존 `evidence`와 `recommendations`는 이 freeze 모델을 설명하는 방향으로 확장한다.

## Architecture

### New or expanded units

- `dump_tool/src/WctTypes.h`
  - 현재 `WctCaptureDecision`만 있는 얕은 모델을 freeze 분석용 summary까지 확장
- `dump_tool/src/AnalyzerInternalsWct.cpp`
  - WCT JSON에서 cycle / blocked thread / longest-wait / loading metadata를 추출
- `dump_tool/src/FreezeCandidateConsensus.h`
- `dump_tool/src/FreezeCandidateConsensus.cpp`
  - freeze family를 집계해 `state + related candidates`를 결정
- `dump_tool/src/Analyzer.h`
  - freeze analysis 결과 모델 추가
- `dump_tool/src/Analyzer.cpp`
  - freeze 계열에서 WCT summary + existing candidates를 freeze consensus로 연결
- `dump_tool/src/EvidenceBuilderEvidence.cpp`
- `dump_tool/src/EvidenceBuilderRecommendations.cpp`
  - freeze state를 evidence/recommendation으로 노출
- `dump_tool/src/OutputWriter.cpp`
  - JSON/report에 `freeze_analysis` 블록 추가

### Data flow

1. helper가 남긴 WCT JSON / incident manifest / actionable candidates를 analyzer가 읽는다.
2. analyzer가 WCT JSON에서 freeze summary를 파싱한다.
3. `FreezeCandidateConsensus`가 family 점수로 state와 related candidate를 만든다.
4. evidence/recommendation/output이 같은 모델을 공유한다.

## Scoring Rules

### WCT family

강한 신호:

- cycle thread 존재
- cycle에 main thread 또는 candidate thread가 포함됨

중간 신호:

- longest wait thread가 명확함
- 다수 thread가 같은 wait chain에서 막힘

### Loading family

강한 신호:

- capture decision이 `isLoading=true`
- dump가 hang/snapshot 계열이고 loading state flag가 유지됨

중간 신호:

- manual snapshot인데 loading 문맥이 함께 기록됨

### Snapshot family

강한 원인 점수는 없음.

- `pss_snapshot_used=true` -> support quality 상승
- `requested=true && used=false` -> support quality 하락 또는 fallback note 추가

### Existing candidate family

보강만 허용한다.

- actionable stack DLL
- object ref
- resource/provider
- history repeat

이 family 단독으로 `deadlock_likely`를 만들 수는 없다.

## Output Contract

Summary JSON 예시:

```json
"freeze_analysis": {
  "state_id": "deadlock_likely",
  "confidence": "Medium",
  "support_quality": "snapshot_backed",
  "primary_reasons": [
    "WCT reported cycle threads",
    "main thread was part of the blocking chain"
  ],
  "related_candidates": [
    {
      "display_name": "ExampleMod.esp",
      "confidence": "Low"
    }
  ]
}
```

Report text도 같은 의미를 유지한다.

## Testing Strategy

- source-guard:
  - freeze consensus unit 존재
  - OutputWriter에 `freeze_analysis` 블록 존재
- runtime/contract:
  - `analysis_engine_runtime_tests.cpp`
  - `output_snapshot_tests.cpp`
  - WCT parser/consensus 전용 테스트 추가
- regression:
  - crash 계열 candidate consensus에는 영향이 없어야 한다
  - snapshot/manual에서 top-suspect 강한 결론이 과도하게 늘어나면 실패로 본다

## Non-goals

- WinUI 카드/레이아웃 변경
- `module churn` / `first-chance` / `thread create-exit`까지 한 번에 도입
- freeze를 crash와 같은 단일 top suspect 모델로 단순화

## Acceptance Criteria

- analyzer가 freeze 계열 dump에 대해 `freeze_analysis.state_id`를 계산할 수 있다.
- WCT cycle 기반 deadlock 판정이 구조화된 결과로 노출된다.
- loading-context 기반 loader stall 판정이 구조화된 결과로 노출된다.
- PSS snapshot 사용 여부는 support quality로만 반영된다.
- JSON/report가 동일한 freeze analysis 결과를 노출한다.
- crash 계열 기존 candidate consensus 동작은 유지된다.

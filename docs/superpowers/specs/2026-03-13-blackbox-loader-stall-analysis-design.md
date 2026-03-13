# Blackbox Loader-Stall Analysis Design

**Date:** 2026-03-13

**Goal:** blackbox에 `module load/unload`, `thread create/exit` 문맥을 추가하고, analyzer가 이 신호를 `loader_stall_likely`와 `freeze_ambiguous` 분류에 실제로 반영하도록 설계한다.

## Context

현재 코드베이스는 다음 상태다.

- plugin은 shared-memory blackbox에 `session`, `heartbeat`, `menu/load`, `perf hitch` 이벤트를 기록한다.
- helper는 이 blackbox를 그대로 dump user stream으로 넣는다.
- analyzer는 blackbox를 읽지만, 현재는 `heartbeat`, `menu/load`, `perf hitch` 정도만 설명적으로 쓴다.
- freeze 분석은 이제 `WCT`, `loading`, `snapshot`, `existing candidate`를 사용해 `deadlock_likely / loader_stall_likely / freeze_candidate / freeze_ambiguous`를 계산한다.

현재 병목은 명확하다.

- `loader stall / ILS`는 WCT만으로는 설명력이 약하다.
- 로딩 직전 어떤 DLL이 들어왔는지, thread churn이 급증했는지 같은 문맥이 없어서 `freeze_ambiguous`가 쉽게 늘어난다.
- analyzer가 blackbox를 “이벤트 로그”로만 보고, `freeze scoring family`로는 아직 거의 쓰지 않는다.

즉 다음 단계는 `more evidence`가 아니라 `loader-context evidence`를 추가하는 것이다.

## Decision

### 1. blackbox는 범용으로 넓히되, 1차 소비자는 loader stall 분석으로 둔다

새 blackbox 이벤트는 범용 구조로 추가한다.

- `module_load`
- `module_unload`
- `thread_create`
- `thread_exit`

하지만 analyzer의 1차 소비 범위는 `loader stall / ILS` 쪽으로 한정한다.

이유:

- `deadlock`은 이미 `WCT`가 직접적인 1차 신호다.
- 새 blackbox 신호는 `module churn`, `initialization churn`, `loading-transition instability` 설명력에 더 적합하다.
- 범용 freeze 전체를 한 번에 건드리면 규칙이 흐려지고 오탐 가능성이 커진다.

### 2. raw event를 직접 점수화하지 않고, `BlackboxFreezeSummary`로 한 번 더 집계한다

analyzer는 blackbox 원시 이벤트를 바로 점수에 쓰지 않는다.

중간 집계 모델을 둔다.

추천 모델:

- recent module load count
- recent module unload count
- recent thread create count
- recent thread exit count
- load/unload churn score
- thread churn score
- non-system recent module names
- loading-window bounded flag

핵심 원칙:

- raw event → aggregate → freeze family scoring
- event 하나가 직접 `loader_stall_likely`를 만들지 않는다.

이렇게 해야 규칙이 추후 `first-chance` 신호까지 확장돼도 유지된다.

### 3. 새 family는 `Blackbox context` 하나로 추가한다

현재 freeze family:

- `WCT`
- `Loading`
- `Snapshot`
- `Existing candidate`

추가 family:

- `Blackbox context`

이 family의 역할:

- `loading=true + module/thread churn`일 때 `loader_stall_likely`를 강하게 보강
- freeze 직전 유입된 비시스템 DLL/module을 related candidate 보강에 사용
- loading 문맥이 약한 경우에는 상태 승격보다 `primary_reasons` 보강에 머무름

즉 blackbox는 deadlock을 뒤집는 family가 아니라, deadlock이 아닌 freeze의 설명력을 올리는 family다.

### 4. 상태 판정 우선순위는 유지한다

상태 우선순위는 바꾸지 않는다.

1. `deadlock_likely`
2. `loader_stall_likely`
3. `freeze_candidate`
4. `freeze_ambiguous`

적용 규칙:

- WCT cycle이 있으면 여전히 `deadlock_likely`가 우선
- WCT cycle이 약하거나 없고, `loading + blackbox churn`이 강하면 `loader_stall_likely`
- actionable candidate는 related candidate 보강용
- blackbox만으로 강한 culprit 단정 금지

### 5. 관련 후보는 “직전 유입된 비시스템 module”을 우선 보강한다

새 related candidate 신호는 이 순서로 본다.

1. loading window 안에서 새로 load된 비시스템 DLL
2. load/unload가 반복된 비시스템 DLL
3. 기존 actionable candidate와 이름/모드 매핑이 일치하는 DLL

`thread create/exit`는 후보 이름을 직접 만들지 않는다. 이 신호는 상태 분류 보강용이다.

즉 결과는 아래처럼 나와야 한다.

- 상태: `loader_stall_likely`
- 이유: `loading window + module churn + thread churn`
- 관련 후보: `SomePhysics.dll`, `ExampleMod.esp`

### 6. OutputWriter는 `freeze_analysis`를 유지하고 이유만 풍부하게 만든다

이번 단계는 output schema를 크게 바꾸지 않는다.

유지:

- `freeze_analysis.state_id`
- `freeze_analysis.confidence`
- `freeze_analysis.support_quality`
- `freeze_analysis.primary_reasons`
- `freeze_analysis.related_candidates`

변경:

- `primary_reasons`에 `module churn`, `recent module load`, `thread churn`, `loading-window instability` 같은 사유를 넣는다.
- `diagnostics` 또는 report text에 blackbox aggregate 수치를 짧게 남긴다.

즉 schema 확장보다 reason quality 개선이 목표다.

## Architecture

### Plugin-side blackbox capture

관련 파일:

- `shared/SkyrimDiagShared.h`
- `plugin/src/Blackbox.cpp`
- `plugin/src/SharedMemory.cpp`
- `plugin/src/EventSinks.cpp`
- `plugin/src/Heartbeat.cpp`
- 새 sink/helper 파일이 필요하면 `plugin/src/` 아래에 추가

역할:

- event enum 확장
- blackbox payload packing 규칙 정의
- module/thread lifecycle event push

### Analyzer aggregate layer

관련 파일:

- `dump_tool/src/Analyzer.h`
- `dump_tool/src/Analyzer.cpp`
- `dump_tool/src/AnalyzerInternals.cpp`
- 새 helper가 필요하면 `dump_tool/src/AnalyzerInternalsBlackbox.cpp/.h` 추가 가능

역할:

- raw blackbox events를 `BlackboxFreezeSummary`로 집계
- loading-window bounded counts 계산
- recent non-system modules 추출

### Freeze scoring consumption

관련 파일:

- `dump_tool/src/FreezeCandidateConsensus.h`
- `dump_tool/src/FreezeCandidateConsensus.cpp`
- `dump_tool/src/EvidenceBuilderEvidence.cpp`
- `dump_tool/src/EvidenceBuilderRecommendations.cpp`
- `dump_tool/src/OutputWriter.cpp`

역할:

- `Blackbox context family` 반영
- loader stall 분류 강화
- primary reasons / related candidates 보강

## Data Model

### New event types

권장 enum:

- `kModuleLoad`
- `kModuleUnload`
- `kThreadCreate`
- `kThreadExit`

payload는 가능한 한 기존 `EventPayload` 4개 필드 안에 맞춘다.

권장 인코딩:

- module events:
  - `a`: normalized module path hash
  - `b/c/d`: truncated UTF-8 basename or key path segment
- thread events:
  - `a`: thread id or owner tid
  - `b`: flags / reserved

YAGNI 원칙:

- 전체 경로 장문 저장 금지
- callstack 저장 금지
- first-chance exception는 이번 단계 범위에서 제외

### New analyzer aggregate

추천 struct:

- `has_blackbox_context`
- `in_loading_window`
- `recent_module_loads`
- `recent_module_unloads`
- `recent_thread_creates`
- `recent_thread_exits`
- `module_churn_score`
- `thread_churn_score`
- `recent_non_system_modules`

## Scoring Rules

### Strong loader stall signals

- `loading=true`
- recent non-system module load/unload churn
- thread create/exit churn 동반

이 조합이면 `loader_stall_likely`를 강하게 지지한다.

### Medium signals

- loading은 맞지만 churn이 약함
- churn은 있지만 loading 문맥이 약함

이 경우는 `freeze_candidate`나 `freeze_ambiguous` 보강에만 쓴다.

### Candidate promotion

- recent non-system module이 기존 actionable candidate와 매핑되면 related candidate로 보강
- blackbox 단독으로 강한 top suspect 승격 금지

## Risks And Mitigations

### 1. 이벤트 폭증

위험:

- module/thread 이벤트가 너무 많아 ring buffer를 오염시킬 수 있다.

대응:

- loading window 주변만 기록하거나, 최소한 analyzer가 최근 bounded window만 소비
- basename/hash 중심 payload로 크기 최소화

### 2. noisy churn이 false positive를 만들 수 있음

위험:

- 로딩 시 원래 많은 이벤트가 발생하므로 정상 churn도 stall 원인처럼 보일 수 있다.

대응:

- `loading + freeze context + churn` 조합일 때만 강하게 사용
- blackbox만으로는 state 승격 상한을 둔다

### 3. plugin-side instrumentation 리스크

위험:

- module/thread lifecycle 후킹이 불안정할 수 있다.

대응:

- 1차 단계에서는 최소 침습 훅 또는 이미 존재하는 신호 지점 우선 활용
- 신호가 불완전해도 analyzer는 aggregate 부재를 허용해야 한다

## Non-goals

- first-chance exception 수집
- WinUI 카드/탭 변경
- deadlock 규칙 재작성
- generic crash culprit scoring 확대

## Acceptance Criteria

- blackbox에 module/thread lifecycle 신호를 기록할 수 있다.
- analyzer가 `BlackboxFreezeSummary`를 계산할 수 있다.
- freeze scoring이 `Blackbox context family`를 사용해 `loader_stall_likely` 설명력을 올린다.
- `freeze_analysis.primary_reasons`에 loader/module/thread churn 사유가 들어간다.
- crash-like dump 기존 판정은 유지된다.

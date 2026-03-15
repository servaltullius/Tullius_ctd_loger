# First-Chance CTD Candidate Design

**Date:** 2026-03-13

**Goal:** suspicious first-chance exception context를 EXE/system-victim CTD 해석 보강에 연결해, `SkyrimSE.exe`/`ntdll.dll`/`KERNELBASE.dll` 같은 약한 fatal victim 케이스에서 actionable candidate confidence를 더 정확하게 조정한다.

## Context

현재 엔진 상태는 다음과 같다.

- plugin은 suspicious first-chance를 benign filter + dedupe/rate-limit 후 blackbox에 기록한다.
- analyzer는 이를 `FirstChanceSummary`로 집계해 `freeze_analysis`에 보강 신호로 사용한다.
- EXE/system-victim CTD 쪽은 `object-ref`, `actionable stack`, `resource/provider`, `history` 중심 candidate consensus가 이미 있다.

지금 비어 있는 구간은 `fatal victim은 EXE/system DLL인데, fatal 직전 repeated suspicious first-chance가 특정 모듈/경로를 반복적으로 가리키는 케이스`다.

이 구간은 사용자가 가장 자주 막히는 유형이다.

- `SkyrimSE.exe`만 뜬다.
- `KERNELBASE.dll`만 뜬다.
- stackwalk는 약하고 object-ref도 단독이다.
- 그런데 fatal 직전 first-chance가 같은 비시스템 모듈/주소 버킷에서 반복됐을 수 있다.

따라서 이번 단계는 first-chance를 freeze가 아니라 `EXE/system-victim CTD candidate consensus`의 보강 family로 연결한다.

## Decision

### 1. 적용 범위는 EXE/system-victim CTD로 제한한다

이번 단계는 다음 조건에서만 동작한다.

- crash-like dump
- fault module이 game exe 또는 system module
- 기존 actionable candidate가 이미 존재하거나, 최소한 object-ref/stack/resource 등 약한 후보 근거가 있음

적용 제외:

- freeze/hang/manual snapshot scoring 변경
- 명확한 non-system DLL culprit CTD 재해석
- first-chance만으로 새 후보 생성

핵심은 “애매한 fatal victim CTD를 보강”하는 것이지, 전반적인 candidate 엔진을 다시 쓰는 것이 아니다.

### 2. first-chance는 새로운 culprit가 아니라 supporting family다

새 family id:

- `first_chance_context`

하지만 이 family는 독립 승격 권한이 없다.

허용:

- `object-ref + first-chance` 합의 시 candidate confidence 보강
- `actionable stack + first-chance` 합의 시 candidate confidence 보강
- `object-ref + stack + first-chance` 합의 시 stronger cross-validation

금지:

- first-chance only 승격
- deadlock/freeze state override
- 기존 strong DLL culprit 뒤집기

즉 역할은 “fatal crash 직전 이미 보였던 반복 예외 문맥이 이 candidate와 맞는다”를 추가하는 것이다.

### 3. repeated signature와 related module이 있을 때만 가점한다

가점 조건은 보수적으로 둔다.

- `FirstChanceSummary.has_context = true`
- `repeated_signature_count > 0` 또는 `loading_window_count/recent_count`가 의미 있게 높음
- `recent_non_system_modules`가 candidate의 DLL/mod/plugin과 연결됨

가점 크기:

- `object-ref + first_chance_context`: 중간
- `actionable stack + first_chance_context`: 중간
- `object-ref + actionable stack + first_chance_context`: 강함

연결이 없으면:

- evidence/recommendation에는 노출 가능
- candidate score 보강은 금지

즉 signal quality와 candidate linkage가 동시에 있을 때만 점수로 사용한다.

### 4. candidate consensus는 완전히 새 레이어를 만들지 않고 기존 actionable candidate scoring에 흡수한다

이번 단계에서 새 CTD consensus 레이어는 만들지 않는다.

이유:

- 이미 `ActionableCandidate` 구조와 family 기반 scoring이 있다.
- `first_chance_context`는 기존 family 하나로 추가하는 편이 가장 작고 안전하다.
- 별도 CTD second-pass consensus를 만들면 과한 복잡도가 생긴다.

따라서 변경 지점은:

- candidate aggregation / scoring
- evidence / recommendation
- output JSON / report

### 5. output은 aggregate와 family 결과만 노출한다

summary/report에는 raw first-chance flood를 늘리지 않는다.

노출 대상:

- candidate `supporting_families`에 `first_chance_context`
- repeated suspicious first-chance가 candidate 보강에 쓰였다는 evidence text
- recent non-system first-chance modules

목표는 사용자가 다음을 이해하는 것이다.

- fatal victim은 EXE/system이지만,
- 반복 first-chance 문맥이 특정 비시스템 모듈 쪽을 계속 가리켔다.

## Architecture

### Analyzer / Candidate Consensus

- `FirstChanceSummary`를 candidate scoring 입력으로 전달
- EXE/system-victim CTD에서만 `first_chance_context` family를 평가
- candidate linkage가 있을 때만 family_count/score/confidence를 보정

### Evidence / Recommendations

- repeated suspicious first-chance를 concise text로 설명
- `recent_non_system_modules`를 triage next step에 사용
- generic EXE crash triage를 바로 넓히기보다, 반복 first-chance 경로를 먼저 확인하도록 유도

### Output

- actionable candidate family list에 `first_chance_context`
- report/evidence에 repeated first-chance 문구 추가
- existing `freeze_analysis.first_chance_context`는 그대로 유지

## Risks And Mitigations

### 1. noisy first-chance로 인한 오탐

위험:

- repeated first-chance가 항상 real culprit는 아니다.

대응:

- EXE/system-victim에만 적용
- candidate linkage 필요
- first-chance only 승격 금지

### 2. 기존 strong signal 뒤집기

위험:

- weak first-chance가 strong object-ref/stack 결론을 덮을 수 있다.

대응:

- first-chance는 supporting family만 허용
- 이미 strong culprit인 non-system DLL 케이스에는 적용하지 않음

### 3. family inflation

위험:

- same-source telemetry를 과도하게 독립 family처럼 취급할 수 있다.

대응:

- first-chance는 항상 하나의 family로만 센다.
- aggregate 품질은 count/repeat/module linkage로만 평가한다.

## Non-goals

- freeze scoring 재설계
- first-chance 기반 dump trigger 추가
- WinUI 변경
- object-ref 중심 EXE candidate model의 전면 재작성

## Acceptance Criteria

- EXE/system-victim CTD에서만 `first_chance_context` family가 candidate scoring에 참여한다.
- first-chance 단독으로는 candidate가 승격되지 않는다.
- repeated suspicious first-chance + candidate linkage가 있을 때만 confidence가 보강된다.
- evidence/recommendation/output이 repeated suspicious first-chance 보강 이유를 설명한다.

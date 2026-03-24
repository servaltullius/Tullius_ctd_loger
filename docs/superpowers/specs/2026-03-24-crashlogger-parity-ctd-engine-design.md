# Crash Logger Parity CTD Engine Design

**Date:** 2026-03-24

**Goal:** `Crash Logger`가 이미 강하게 드러내는 CTD 신호를 Tullius 엔진의 중심 랭킹 경로에 승격해, 사용자가 `Crash Logger만 보고 해결했다`고 느끼는 격차를 줄인다.

## Context

현재 엔진은 이미 `Crash Logger`를 읽고 있다.

- [Analyzer.cpp](/home/kdw73/Tullius_ctd_loger/dump_tool/src/Analyzer.cpp) 는 `IntegrateCrashLoggerLog(...)`를 호출하고, `top_modules`를 suspect 재정렬 bonus에 사용한다.
- [CrashLoggerParseCore.cpp](/home/kdw73/Tullius_ctd_loger/dump_tool/src/CrashLoggerParseCore.cpp) 는 `version`, `top modules`, `C++ exception details`, `object refs`를 파싱한다.
- [EvidenceBuilderCandidates.cpp](/home/kdw73/Tullius_ctd_loger/dump_tool/src/EvidenceBuilderCandidates.cpp) 와 [CandidateConsensus.cpp](/home/kdw73/Tullius_ctd_loger/dump_tool/src/CandidateConsensus.cpp) 는 `object ref`, `actionable stack`, `resource`, `history`, `first-chance`를 actionable candidate family로 합성한다.

하지만 현재 병목은 명확하다.

- `Crash Logger` 신호가 mostly evidence/bonus 계층에 머문다.
- `direct DLL fault`, `first non-system probable frame`, `same-DLL probable streak` 같은 강한 CTD 신호가 별도 구조화되지 않는다.
- EXE/system victim CTD에서 `Crash Logger`가 더 강하게 말하는데도, Tullius는 `object ref`나 약한 stack candidate 위주로 요약하는 경우가 남는다.

즉, 문제는 `Crash Logger` 통합 부재가 아니라 `Crash Logger` 핵심 신호를 **랭킹 엔진이 충분히 소비하지 못하는 것**이다.

## Decision

### 1. 1차 목표는 `Crash Logger parity`다

이번 단계의 성공 기준은 "Tullius가 Crash Logger보다 더 똑똑해진다"가 아니다.

먼저 확보할 기준은 아래다.

- `Crash Logger`가 명확한 CTD culprit를 보여 주는 케이스에서
- Tullius도 최소한 같은 culprit를 top candidate 또는 top actionable candidate로 드러낸다.

즉 이번 단계는 절대 정확도 최적화보다 `Crash Logger` 대비 parity gap 축소가 목표다.

### 2. `Crash Logger raw CTD frame`을 1급 분석 입력으로 승격한다

현재 `top_modules`는 빈도 기반 압축 결과다. 이 값만으로는 아래 구분이 사라진다.

- direct fault module
- 첫 non-system probable frame
- 같은 DLL이 probable call stack 상단을 연속 점유하는지
- thread dump / probable call stack 중 어느 쪽이 더 명확한지

따라서 이번 단계에서 `Crash Logger` raw log는 아래 구조 신호를 추가로 만든다.

- `direct_fault_module`
- `first_actionable_probable_module`
- `probable_module_streak_winner`
- `probable_module_streak_length`
- `cpp_exception_module`

이 값들은 evidence 전용이 아니라 suspect/actionable candidate scoring에 직접 들어간다.

### 3. `top_modules bonus` 모델을 `rank-and-promote` 모델로 바꾼다

현재 [Analyzer.cpp](/home/kdw73/Tullius_ctd_loger/dump_tool/src/Analyzer.cpp)의 `ApplyCrashLoggerCorroborationToSuspects(...)` 는 기본적으로 기존 suspect score에 bonus를 얹는 구조다.

이 모델은 다음 상황에서 부족하다.

- Tullius stackwalk가 약하지만 Crash Logger direct fault가 명확한 경우
- EXE/system victim이라 Tullius top suspect가 약한 경우
- `Crash Logger`는 한 DLL로 집중되는데 Tullius는 분산 점수 상태인 경우

따라서 보정 규칙을 아래처럼 바꾼다.

- `direct_fault_module` 일치 시: strong promotion
- `first_actionable_probable_module` 일치 시: medium promotion
- `probable_module_streak_winner` 일치 시: medium promotion
- `cpp_exception_module` 일치 시: additive promotion

즉 `Crash Logger`는 더 이상 "조금 가산점 주는 보조 증거"가 아니라, 조건 충족 시 top ordering을 뒤집을 수 있는 승격 규칙이 된다.

### 4. actionable candidate family에 `crash_logger_frame` 계열을 추가한다

현재 candidate consensus는 `crash_logger_object_ref`, `actionable_stack`, `resource_provider`, `history_repeat`, `first_chance_context` 중심이다.

여기에 새 family를 추가한다.

- `crash_logger_frame`

이 family는 `object ref`와 다르다. 의미는 아래와 같다.

- object ref: 사고 당시 게임이 처리하던 ESP/ESM 단서
- crash_logger_frame: 사고 직전/즉시 frame 상 culprit DLL 단서

이 분리는 중요하다. 지금은 `Crash Logger`가 강하게 DLL을 말해도, candidate consensus에서는 object ref 쪽이 더 눈에 띄는 경우가 생긴다.

새 규칙:

- `crash_logger_frame + actionable_stack` => `cross_validated` 후보 가능
- `crash_logger_frame + object_ref` => `related` 이상 가능
- `crash_logger_frame only` => strong `reference_clue` 가능
- `history_repeat`는 여전히 boost only로 유지

### 5. EXE/system victim summary를 `Crash Logger frame first`로 재작성한다

현재 [EvidenceBuilderSummary.cpp](/home/kdw73/Tullius_ctd_loger/dump_tool/src/EvidenceBuilderSummary.cpp)는 EXE/system victim에서 `object ref`, `actionable stack`, `related candidate` 중심으로 문장을 만든다.

이번 단계에서는 다음 우선순위로 바꾼다.

1. `cross_validated actionable candidate`
2. `Crash Logger direct/probable frame`으로 지지되는 actionable DLL candidate
3. `object ref + weak stack`
4. isolated object ref
5. weak stack scan only

즉 `Crash Logger`가 DLL culprit를 강하게 말하는데도 summary가 ESP clue를 먼저 말하는 상황을 줄인다.

### 6. 이번 단계는 CTD 전용이다

범위 포함:

- crash-like incidents
- EXE victim
- Windows system DLL victim
- direct non-system DLL fault

범위 제외:

- freeze/hang scoring redesign
- PSS/WCT 강화
- dump capture profile 변경
- WinUI 개편

지금 사용자 피드백의 핵심은 CTD 정확도 부족이므로, 1차 작업은 CTD 엔진에 집중한다.

## Architecture

### A. CrashLogger parser layer

대상 파일:

- [CrashLoggerParseCore.h](/home/kdw73/Tullius_ctd_loger/dump_tool/src/CrashLoggerParseCore.h)
- [CrashLoggerParseCore.cpp](/home/kdw73/Tullius_ctd_loger/dump_tool/src/CrashLoggerParseCore.cpp)
- [CrashLogger.cpp](/home/kdw73/Tullius_ctd_loger/dump_tool/src/CrashLogger.cpp)

추가 구조:

- probable frame row parser
- first actionable module extractor
- streak detector
- direct fault extractor

원칙:

- system/game exe/hook framework는 raw parse 단계에서 버리지 않는다.
- parser는 최대한 raw structure를 보존하고,
- filtering/ranking은 analyzer에서 한다.

### B. Analysis result contract

대상 파일:

- [Analyzer.h](/home/kdw73/Tullius_ctd_loger/dump_tool/src/Analyzer.h)

추가 필드:

- `crash_logger_direct_fault_module`
- `crash_logger_first_actionable_probable_module`
- `crash_logger_probable_streak_module`
- `crash_logger_probable_streak_length`
- `crash_logger_frame_signal_strength`

필드 목적은 "요약 문자열"이 아니라 후속 scoring의 명시적 입력을 만드는 것이다.

### C. Suspect ranking

대상 파일:

- [Analyzer.cpp](/home/kdw73/Tullius_ctd_loger/dump_tool/src/Analyzer.cpp)

핵심 변경:

- `ApplyCrashLoggerCorroborationToSuspects(...)`를 bonus model에서 promotion model로 변경
- direct fault / first probable / streak / C++ exception module을 각각 다른 강도로 적용
- hook framework victim false positive는 기존 demotion 유지

### D. Candidate consensus

대상 파일:

- [EvidenceBuilderCandidates.cpp](/home/kdw73/Tullius_ctd_loger/dump_tool/src/EvidenceBuilderCandidates.cpp)
- [CandidateConsensus.cpp](/home/kdw73/Tullius_ctd_loger/dump_tool/src/CandidateConsensus.cpp)

핵심 변경:

- `crash_logger_frame` family 추가
- frame family와 stack family가 합의하면 high-confidence candidate 승격 허용
- object ref와 frame family가 엇갈리면 `conflicting` 가능
- EXE/system victim에서는 frame family를 object ref보다 상위 우선순위로 평가

### E. Summary and recommendations

대상 파일:

- [EvidenceBuilderSummary.cpp](/home/kdw73/Tullius_ctd_loger/dump_tool/src/EvidenceBuilderSummary.cpp)
- [EvidenceBuilderRecommendations.cpp](/home/kdw73/Tullius_ctd_loger/dump_tool/src/EvidenceBuilderRecommendations.cpp)

핵심 변경:

- `Crash Logger frame first` reading path 추가
- direct DLL fault일 때는 object-ref보다 DLL guidance를 먼저 제시
- EXE/system victim에서 `Crash Logger`와 stack candidate가 일치하면 그 합의를 먼저 설명

## Recommended Phases

### Phase 1: raw signal extraction

- probable frame row parser 추가
- direct fault / first probable / streak 추출
- parser 회귀 테스트 추가

### Phase 2: suspect promotion

- new analyzer fields 연결
- suspect reorder/promotion 규칙 추가
- direct-fault / probable-streak 기반 ranking 회귀 테스트 추가

### Phase 3: candidate consensus and output

- `crash_logger_frame` family 추가
- EXE/system victim summary/recommendation 우선순위 수정
- output snapshot / summary regression 보강

## Risks And Mitigations

### 1. Crash Logger 과신

위험:

- `Crash Logger`가 victim frame이나 hook DLL을 직접 culprit처럼 보이게 할 수 있다.

대응:

- hook framework / system / game exe filtering은 analyzer 단계에서 유지
- `direct_fault_module`도 hook/system/game exe면 unconditional promotion 금지

### 2. parser overfitting

위험:

- 특정 Crash Logger 버전 포맷에만 맞춘 파서가 될 수 있다.

대응:

- `v1.17`, `v1.18`, `v1.19+`, `v1.20+` fixture를 유지
- row parser는 tolerant parsing으로 구현

### 3. object ref 가치 하락

위험:

- DLL culprit 강화 과정에서 resource/plugin-side clue가 가려질 수 있다.

대응:

- object ref 제거가 아니라 family 분리
- DLL frame과 object ref가 같은 candidate로 합쳐질 때만 high confidence

## Non-goals

- freeze/hang 분석 재설계
- PSS/WCT capture 개선
- dump profile 변경
- benchmark corpus 재도입
- Crash Logger raw log를 외부 공개 정확도 지표의 정답으로 선언하는 것

## Acceptance Criteria

- `Crash Logger` direct fault, first probable module, probable streak가 구조화 필드로 파싱된다.
- `Analyzer.cpp`는 위 신호를 단순 bonus가 아니라 suspect promotion 규칙으로 사용한다.
- actionable candidate consensus에 `crash_logger_frame` family가 추가된다.
- EXE/system victim summary는 `Crash Logger frame` 기반 DLL signal을 object-ref보다 우선 설명할 수 있다.
- direct DLL fault 케이스에서 Tullius summary/recommendation이 DLL culprit를 먼저 제시한다.

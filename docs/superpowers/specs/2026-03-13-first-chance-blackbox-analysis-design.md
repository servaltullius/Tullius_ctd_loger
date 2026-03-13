# First-Chance Blackbox Analysis Design

**Date:** 2026-03-13

**Goal:** benign 예외는 제외하고, 의심스러운 first-chance exception만 blackbox에 기록해 CTD/프리징 직전 문맥을 더 정확히 해석한다.

## Context

현재 엔진은 다음 상태다.

- `CrashHookMode=1`에서 fatal exception만 crash dump로 이어진다.
- plugin blackbox는 menu/load/perf hitch/module/thread churn까지 기록한다.
- analyzer는 `BlackboxFreezeSummary`와 `freeze_analysis`를 통해 loader stall / deadlock / ambiguous freeze를 구분한다.

현재 빠진 것은 `dump 직전 예외 문맥`이다.

- 실제 CTD 전에 같은 address/code의 first-chance가 반복될 수 있다.
- loading 중 반복되는 first-chance는 loader stall 또는 초기화 실패 정황일 수 있다.
- 반대로 C++ throw, thread naming, `OutputDebugString` 같은 benign first-chance는 노이즈가 크다.

따라서 이번 단계는 `first-chance exception telemetry`를 blackbox에 추가하되, dump 정책은 건드리지 않는다.

## Decision

### 1. dump policy는 유지하고, first-chance는 telemetry로만 추가한다

이번 단계는 crash capture policy를 바꾸지 않는다.

- `CrashHookMode=1`의 fatal-only 원칙 유지
- first-chance 때문에 dump를 더 자주 만들지 않음
- 추가되는 것은 blackbox ring buffer의 예외 문맥뿐

즉 제품적으로는 “오탐 dump 증가” 없이 “pre-crash context 증가”가 목표다.

### 2. benign first-chance는 기록하지 않는다

기록 제외 대상:

- `0xE06D7363` C++ exception
- `0x406D1388` thread naming
- `0x40010006`, `0x4001000A` OutputDebugString 계열
- crash handler가 이미 benign으로 분류하는 기존 예외 코드

기록 후보:

- access violation 계열
- loading 문맥에서 반복되는 비-benign first-chance
- 동일 `(code, address, module)` 조합이 짧은 구간에 반복되는 경우

핵심은 “모든 first-chance”가 아니라 “설명력이 있는 suspicious first-chance”만 남기는 것이다.

### 3. rate limit와 dedupe를 넣는다

first-chance는 매우 시끄러울 수 있으므로 다음 제한이 필요하다.

- 같은 `(code, address, module)` 조합은 짧은 윈도우에서 한 번만 기록
- 초당 상한을 둔다
- ring buffer를 과도하게 소비하지 않는다

이 제한은 module/thread churn과 같은 수준의 lightweight telemetry로 유지하기 위한 것이다.

### 4. event model은 범용으로 두고, 1차 소비는 loader stall과 weak CTD 보강으로 제한한다

새 event type은 최소 아래가 필요하다.

- `kFirstChanceException`

payload는 아래 정보를 담는다.

- exception code
- exception address 또는 stable hash
- module hash
- short module label

analyzer는 raw event를 그대로 쓰지 않고 `FirstChanceSummary`로 집계한다.

집계 필드:

- 최근 first-chance count
- unique signature count
- loading-window first-chance count
- repeated suspicious signature count
- recent non-system modules

### 5. scoring은 보강용으로만 쓴다

`first_chance_context` family를 추가하되, 단독 확정은 금지한다.

사용처:

- `loader_stall_likely` 보강
- `freeze_ambiguous`를 `loader_stall_likely` 또는 `freeze_candidate`로 올릴지 판단
- `EXE/system victim` CTD에서 weak stack/object-ref 해석 보강

금지 규칙:

- deadlock(WCT cycle)을 뒤집지 않음
- object-ref + actionable stack 합의를 뒤집지 않음
- first-chance only로 단일 범인 확정 금지

즉 역할은 “새 범인을 만드는 것”이 아니라 “기존 해석을 더 정당화하는 것”이다.

### 6. output에는 aggregate만 노출한다

summary/report에는 raw first-chance flood를 그대로 쓰지 않는다.

노출 대상:

- `first_chance_context` aggregate
- repeated suspicious signatures 존재 여부
- loading-window first-chance 정황
- 관련 non-system modules

이 방식이면 output noise를 늘리지 않고 의미 있는 문맥만 노출할 수 있다.

## Architecture

### Plugin

- crash handler 또는 vectored exception path에서 suspicious first-chance를 필터링
- dedupe/rate-limit 후 `kFirstChanceException` blackbox event 기록
- 기존 crash capture policy는 그대로 유지

### Analyzer

- raw events -> `FirstChanceSummary`
- `BlackboxFreezeSummary`와 함께 freeze consensus에 보강 family로 제공
- crash-like dumps에서는 weak victim-ish 해석 보강용 evidence로만 사용

### Output

- `freeze_analysis.first_chance_context`
- diagnostics/evidence/recommendations에 concise text 추가

## Risks And Mitigations

### 1. 노이즈 증가

위험:
- first-chance는 본질적으로 noisy하다.

대응:
- benign filter
- dedupe
- rate limit
- aggregate-only output

### 2. crash policy와 telemetry 경계가 흐려질 수 있음

위험:
- first-chance event를 기록하다가 잘못 dump policy까지 넓힐 수 있다.

대응:
- 이번 단계에서는 `CrashHookMode`와 dump trigger 로직을 건드리지 않는다.
- output 문구도 “context”라고 명확히 표현한다.

### 3. 잘못된 승격

위험:
- noisy first-chance가 loader stall 또는 candidate를 과도하게 끌어올릴 수 있다.

대응:
- deadlock precedence 유지
- blackbox-only 승격 금지
- loading-window/repeated-signature 조건이 있을 때만 가점

## Non-goals

- first-chance 기반 dump trigger 추가
- WinUI 시각화 변경
- deadlock 규칙 재작성
- 모든 예외 코드 기록

## Acceptance Criteria

- suspicious first-chance만 blackbox에 기록된다.
- benign first-chance는 기록되지 않는다.
- analyzer가 `FirstChanceSummary`를 만들 수 있다.
- freeze/crash scoring이 `first_chance_context`를 보강용으로 사용할 수 있다.
- summary/report가 first-chance aggregate를 설명한다.

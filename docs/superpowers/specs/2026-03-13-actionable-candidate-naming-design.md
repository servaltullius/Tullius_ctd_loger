# Actionable Candidate Naming Design

**Date:** 2026-03-13

**Goal:** `Faction Ranks`처럼 사용자가 실제 파일/플러그인과 연결하기 어려운 친화 라벨이 대표 후보 이름으로 보이지 않도록, actionable candidate의 표시 이름 정책을 파일 기반 식별자 우선으로 재정의한다.

## Context

현재 actionable candidate의 대표 이름은 사용자가 실제로 찾을 수 있는 식별자와 어긋날 수 있다.

문제 예시:

- UI 상단과 후보 카드가 `Faction Ranks`를 대표 이름처럼 보여준다.
- 하지만 사용자는 MO2, Vortex, Explorer, 플러그인 목록에서 이 이름을 바로 찾을 수 없다.
- 반면 같은 후보에 `FactionRanks.esp` 또는 `paragon-perks.dll` 같은 실제 파일 식별자가 있을 수 있다.

현재 시스템은 candidate에 여러 이름 축을 이미 갖고 있다.

- `plugin_name`
- `mod_name`
- `module_filename`
- `display_name`

문제는 이 값들을 어떤 우선순위로 대표 이름으로 노출할지 정책이 약하다는 점이다.

## Decision

### 1. 대표 이름은 “찾을 수 있는 식별자”를 우선한다

대표 표시 규칙:

1. `plugin filename` (`.esp/.esm/.esl`)
2. `dll filename`
3. `mod folder name`
4. 마지막 fallback만 친화 라벨 / 기타 추론 이름

즉 `Faction Ranks` 같은 친화 라벨은 더 이상 대표 이름이 아니라 fallback이어야 한다.

### 2. `ESP/ESM`이 있으면 `DLL`보다 우선한다

`plugin_name`과 `module_filename`이 동시에 있을 때는 `plugin_name`을 대표 이름으로 쓴다.

이유:

- 사용자가 MO2/Vortex 플러그인 목록에서 바로 찾을 수 있다.
- `DLL`은 관련 신호로는 유용하지만, gameplay 모드/플러그인 단위 확인에는 `ESP/ESM`이 더 실용적이다.

예:

- 대표: `FactionRanks.esp`
- 보조: `표시명: Faction Ranks`
- 추가: `관련 DLL: paragon-perks.dll`

### 3. 친화 라벨은 보조 표시로만 남긴다

친화 라벨을 완전히 버리지는 않는다.

대신 역할을 바꾼다.

- 대표 이름: 파일 기반 식별자
- 보조 줄: 친화 라벨 / 모드 폴더명 / 관련 DLL

즉 `Faction Ranks`는 카드 제목이 아니라 세부 설명 줄에 들어가야 한다.

### 4. 이름 정책은 backend에서 고정한다

이번 문제는 WinUI만의 문제가 아니다.

영향 출력:

- WinUI 상단 `행동 우선 후보`
- 후보 리스트 카드 제목
- summary sentence
- clipboard/share text
- report text

따라서 이름 우선순위는 backend candidate model에서 먼저 고정하고, UI/CLI/report가 그 결과를 공통으로 소비해야 한다.

### 5. 출력 모델은 대표 식별자와 보조 별칭을 분리한다

추천 모델:

- `primary_identifier`
- `secondary_label`

해석:

- `primary_identifier`: 찾을 수 있는 이름
- `secondary_label`: 사람이 읽기 쉬운 별칭

기존 `display_name`은 내부 호환 때문에 남길 수 있지만, 사용자-facing 출력은 `primary_identifier`를 우선 사용해야 한다.

## Architecture

### Backend

변경 지점:

- actionable candidate naming helper
- candidate consensus output fields
- summary/report/export fields

역할:

- `plugin_name`, `module_filename`, `mod_name`, fallback label을 받아
- `primary_identifier`, `secondary_label`, optional `related_module`을 계산

### WinUI

표시 규칙:

- 카드 제목: `primary_identifier`
- 보조 줄: `secondary_label`
- 추가 줄: 필요 시 `related DLL` 또는 `모드 폴더`

상단 KPI와 summary sentence도 같은 대표 이름을 써야 한다.

### Clipboard / Report

- summary/share text는 대표 이름을 먼저 노출
- 친화 라벨은 필요할 때만 괄호나 보조 줄로 노출

## Risks And Mitigations

### 1. 기존 출력과 호환성 문제

대응:

- 기존 필드는 유지할 수 있다.
- 새 대표 식별자를 우선 사용하되 fallback은 남긴다.

### 2. 사람이 읽기 어려워질 위험

대응:

- 친화 라벨을 삭제하지 않고 secondary label로 남긴다.
- DLL만 있는 경우도 보조 줄에 모드/라벨을 붙인다.

### 3. 이름 축이 비어 있는 candidate

대응:

- 우선순위대로 내려가며 fallback
- 어떤 경우에도 empty title은 금지

## Non-goals

- candidate score 변경
- 새로운 원인 분석 signal 추가
- naming database 구축
- WinUI 전체 레이아웃 변경

## Acceptance Criteria

- `Faction Ranks` 같은 친화 라벨이 대표 이름으로 먼저 나오지 않는다.
- `plugin filename > dll filename > mod folder name > fallback` 규칙이 backend에서 고정된다.
- WinUI, summary/report, share text가 같은 대표 이름 정책을 쓴다.
- 친화 라벨은 secondary label로만 남는다.

# Output-Root-Only Dump Discovery Design

**Date:** 2026-03-13

**Goal:** 덤프툴 시작 화면의 자동 발견 흐름을 generic Windows dump 탐색이 아니라 `툴리우스가 실제로 쓰는 output 위치` 중심으로 재정의한다.

## Context

기존 dump discovery UX는 `학습 경로 / 등록 경로 / 자동 경로` 구조를 도입했지만, 자동 경로에 `%LOCALAPPDATA%\CrashDumps`를 포함시켰다.

이 구성은 일반적인 Windows crash dump 위치를 보여주는 데는 도움이 될 수 있지만, 현재 제품 목표와는 어긋난다.

- 제품 문서가 안내하는 기본 output은 MO2 `overwrite\SKSE\Plugins\`다.
- 경로를 바꾸는 공식 방식은 `SkyrimDiagHelper.ini`의 `OutputDir=`다.
- 사용자가 실제로 찾고 싶은 것은 generic system crash dump가 아니라 `툴리우스가 생성한 dump`다.
- 따라서 `CrashDumps`가 첫 화면 주 결과로 보이면, 제품이 의도한 output 위치보다 엉뚱한 dump를 먼저 보여줄 수 있다.

## Decision

### 1. 자동 발견은 `툴리우스 output root`만 본다

자동 발견 대상은 아래로 제한한다.

- `학습 경로`: 과거에 실제 dump를 열거나 분석했던 output 위치
- `등록 경로`: 사용자가 저장한 output 위치
- `자동 output 경로`: 현재 설치/설정에서 추론 가능한 툴리우스 output root

generic Windows `CrashDumps`는 자동 발견 대상에서 제외한다.

### 2. 자동 output 경로는 helper 설정과 설치 위치에서 추론한다

자동 output 경로는 `SkyrimDiagHelper.ini`와 현재 WinUI 설치 위치를 기준으로 계산한다.

- `OutputDir=`가 설정되어 있으면 그 값을 우선 사용한다.
- `OutputDir=`가 비어 있으면 기본 output 규칙을 적용한다.
- MO2 구조가 감지되면 기본 output은 `MO2 base\overwrite\SKSE\Plugins`로 간주한다.
- MO2가 아니면 기본 output은 helper exe가 있는 `SKSE\Plugins` 폴더로 간주한다.

### 3. UI 용어도 `검색 위치`보다 `출력 위치`를 우선한다

사용자-facing 용어는 `덤프 검색 위치`보다 `덤프 출력 위치`가 더 정확하다.

- empty state: `MO2 overwrite 또는 OutputDir 폴더를 추가하세요`
- 관리 패널 제목: `덤프 출력 위치`
- 상태 문구: `출력 위치 N곳, 최근 덤프 M개`

내부 구현은 기존 `roots` 모델을 재사용해도 되지만, 사용자에게는 `output location` 개념으로 보여준다.

### 4. `직접 선택`은 유지한다

generic `.dmp`를 완전히 배제하지는 않는다.

- 자동 발견은 output root만 본다.
- 예외적인 generic `.dmp` 열기는 `직접 선택`으로만 허용한다.

즉, `CrashDumps`는 더 이상 제품의 자동 intake 경로가 아니고, 수동 열기 예외만 남는다.

## Non-goals

- generic Windows crash dump 지원 확대
- 전체 디스크 스캔
- MO2 모든 변형 구조의 완전 자동 추론
- 분석 엔진 규칙 변경

## Acceptance Criteria

- 첫 화면 자동 발견에서 `CrashDumps`가 제거된다.
- `DumpDiscoveryService`가 `SkyrimDiagHelper.ini`의 `OutputDir=`를 읽는다.
- `OutputDir=`가 비어 있을 때 MO2 설치면 `overwrite\SKSE\Plugins`를 기본 output root로 사용한다.
- 사용자-facing 문구가 `dump search location`보다 `dump output location` 중심으로 바뀐다.
- `직접 선택`은 그대로 유지된다.

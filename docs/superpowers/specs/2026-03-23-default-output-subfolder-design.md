# Default Output Subfolder Design

**Date:** 2026-03-23

**Goal:** `OutputDir`를 비워 둔 기본 사용자도 MO2 `overwrite\SKSE\Plugins` 바로 아래를 어지럽히지 않도록, 기본 출력 위치를 `Tullius Ctd Logs` 하위 폴더로 재정의한다.

## Context

현재 helper는 `OutputDir`를 문자열 그대로 읽고, 비어 있으면 helper exe 폴더를 기본값으로 사용한다.

이 동작은 helper runtime 관점에서는 단순하지만, 사용자 UX 관점에서는 다음 문제가 있다.

- MO2 사용자는 기본적으로 `overwrite\SKSE\Plugins` 바로 아래에 dump/log/report가 섞여 쌓인다고 느낀다.
- `OutputDir`를 직접 설정하지 않은 사용자도 결과물을 별도 폴더에 모으고 싶어 한다.
- 상대경로를 직접 설정하도록 안내하면 `SKSE\Plugins\...`를 중복 입력하는 혼동이 반복된다.

동시에 현재 WinUI dump discovery는 `OutputDir`가 비어 있을 때만 MO2 `overwrite\SKSE\Plugins`를 기본 output root로 추론한다. 따라서 단순히 ini 기본값을 `OutputDir=Tullius Ctd Logs`로 바꾸면, helper runtime과 WinUI discovery가 서로 다른 위치를 기본값으로 해석할 위험이 있다.

## Decision

### 1. `OutputDir=` blank 계약 자체를 바꾼다

이번 변경은 ini 기본 문자열을 채우는 방식으로 하지 않는다.

- `OutputDir=`가 비어 있으면 새 기본 규칙을 적용한다.
- `OutputDir`가 명시되어 있으면 현재 계약을 유지한다.

즉 사용자 지정 경로의 의미는 바꾸지 않고, 기본값의 의미만 바꾼다.

### 2. 새 기본 출력 위치는 `Tullius Ctd Logs` 하위 폴더다

blank `OutputDir`의 새 기본 위치는 아래와 같이 정의한다.

- MO2 기준 사용자 해석: `overwrite\SKSE\Plugins\Tullius Ctd Logs`
- 비-MO2 기준 사용자 해석: `<helper exe dir>\Tullius Ctd Logs`

helper runtime 구현은 MO2 여부를 직접 계산하지 않아도 된다. helper는 기존처럼 helper exe 기준 경로를 사용하되, blank일 때 최종 기본 경로를 `ExeDir()/Tullius Ctd Logs`로 계산한다. MO2 환경에서는 이 경로에 대한 쓰기가 기존 제품 UX와 동일하게 overwrite 쪽 결과로 나타난다고 본다.

### 3. WinUI discovery는 새 기본 하위 폴더를 기본 root로 추론한다

WinUI 자동 발견 규칙도 helper의 새 blank 계약과 맞춘다.

- `OutputDir`가 명시되어 있으면 기존처럼 그 값을 우선 사용한다.
- `OutputDir`가 비어 있고 MO2 구조가 감지되면 기본 output root를 `MO2 base\overwrite\SKSE\Plugins\Tullius Ctd Logs`로 추론한다.
- `OutputDir`가 비어 있고 MO2가 아니면 기본 output root를 `helperDirectoryPath\Tullius Ctd Logs`로 추론한다.

즉 blank 상태의 기본 자동 발견 위치도 더 이상 루트 `SKSE\Plugins`가 아니라 새 하위 폴더가 된다.

### 4. 전환기에는 legacy 기본 위치도 함께 발견한다

기존 버전이 남긴 dump/log는 이전 기본 위치에 남아 있을 수 있다.

- MO2 legacy 기본 위치: `MO2 base\overwrite\SKSE\Plugins`
- 비-MO2 legacy 기본 위치: `helperDirectoryPath`

새 버전에서는 discovery가 새 기본 하위 폴더를 우선 대상으로 삼되, 전환기 호환을 위해 legacy 기본 위치도 보조 자동 root로 포함한다.

이렇게 하면 업데이트 직후에도 예전 dump를 바로 찾을 수 있고, 새 dump는 새 하위 폴더에 모인다.

### 5. 문서/ini는 “풀경로 불필요”와 “blank = 기본 하위 폴더”를 명확히 쓴다

사용자 안내는 다음 계약에 맞춰 정리한다.

- blank `OutputDir`는 금지가 아니라 권장 기본값이다.
- blank면 기본 출력 위치 아래의 `Tullius Ctd Logs` 폴더를 사용한다.
- 별도 위치가 필요하면 절대경로나 상대경로를 직접 지정할 수 있다.
- 같은 기본 위치 아래 새 폴더만 원하면 `OutputDir=Some Folder Name`처럼 폴더명만 쓰면 된다.

## Architecture

### Files

- Modify: `helper/src/Config.cpp`
- Modify: `helper/include/SkyrimDiagHelper/Config.h` if a helper function or explicit default-path contract is introduced there
- Modify: `dump_tool_winui/DumpDiscoveryService.cs`
- Modify: `dist/SkyrimDiagHelper.ini`
- Modify: `README.md`
- Modify: `docs/README_KO.md`
- Modify: `docs/nexus-description.bbcode`
- Modify: `docs/BETA_TESTING.md`
- Modify: `tests/winui_xaml_tests.cpp`
- Add or modify helper/source-guard tests covering the new blank-default contract

### Runtime path contract

권장 구현은 `Config.cpp`에서 blank `OutputDir`를 처리할 때 명시적인 기본 경로 계산 함수를 도입하는 것이다.

예시 계약:

- `ResolveEffectiveOutputDir(rawOutputDir)`
- raw가 blank면 `ExeDir()/kDefaultOutputSubdir`
- raw가 non-blank면 raw를 그대로 유지

이렇게 하면 출력 경로 결정이 한 곳에 모이고, `HelperCommon.cpp`는 이미 계산된 `cfg.outputDir`만 사용하면 된다.

### Discovery contract

`DumpDiscoveryService`는 자동 root를 단일 값이 아니라 “우선순위가 있는 후보 집합”으로 다루는 편이 낫다.

우선순위:

1. configured `OutputDir`
2. new blank default root
3. legacy blank default root

이 순서를 쓰면 새 정책이 기본이 되면서도 이전 산출물도 놓치지 않는다.

## Risks And Mitigations

### 1. helper runtime과 discovery가 다시 어긋날 수 있다

대응:

- blank `OutputDir` 계약을 문서와 코드에서 하나로 명시한다.
- helper와 WinUI가 같은 상수명/문구를 공유하지 못하더라도, 테스트에서 동일 계약을 강제한다.

### 2. 업데이트 직후 사용자가 파일이 “사라졌다”고 느낄 수 있다

대응:

- discovery에 legacy 기본 위치를 같이 포함한다.
- 릴리즈 노트와 ini 주석에 새 기본 폴더 이름을 명시한다.

### 3. MO2 VFS 해석이 설치 형태에 따라 다를 수 있다

대응:

- helper runtime은 기존과 같은 exe-dir 기준 쓰기 모델을 유지한다.
- Windows/MO2 smoke에서 실제 생성 위치를 한 번 확인한다.
- 문서 문구는 “기본 출력 위치 아래의 하위 폴더” 표현을 쓰고, 구현 검증 결과에 맞춰 최종 릴리즈 문구를 다듬는다.

## Non-goals

- 사용자가 명시한 `OutputDir` 상대경로 기준 변경
- 전체 경로 시스템 재설계
- Vortex/기타 매니저용 별도 기본 정책 추가
- retention 정책 변경

## Acceptance Criteria

- blank `OutputDir`일 때 새 산출물은 기본 `Tullius Ctd Logs` 하위 폴더로 생성된다.
- 명시적 `OutputDir` 사용자는 기존 동작이 바뀌지 않는다.
- WinUI 자동 발견은 새 기본 하위 폴더를 우선 자동 root로 사용한다.
- WinUI 자동 발견은 legacy 기본 위치 dump도 계속 찾을 수 있다.
- 기본 ini/README/넥서스 설명이 blank 의미와 상대경로 사용법을 새 계약에 맞게 설명한다.
- Windows MO2 smoke로 실제 생성/발견 위치를 확인한다.

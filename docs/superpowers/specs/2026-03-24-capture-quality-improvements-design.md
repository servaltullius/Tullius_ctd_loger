# Capture Quality Improvements Design

**Date:** 2026-03-24

**Goal:** CTD 분석 품질을 높이기 위해 helper 쪽 dump capture를 `mode-only` 구조에서 `richer flags + callback-shaped dump` 구조로 확장한다.

## Context

현재 helper capture 경로는 [DumpProfile.h](/home/kdw73/Tullius_ctd_loger/helper/include/SkyrimDiagHelper/DumpProfile.h), [DumpProfile.cpp](/home/kdw73/Tullius_ctd_loger/helper/src/DumpProfile.cpp), [DumpWriter.cpp](/home/kdw73/Tullius_ctd_loger/helper/src/DumpWriter.cpp)에 집중되어 있다.

현 상태의 한계는 두 가지다.

- profile이 사실상 `thread_info`, `handle_data`, `unloaded_modules`, `code_segments`, `full_memory`만 구분한다.
- `MiniDumpCallback`은 [DumpWriter.cpp](/home/kdw73/Tullius_ctd_loger/helper/src/DumpWriter.cpp) 에서 `IsProcessSnapshotCallback`만 처리하고, 실제 thread/module shaping에는 아직 쓰이지 않는다.

즉 지금은 `작은 덤프 / 큰 덤프` 정도의 차이만 있고, CTD triage에 필요한 `richer but targeted capture`가 부족하다.

## Decision

### 1. 이번 단계는 CTD 중심 capture quality 개선이다

범위 포함:

- crash capture
- crash recapture
- EXE/system victim CTD용 richer minidump flags
- callback 기반 thread shaping의 첫 단계

범위 제외:

- freeze/PSS snapshot 재설계
- WCT consensus 재설계
- full-memory 기본값 변경

이번 단계의 목적은 `DumpMode=2`를 기본으로 올리는 것이 아니라, `DumpMode=1`과 recapture 품질을 더 좋게 만드는 것이다.

### 2. DumpProfile contract를 richer minidump flags까지 확장한다

추가할 profile 플래그:

- `includeProcessThreadData`
- `includeFullMemoryInfo`
- `includeModuleHeaders`
- `includeIndirectMemory`
- `ignoreInaccessibleMemory`

의도는 이렇다.

- 기본 crash/default capture는 `FullMemory` 없이도 thread/process/module memory metadata를 더 많이 보존한다.
- crash recapture는 한 단계 더 rich 하게 가져가되, `FullMemory`로 바로 뛰지 않는다.
- analyzer가 fault-module, stackwalk, indirect references를 더 안정적으로 소비할 수 있는 기반을 만든다.

### 3. capture profile은 `Crash`와 `CrashRecapture`를 다르게 튜닝한다

권장 profile:

- `Crash + DumpMode::kDefault`
  - `ThreadInfo`
  - `HandleData`
  - `UnloadedModules`
  - `CodeSegs`
  - `ProcessThreadData`
  - `FullMemoryInfo`
  - `ModuleHeaders`

- `CrashRecapture + DumpMode::kDefault`
  - 위 기본 crash flags 전부
  - `IndirectlyReferencedMemory`
  - `IgnoreInaccessibleMemory`

- `DumpMode::kFull`
  - 기존처럼 `MiniDumpWithFullMemory` 포함

즉 `CrashRecapture`는 기본 crash보다 richer 하되, `FullMemory`는 여전히 별도 escalation로 유지한다.

### 4. callback shaping은 “과감한 제외”보다 “핵심 포함 보장”부터 시작한다

첫 단계 callback 목적:

- crash thread를 확실히 우선한다
- main thread 선호를 반영한다
- process snapshot 여부를 기존처럼 유지한다

이번 단계에서는 callback을 써서 대량 배제를 하려는 것이 아니다. 첫 단계는 `preferred crash/main thread` 중심으로 future shaping 기반을 마련하는 수준이 맞다.

이유:

- 현재 helper는 callback-driven exclusion 정책이 전혀 없어서, 갑자기 aggressive filtering으로 가면 회귀 위험이 크다.
- CTD 품질 개선의 첫 라운드는 `더 필요한 것 포함`이지 `더 많이 버리기`가 아니다.

### 5. analyzer 쪽 변경은 최소화한다

이번 단계는 capture quality 자체가 목적이므로, analyzer 쪽 변경은 최소 contract 유지 수준으로 둔다.

필수 연결:

- incident manifest/profile metadata에 새 플래그 노출
- source guard/tests에서 richer flags와 callback shaping 시작을 확인

비포함:

- CTD ranking/scoring 재설계
- summary/recommendation wording 변경

## Architecture

### A. DumpProfile

대상 파일:

- [DumpProfile.h](/home/kdw73/Tullius_ctd_loger/helper/include/SkyrimDiagHelper/DumpProfile.h)
- [DumpProfile.cpp](/home/kdw73/Tullius_ctd_loger/helper/src/DumpProfile.cpp)

책임:

- capture kind별 flag set을 선언적으로 유지
- `Crash`, `Hang`, `Manual`, `CrashRecapture` 간 차이를 profile contract로 보이게 한다

### B. DumpWriter

대상 파일:

- [DumpWriter.cpp](/home/kdw73/Tullius_ctd_loger/helper/src/DumpWriter.cpp)

책임:

- profile flags를 `MINIDUMP_TYPE`으로 변환
- callback entrypoint를 `IsProcessSnapshotCallback` 전용에서 richer shaping 기반으로 확장
- first-stage shaping은 crash/main thread 선호 정도만 반영

### C. Manifest / reporting metadata

대상 파일:

- [IncidentManifest.cpp](/home/kdw73/Tullius_ctd_loger/helper/src/IncidentManifest.cpp)
- 관련 summary/report source guards

책임:

- 어떤 profile flags로 캡처되었는지 나중에 분석/디버깅할 수 있게 남긴다

## Recommended Phases

### Phase 1: profile contract 확장

- `DumpProfile` 새 플래그 추가
- `ResolveDumpProfile(...)`에 crash / recapture richer profile 반영
- `dump_profile_tests.cpp` source guard 갱신

### Phase 2: dump type wiring

- `ApplyProfileToDumpType(...)`에 new `MINIDUMP_TYPE` 플래그 추가
- `dump_writer_guard_tests.cpp` source guard 갱신

### Phase 3: callback shaping bootstrap

- callback context에 preferred thread intent 유지
- `IncludeThreadCallback` 또는 equivalent shaping branch를 첫 단계로 도입
- process snapshot callback은 기존 동작 유지

### Phase 4: metadata verification

- incident manifest/profile JSON에 새 flag 노출 유지
- 관련 schema/source guard 확인

## Success Criteria

- 기본 crash capture가 `FullMemory` 없이도 richer process/thread/module metadata를 담는다
- crash recapture가 default crash보다 한 단계 richer 하다
- callback이 더 이상 snapshot passthrough만 하는 구조가 아니다
- Linux source guard tests와 Windows helper build가 모두 통과한다

## Non-Goals

- `DumpMode=2` 기본화
- freeze snapshot redesign
- analyzer scoring 변경
- user-facing wording 변경

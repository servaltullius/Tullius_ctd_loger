# Engine Capture And Analysis Accuracy Design

**Date:** 2026-03-13

**Goal:** CTD와 프리징 발생 시 helper가 남기는 증거 품질을 높이고, dump_tool이 그 증거를 실제 원인 판정에 더 많이 반영하도록 엔진 개선 우선순위를 정한다.

## Context

현재 제품은 이미 다음 기반을 갖고 있다.

- out-of-proc helper가 크래시/프리징을 감지하고 dump를 쓴다.
- helper는 WCT JSON, blackbox, plugin scan, incident manifest를 함께 남긴다.
- analyzer는 dump, WCT, blackbox, Crash Logger log, plugin scan, crash history를 읽어 triage를 만든다.
- unknown crash bucket 반복 시 full recapture도 일부 자동화돼 있다.

즉, 현재 문제는 "엔진이 없다"가 아니다. 현재 병목은 아래 두 가지다.

- 캡처 계층이 `DumpMode=0/1/2`의 너무 거친 3단 모델에 묶여 있다.
- 분석 계층이 helper가 남길 수 있는 추가 신호를 아직 충분히 후보 점수에 쓰지 않는다.

현재 관련 핵심 파일:

- helper config / dump 생성:
  - `helper/include/SkyrimDiagHelper/Config.h`
  - `helper/src/Config.cpp`
  - `helper/src/DumpWriter.cpp`
  - `helper/src/CrashCapture.cpp`
  - `helper/src/HangCapture.cpp`
  - `helper/src/ManualCapture.cpp`
  - `helper/src/PendingCrashAnalysis.cpp`
- helper preflight:
  - `helper/src/CompatibilityPreflight.cpp`
- analyzer:
  - `dump_tool/src/Analyzer.h`
  - `dump_tool/src/Analyzer.cpp`
  - `dump_tool/src/CandidateConsensus.cpp`
  - `dump_tool/src/EvidenceBuilderEvidence.cpp`
  - `dump_tool/src/EvidenceBuilderRecommendations.cpp`
  - `dump_tool/src/OutputWriter.cpp`

## Decision

### 1. 전면 교체 대신 `capture-profile + analyzer-consumption` 강화로 간다

현 구조는 이미 올바른 방향의 MVP다. 따라서 Crashpad/Breakpad 같은 완전 다른 엔진으로 갈아타지 않는다.

대신 기존 helper/dump_tool에 아래 순서로 개선을 넣는다.

1. 사고 유형별 dump profile
2. MiniDump callback shaping
3. symbol/runtime preflight 강화
4. profile-aware recapture
5. blackbox/WCT 추가 신호를 analyzer 후보 점수에 반영
6. hang/manual의 PSS snapshot 경로는 2차 작업으로 분리

### 2. `DumpMode`는 유지하되, 내부 모델은 `incident-specific profiles`로 바꾼다

사용자 설정의 단순성을 위해 ini의 `DumpMode=0/1/2`는 당장 제거하지 않는다. 대신 내부에서 이를 직접 `MINIDUMP_TYPE`에 매핑하지 않고, 각 사고 유형별 기본 profile을 고르는 상위 입력으로만 사용한다.

내부적으로는 최소 아래 프로필이 필요하다.

- `CrashDefaultProfile`
- `HangDefaultProfile`
- `ManualSnapshotProfile`
- `CrashFullRecaptureProfile`

예상 규칙:

- crash는 stack/thread/module 품질을 우선
- hang/manual은 WCT/대기 상태/문맥 보존을 우선
- recapture는 필요한 경우에만 더 풍부한 메모리 범위를 포함

이 변경의 목적은 `DumpMode=2`를 자주 요구하지 않고도, CTD와 프리징 각각에 더 맞는 증거를 남기는 것이다.

### 3. `MiniDumpWriteDump` callback을 1차 핵심 개선으로 둔다

현재 dump 생성은 callback 없이 단순 호출된다. 이 상태에서는 필요한 스레드/모듈/메모리만 더 넣는 정밀 조정이 어렵다.

따라서 1차 개선의 핵심은 `DumpWriter`에 callback shaping을 도입하는 것이다.

포함 우선순위는 아래와 같다.

- crash thread
- main thread 추정 스레드
- WCT cycle thread / wait-chain 핵심 스레드
- unloaded modules
- process/thread data
- 분석 가치가 높은 VM region

핵심 원칙:

- full memory를 기본값으로 올리지 않는다.
- triage dump를 더 똑똑하게 만든다.
- incident manifest에 실제 사용된 profile/flags/callback 결과를 기록한다.

### 4. recapture 정책은 `unknown fault module` 단일 기준에서 확장한다

현재 auto recapture는 unknown fault module bucket 반복에 크게 의존한다.

이 기준은 유지하되, 다음 상태도 recapture 입력에 추가한다.

- candidate conflict가 반복되는 사고
- object-ref only가 반복되는 사고
- stackwalk failure / stack-scan fallback 반복
- symbol environment degradation이 겹친 사고

또한 recapture는 항상 full dump로 가지 않고, richer crash profile부터 시도하도록 바꾼다.

### 5. preflight는 `모드 호환성`뿐 아니라 `분석 가능성`도 다뤄야 한다

현재 preflight는 plugin 충돌, BEES, crash logger 중복 등은 잘 본다. 하지만 실제 원인 분석 정확도에 큰 영향을 주는 `DbgHelp`, `DIA/msdia`, symbol cache/path 품질은 약하다.

따라서 preflight 범위를 확장한다.

- 현재 로딩될 `dbghelp.dll` 버전/경로 점검
- `msdia140.dll` 가용성 점검
- online/offline symbol policy와 실제 analyzer diagnostics 연결
- symbol cache/path misconfiguration 탐지

이 변경은 "엔진이 원인을 못 찾은 것"과 "심볼 환경이 나빠서 분석이 약한 것"을 구분해 준다.

### 6. blackbox 추가 신호는 helper와 analyzer를 함께 바꿀 때만 가치가 있다

향후 추가할 가치가 큰 신호:

- 최근 first-chance exception 샘플
- DLL load/unload
- thread create/exit
- hang 시 async WCT 요약

그러나 수집만 늘리는 것은 금지한다. 새 신호는 반드시 analyzer 후보 점수와 evidence 표현에 연결되어야 한다.

즉 helper와 analyzer는 아래 계약으로 묶인다.

- helper가 새 신호를 남긴다.
- analyzer가 그 신호를 원인 후보/충돌 근거/재수집 권고에 반영한다.
- UI는 왜 그 후보를 더 강하게 봤는지 설명한다.

### 7. `PSS snapshot`은 프리징 품질 향상용 2차 작업으로 분리한다

hang/manual capture는 현재도 live process 기준 WCT + dump를 남긴다. 이 경로 위에 PSS snapshot 기반 export를 얹는 것은 가치가 있지만, 구현/검증 리스크가 크다.

따라서 PSS는 별도 phase로 둔다.

이 phase의 목적:

- freeze 중 live process에 주는 부담을 줄인다.
- snapshot 시점의 thread/wait 상태를 더 안정적으로 보존한다.
- WCT와 dump를 더 일관된 시점 정보로 묶는다.

## Recommended Phases

### Phase 1: CTD 정확도 개선

- incident-specific dump profile 도입
- MiniDump callback shaping 도입
- incident manifest에 effective profile/flags 기록
- analyzer diagnostics에 stackwalk/symbol 품질 설명 보강
- recapture 정책을 profile-aware로 확장

이 phase는 CTD, 특히 `EXE/system victim`, `fault module 불명`, `stackwalk fallback` 케이스의 정확도 개선에 직접적이다.

### Phase 2: Freeze 정확도 개선

- preflight에 symbol/runtime health 추가
- blackbox에 first-chance / module / thread 신호 추가
- analyzer가 새 신호를 candidate scoring에 반영
- hang/manual 경로에 PSS snapshot spike 또는 도입

이 phase는 deadlock, busy wait, loader stall, infinite loading 구분을 개선하는 데 집중한다.

## Tradeoffs

### 채택한 방향

- 기존 helper/dump_tool 유지
- 단계적 개선
- dump 크기와 분석 품질의 균형 추구

### 기각한 방향

- `DumpMode=2 FullMemory`를 기본값으로 올리는 것
  - 이유: 파일 크기와 캡처 비용이 과도하고, CTD 원인 분석 품질을 반드시 보장하지 않는다.
- WER/CrashDumps를 메인 intake/capture 경로로 키우는 것
  - 이유: 현재 제품의 주 경로는 helper output이며, WER는 fallback이면 충분하다.
- 다른 crash-reporting 엔진으로 교체
  - 이유: ROI가 낮고, 현재 구조가 이미 필요한 핵심을 상당 부분 갖추고 있다.

## Risks And Mitigations

### 1. profile/callback 복잡도 증가

위험:
- helper dump 생성 코드가 급격히 복잡해질 수 있다.

대응:
- `profile selection`과 `MiniDump callback wiring`을 분리된 유닛으로 쪼갠다.
- incident manifest에 effective flags를 남겨 디버깅 가능하게 한다.

### 2. 새 신호를 analyzer가 못 쓰는 half-finished 상태

위험:
- helper만 복잡해지고 체감 정확도는 안 오를 수 있다.

대응:
- 각 helper 신호 추가는 반드시 analyzer/evidence/candidate scoring 변경과 같은 phase에서 다룬다.

### 3. freeze 경로의 회귀

위험:
- hang/manual capture는 timing-sensitive라 PSS 도입 시 회귀 위험이 높다.

대응:
- PSS는 phase 2로 분리하고 spike 뒤에 채택 여부를 결정한다.

## Non-goals

- generic Windows crash dump 수집 확대
- Crashpad/Breakpad 전환
- analyzer 전체 재작성
- phase 1에서의 PSS snapshot 즉시 도입

## Acceptance Criteria

- helper가 crash/hang/manual/recapture에 대해 서로 다른 effective dump profile을 선택할 수 있다.
- dump writer가 callback을 사용해 triage dump shaping을 수행한다.
- incident manifest에 effective profile/flags가 남는다.
- recapture 정책이 unknown fault module 외의 분석 약점도 입력으로 받을 수 있다.
- preflight가 symbol/runtime health를 별도 체크로 출력한다.
- analyzer diagnostics와 evidence가 새 신호의 존재를 설명한다.
- phase 1 완료 후 `DumpMode=2로 다시 캡처` 권고 빈도가 감소한다.

# Freeze PSS Quality Improvements Design

**Date:** 2026-03-25

**Goal:** freeze/hang/manual capture 품질을 `opt-in PSS snapshot + WCT live capture`의 단발 경로에서 `richer snapshot flags + WCT consensus metadata + analyzer consumption` 구조로 보강한다.

## Context

현재 freeze 계열 품질은 세 층으로 나뉘어 있다.

- helper의 [PssSnapshot.cpp](/home/kdw73/Tullius_ctd_loger/helper/src/PssSnapshot.cpp)는 `PSS_CAPTURE_VA_CLONE | PSS_CAPTURE_THREADS | PSS_CAPTURE_THREAD_CONTEXT`만 사용한다.
- helper의 [WctCapture.cpp](/home/kdw73/Tullius_ctd_loger/helper/src/WctCapture.cpp)는 각 thread에 대해 WCT를 한 번만 캡처하고, 그 결과를 단발 JSON으로 남긴다.
- analyzer의 [FreezeCandidateConsensus.cpp](/home/kdw73/Tullius_ctd_loger/dump_tool/src/FreezeCandidateConsensus.cpp)는 `cycles`, `isLoading`, `pss_snapshot_used`, `dump_transport` 정도만 읽어 `deadlock_likely`, `loader_stall_likely`, `freeze_candidate`, `freeze_ambiguous`를 계산한다.

즉 지금 병목은 두 가지다.

- freeze snapshot 경로가 `thread context` 중심이라 address-space/section 관점의 freeze 품질을 충분히 확보하지 못한다.
- WCT가 단발 캡처라 cycle/load 정황이 일시적 노이즈인지 반복 일치인지 구분하지 못한다.

따라서 이번 단계는 `PSS default-on` 같은 공격적 전환이 아니라, **capture 품질을 더 잘 기록하고 analyzer가 그 차이를 실제 confidence에 반영하게 만드는 것**이 목표다.

## Decision

### 1. 이번 단계는 freeze 품질의 `balanced` 개선이다

범위 포함:

- hang/manual freeze capture
- opt-in PSS snapshot flags 확장
- WCT 2-pass consensus metadata 추가
- analyzer의 freeze confidence / reason 강화

범위 제외:

- PSS snapshot default-on 승격
- CTD scoring 변경
- handle-heavy PSS capture
- WinUI 구조 변경

이번 단계의 목적은 `freeze capture richness`와 `freeze analysis stability`를 같이 높이는 것이다.

### 2. PSS snapshot은 opt-in 유지하되 richer flags를 추가한다

현재 `kFreezeSnapshotFlags`는 아래 세 개뿐이다.

- `PSS_CAPTURE_VA_CLONE`
- `PSS_CAPTURE_THREADS`
- `PSS_CAPTURE_THREAD_CONTEXT`

이번 단계에서 추가할 플래그:

- `PSS_CAPTURE_VA_SPACE`
- `PSS_CAPTURE_VA_SPACE_SECTION_INFORMATION`

의도:

- freeze snapshot이 단순 thread dump가 아니라 address-space / section layout까지 보존하도록 한다.
- loader stall, mapped image/section 정황, module/region stability를 더 잘 설명할 수 있는 기반을 만든다.

이번 단계에서는 `PSS_CAPTURE_HANDLES`와 type-specific handle capture는 넣지 않는다. Skyrim freeze에서는 비용 대비 노이즈가 더 클 가능성이 높기 때문이다.

### 3. WCT는 단발 결과 대신 `2회 합의` 메타데이터를 남긴다

현행 WCT capture는 각 thread마다 한 번만 `GetThreadWaitChain(...)`을 호출하고 그 순간의 cycle/load 정황을 저장한다.

이번 단계에서는 helper가 같은 PID에 대해 짧은 간격으로 **2회 WCT 캡처**를 수행하고, 원문 thread 배열은 유지하되 capture-level summary를 추가한다.

추가할 summary metadata:

- `capture_passes`
- `cycle_consensus`
- `repeated_cycle_tids`
- `consistent_loading_signal`
- `longest_wait_tid_consensus`

의도:

- `deadlock_likely`는 일회성 cycle보다 `반복 일치하는 cycle`을 더 강하게 신뢰한다.
- `loader_stall_likely`는 loading signal이 한 번만 뜬 경우보다 `두 번 연속 일치`한 경우를 더 강하게 본다.

### 4. analyzer는 새 품질 차이를 실제 freeze confidence에 반영한다

현재 [FreezeCandidateConsensus.cpp](/home/kdw73/Tullius_ctd_loger/dump_tool/src/FreezeCandidateConsensus.cpp)는 아래 정도만 사용한다.

- `pss_snapshot_used`
- `pss_snapshot_requested`
- `cycles`
- `isLoading`

이번 단계에서 analyzer는 아래를 실제로 소비한다.

- richer PSS flags 사용 여부
- `cycle_consensus`
- `repeated_cycle_tids`
- `consistent_loading_signal`
- `longest_wait_tid_consensus`

원칙:

- snapshot 사용 여부만으로 deadlock/stall을 판정하지 않는다
- consensus가 반복될수록 confidence를 높인다
- richer PSS flags는 state 승격이 아니라 `support_quality`와 reason 강화에 더 가깝게 사용한다

### 5. output은 새 freeze capture 품질을 설명해야 한다

이번 단계의 출력층 목표는 “왜 이 freeze 판정이 이전보다 더 믿을 만한가”를 보여주는 것이다.

추가/강화할 정보:

- `support_quality`가 `snapshot_backed`, `snapshot_consensus_backed`, `snapshot_fallback`, `live_process`처럼 더 구체화
- summary/report evidence에 `WCT consensus`와 richer snapshot flags 반영
- recommendation에 `single-pass live WCT`인지 `snapshot-backed consensus`인지 구분

즉 사용자 출력은 `cycle thread가 있었다` 수준을 넘어, `두 번의 WCT에서 같은 cycle이 반복됐고 richer snapshot이 backing 했다` 같은 설명을 할 수 있어야 한다.

## Architecture

### A. PSS capture

대상 파일:

- [PssSnapshot.cpp](/home/kdw73/Tullius_ctd_loger/helper/src/PssSnapshot.cpp)
- [PssSnapshot.h](/home/kdw73/Tullius_ctd_loger/helper/src/PssSnapshot.h) 또는 관련 선언부

책임:

- freeze snapshot flags 확장
- richer snapshot 사용 여부를 status metadata로 유지

### B. WCT capture

대상 파일:

- [WctCapture.cpp](/home/kdw73/Tullius_ctd_loger/helper/src/WctCapture.cpp)

책임:

- 2회 live WCT 캡처
- pass별 thread 결과는 유지
- consensus summary를 capture-level JSON에 추가

### C. WCT parsing / freeze consensus

대상 파일:

- [WctTypes.h](/home/kdw73/Tullius_ctd_loger/dump_tool/src/WctTypes.h)
- [AnalyzerInternalsWct.cpp](/home/kdw73/Tullius_ctd_loger/dump_tool/src/AnalyzerInternalsWct.cpp)
- [FreezeCandidateConsensus.cpp](/home/kdw73/Tullius_ctd_loger/dump_tool/src/FreezeCandidateConsensus.cpp)

책임:

- new WCT summary fields 파싱
- consensus-backed cycle/loading 판정
- richer support quality 산정

### D. Evidence / output

대상 파일:

- [EvidenceBuilderEvidence.Context.cpp](/home/kdw73/Tullius_ctd_loger/dump_tool/src/EvidenceBuilderEvidence.Context.cpp)
- [EvidenceBuilderRecommendations.cpp](/home/kdw73/Tullius_ctd_loger/dump_tool/src/EvidenceBuilderRecommendations.cpp)
- [OutputWriter.Summary.cpp](/home/kdw73/Tullius_ctd_loger/dump_tool/src/OutputWriter.Summary.cpp)
- [OutputWriter.Report.cpp](/home/kdw73/Tullius_ctd_loger/dump_tool/src/OutputWriter.Report.cpp)

책임:

- freeze capture 품질과 WCT consensus를 실제 근거 문장으로 노출

## Recommended Phases

### Phase 1: PSS flags contract 확장

- freeze snapshot flags를 `VA_SPACE`와 `SECTION_INFORMATION`까지 확장
- source guard/tests에서 richer PSS flags를 고정

### Phase 2: WCT consensus capture 추가

- helper가 2회 WCT 캡처
- capture-level consensus summary JSON 추가
- parser/runtime tests 갱신

### Phase 3: freeze consensus consumption

- analyzer가 `cycle_consensus`, `consistent_loading_signal`, `repeated_cycle_tids`를 읽음
- `support_quality`, confidence, primary reasons 조정

### Phase 4: output propagation

- summary/report/evidence/recommendation에 새 품질 근거 노출

## Testing Strategy

- helper guard:
  - PSS richer flags source guard
  - WCT consensus summary source guard
- runtime:
  - `wct_parsing_tests.cpp`
  - `freeze_candidate_consensus_tests.cpp`
  - `analysis_engine_runtime_tests.cpp`
- output:
  - `output_snapshot_tests.cpp`
- verification:
  - Linux full test suite
  - Windows helper build
  - Windows WinUI build

## Success Criteria

- opt-in freeze PSS snapshot이 address-space/section metadata까지 담는다
- WCT JSON이 2회 capture consensus metadata를 남긴다
- `deadlock_likely`가 반복 cycle 합의에 더 민감해진다
- `loader_stall_likely`가 repeated loading signal에 더 민감해진다
- output이 `single-pass live capture`와 `snapshot-backed consensus`를 구분해 설명한다

## Non-Goals

- PSS snapshot default-on 전환
- handle-heavy PSS capture
- CTD candidate scoring 변경
- WinUI 카드/레이아웃 재설계

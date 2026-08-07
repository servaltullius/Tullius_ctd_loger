# ADR-0005: Hang Main-Thread and Thread-Group Consensus

## Status
Accepted

## Context
프리징 덤프에서 정식 DbgHelp 스택 워크가 실패하면 기존 분석기는 WCT 후보
스레드 여러 개의 스택 메모리를 포인터 스캔했습니다. 이 방식은 스레드를 많이
생성한 모드나 직전에 로드된 리소스 제공자를 원인처럼 과대평가할 수 있습니다.

2026-08-07 실제 프리징 덤프에서는 기존 결과가 SkyrimNet과 Spark Patch를
지목했지만, 메인 스레드와 정지된 워커 그룹에는 `hdtsmp64.dll`이 공통으로
나타났습니다. 두 WCT 패스 사이 이 스레드들의 context-switch 수 역시 변하지
않았습니다.

## Decision

1. 프리징 분석의 기준 스레드는 최신 `Heartbeat` 이벤트의 스레드 ID로 정한다.
   Heartbeat가 없으면 `SessionStart` 이벤트를 하위 호환용 기준으로 사용한다.
2. 헬퍼의 덤프 생성과 DumpTool 분석 모두 이 메인 스레드를 우선한다.
3. 프리징에서 정식 스택 워크가 실패하면 포인터 스캔은 메인 스레드만 대상으로
   하며, 결과는 항상 낮은 신뢰도의 약한 단서로 표시한다.
4. 리소스 제공자 신호는 다른 실행 증거를 보강할 수 있지만, 단독으로 실행 가능한
   원인 후보가 될 수 없다.
5. 다음 조건을 모두 만족하면 모듈 단위 스레드 그룹 합의를 인정한다.
   - 메인 스레드를 포함한 최소 4개 스레드의 현재 스택 상단 32 슬롯 안에 같은
     모듈 주소가 있다.
   - 첫 번째와 마지막 WCT 패스가 모두 사용 가능하다.
   - 해당 스레드 전부의 context-switch 수가 두 패스 사이 변하지 않는다.
6. 스레드 그룹 합의는 `synchronization_stall_likely`와 중간 신뢰도를 부여한다.
   WCT가 실제 순환 대기를 보고하지 않았다면 OS 잠금 사이클이 입증됐다고 표현하지
   않는다.

## Output Contract

요약 JSON의 `freeze_analysis.thread_module_consensus`에는 다음이 기록된다.

- `has_consensus`
- `main_thread_id`
- `module_filename`
- `matching_thread_count`
- `stable_thread_count`
- `os_lock_cycle_proven`

텍스트 리포트와 WinUI는 동일한 의미를 사용하며, 포인터 스캔과 정식 스택 워크의
신뢰도 차이를 명시한다.

## Consequences

### Positive

- 스레드 수가 많은 플러그인과 직전 리소스 제공자의 오탐 가능성을 낮춘다.
- 메인 스레드와 워커 그룹이 함께 멈춘 모듈 수준 동기화 정지를 별도로 설명한다.
- 관찰된 정지와 입증되지 않은 OS 잠금 사이클을 구분한다.

### Limitations

- 모듈 귀속은 함수 의미나 데드락의 정확한 코드 위치를 증명하지 않는다.
- 중간 신뢰도 판정은 실제 게임 플레이 재현이나 모드 격리 A/B 테스트를 대체하지
  않는다.
- `SessionStart` fallback은 기존 SharedLayout v4 이벤트를 재사용하며 공유 메모리
  레이아웃을 변경하지 않는다.

## Verification

- 단위/회귀 테스트로 메인 스레드 우선순위, 약한 포인터 스캔, 리소스 단독 후보
  억제, WCT 안정성 합의를 검증한다.
- 실제 덤프 승인 테스트에서는 `hdtsmp64.dll`이 메인 스레드와 15개 워커에
  반복되고, 기존 SkyrimNet/Spark Patch는 실행 가능한 후보에서 제외되는지 확인한다.
- 실제 덤프 자체는 저장소에 포함하지 않는다.

## References

- `dump_tool/src/Analyzer.CaptureInputs.cpp`
- `dump_tool/src/AnalyzerInternalsStackScan.cpp`
- `dump_tool/src/AnalyzerInternalsWct.cpp`
- `dump_tool/src/FreezeCandidateConsensus.cpp`
- `helper/src/DumpWriter.cpp`
- `docs/adr/0004-sharedlayout-versioning-and-compatibility-policy.md`

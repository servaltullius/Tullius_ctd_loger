# Analyzer/OutputWriter/HangCapture Cleanup Design

## Goal
최근 기능 추가로 다시 비대해진 세 파일을 동작 변경 없이 책임 기준으로 분리한다.

- `dump_tool/src/Analyzer.cpp`
- `dump_tool/src/OutputWriter.cpp`
- `helper/src/HangCapture.cpp`

이번 라운드는 구조 정리만 다루며, 분석 결과/문구/JSON schema/WinUI binding/CI 동작은 바꾸지 않는다.

## Scope

포함:
- `Analyzer.cpp`의 helper를 파싱/통합/히스토리 경계로 분리
- `OutputWriter.cpp`의 summary/report/write 경계를 분리
- `HangCapture.cpp`의 guard/confirmed capture 경계를 분리
- 기존 테스트와 Windows 빌드로 회귀 확인

제외:
- `Analyzer.h` public contract 변경
- `OutputWriter` summary schema 변경
- `ManualCapture.cpp` 구조 정리
- 점수/정책/추천 문구 변경
- WinUI 코드 변경

## Design

### 1. Analyzer 분리

현재 `Analyzer.cpp`에는 아래 책임이 같이 들어 있다.

- exception/fault module/blackbox/plugin scan 파싱
- CrashLogger 통합
- suspect 계산
- incident manifest / crash history 로드 및 저장
- 최종 orchestration

이 파일은 public entrypoint `AnalyzeDump(...)`를 유지한 채, helper만 별도 compilation unit으로 분리한다.

예상 파일 구조:
- `Analyzer.cpp`
  - `NowIso8601Utc`
  - `ApplyCrashLoggerCorroborationToSuspects`
  - `AnalyzeDump` orchestration
- `Analyzer.CaptureInputs.cpp`
  - exception/fault module
  - blackbox/plugin scan
  - hang-like 판단
  - CrashLogger 통합
  - suspect 계산
- `Analyzer.History.cpp`
  - incident capture profile load
  - crash history path / load / append
  - history candidate key 수집
- `AnalyzerPipeline.h`
  - 위 helper 선언

원칙:
- `AnalysisResult`와 `AnalyzeOptions`는 그대로 유지
- `AnalyzeDump`의 단계 순서 유지
- static helper를 옮기되 함수 이름과 동작은 가능하면 그대로 유지

### 2. OutputWriter 분리

현재 `OutputWriter.cpp`에는 아래 책임이 같이 들어 있다.

- summary JSON 조립
- text report 조립
- output 파일 쓰기 orchestration

예상 파일 구조:
- `OutputWriter.cpp`
  - `WriteOutputs`
- `OutputWriter.Summary.cpp`
  - `BuildSummaryJson`
- `OutputWriter.Report.cpp`
  - `BuildReportText`
- `OutputWriterPipeline.h`
  - helper 선언

원칙:
- `BuildSummaryJson` 출력 구조 유지
- `BuildReportText` 줄/라벨/순서 유지
- blackbox JSONL/WCT write는 `WriteOutputs`에 유지

### 3. HangCapture 분리

현재 `HangCapture.cpp`에는 아래 책임이 섞여 있다.

- hang episode state reset / foreground suppression / responsive suppression
- grace period 재검사
- WCT/PSS/freeze recapture evaluation
- dump/manifest/ETW/write path 실행

예상 파일 구조:
- `HangCapture.cpp`
  - `HandleHangTick` orchestration
- `HangCapture.Guards.cpp`
  - episode reset
  - target window ensure
  - suppression 판단
  - `WctJsonHasCycle`
- `HangCapture.Execute.cpp`
  - confirmed hang 이후 WCT/PSS/dump/manifest/ETW path
- `HangCaptureInternal.h`
  - internal helper 선언

원칙:
- `HandleHangTick` 시그니처 유지
- 로그 문구와 side effect 순서 유지
- `ManualCapture.cpp`는 이번 라운드에서 건드리지 않음

## Verification

Analyzer 정리 후:
- `ctest --test-dir build-linux-test --output-on-failure -R "analysis_engine_runtime|candidate_consensus|output_snapshot"`

OutputWriter 정리 후:
- `ctest --test-dir build-linux-test --output-on-failure -R "output_snapshot|analysis_engine_runtime"`

HangCapture 정리 후:
- `ctest --test-dir build-linux-test --output-on-failure -R "helper_crash_autopen_config|helper_preflight_guard|incident_manifest"`

최종:
- `ctest --test-dir build-linux-test --output-on-failure`
- `bash scripts/build-win-from-wsl.sh`
- `bash scripts/build-winui-from-wsl.sh`

## Risks

- cross-file helper 선언/정의 mismatch
- anonymous namespace/static linkage 누락
- `AnalyzeDump` 단계 순서가 미세하게 바뀌는 실수
- `HangCapture` 로그/ETW/manifest 순서 변화

대응:
- move-only refactor로 제한
- 단계별 테스트
- 마지막 전체 테스트와 Windows 빌드까지 실행

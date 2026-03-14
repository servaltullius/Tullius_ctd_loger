# Structural Cleanup Design

## Goal
최근 기능 추가로 비대해진 파일 두 곳을 동작 변경 없이 책임 기준으로 분리한다.

- WinUI: `dump_tool_winui/MainWindowViewModel.cs`
- helper: `helper/src/PendingCrashAnalysis.cpp`

이번 라운드에서는 읽기/유지보수성 개선만 다루고, 동작/출력/스키마/문구는 바꾸지 않는다.

## Scope

포함:
- `MainWindowViewModel`를 partial 파일로 분리
- `PendingCrashAnalysis`를 정책 평가와 실행 경로로 분리
- 기존 테스트/빌드로 회귀 확인

제외:
- `Analyzer.cpp` 구조 정리
- JSON schema 변경
- WinUI binding 이름 변경
- recommendation/evidence/share text 내용 변경
- recapture 정책/점수 변경

## Design

### 1. MainWindowViewModel 분리

현재 `MainWindowViewModel.cs`에는 아래 책임이 같이 들어 있다.

- dump discovery / intake
- actionable candidate 표시와 conflict 요약
- recommendations / recapture context
- clipboard / community share text

이 파일은 public surface는 그대로 유지하고 partial class로만 분리한다.

예상 파일 구조:
- `MainWindowViewModel.cs`
  - 생성자
  - 공용 상태와 기본 field/property
  - `LoadFromSummary` 같은 상위 진입점
- `MainWindowViewModel.DumpDiscovery.cs`
  - recent dump / output root intake 관련 메서드
- `MainWindowViewModel.Recommendations.cs`
  - recommendation grouping
  - recapture context 계산
- `MainWindowViewModel.Candidates.cs`
  - candidate display name / reason / agreement / conflict 요약
- `MainWindowViewModel.ShareText.cs`
  - clipboard/community share text 조립

원칙:
- XAML binding property 이름 유지
- 기존 private helper 이름은 가능하면 유지
- 새 추상화 계층은 만들지 않고, 파일 경계만 정리

### 2. PendingCrashAnalysis 분리

현재 `PendingCrashAnalysis.cpp`에는 아래 책임이 섞여 있다.

- summary/manfiest 경로 계산
- summary 로드
- recapture decision 계산
- target profile -> dump profile 변환
- manifest 업데이트
- recapture dump 실행

이 파일은 helper 함수와 compilation unit 분리로 정리한다.

예상 파일 구조:
- `PendingCrashAnalysis.cpp`
  - public entrypoint
  - 상위 orchestration
- `PendingCrashAnalysis.Decision.cpp`
  - summary 로드
  - `RecaptureDecision` 계산
  - target profile / suffix / dump profile mapping
- `PendingCrashAnalysis.Execute.cpp`
  - manifest update
  - recapture capture/write
  - incident manifest write / log side effect

원칙:
- 함수 시그니처/로그 메시지/동작 유지
- `CrashRecapturePolicy.h` 계약은 바꾸지 않음
- `IncidentManifest` schema 변경 없음

## Verification

WinUI 정리 후:
- `ctest --test-dir build-linux-test --output-on-failure -R winui_xaml`
- `bash scripts/build-winui-from-wsl.sh`

helper 정리 후:
- `ctest --test-dir build-linux-test --output-on-failure -R "pending_crash_analysis|crash_recapture_policy|incident_manifest"`

최종:
- `ctest --test-dir build-linux-test --output-on-failure`

## Risks

- partial 분리 과정에서 private helper 참조 누락 가능성
- helper 분리 과정에서 static/internal linkage 실수 가능성

대응:
- 기능 변경 없이 move-only refactor로 제한
- 단계별 테스트 + 마지막 전체 테스트

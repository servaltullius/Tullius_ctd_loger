# MainWindow Helper Evidence Cleanup Design

## Goal

동작을 바꾸지 않고 다음 4개 지점을 책임 기준으로 분리한다.

1. `dump_tool_winui/MainWindow.xaml.cs`
2. `helper/src/main.cpp`
3. `dump_tool/src/EvidenceBuilderEvidence.cpp`
4. `tests/SourceGuardTestUtils.h` 기반 source-guard 공통화

## Non-Goals

- JSON schema 변경
- XAML 바인딩 이름 변경
- evidence/recommendation/score 동작 변경
- helper loop 정책 변경
- CI/workflow 변경

## File Structure

### WinUI

현재 `MainWindow.xaml.cs`는 다음 책임이 섞여 있다.

- 정적 텍스트 로컬라이즈
- dump discovery UI/폴더 관리
- 분석 실행/결과 렌더링
- triage/review 편집기
- 레이아웃/내비게이션/공통 busy 상태

분리 대상:

- `dump_tool_winui/MainWindow.xaml.cs`
  - 생성자
  - 공통 필드
  - `SetBusy`
  - 내비게이션/레이아웃 entry point
- `dump_tool_winui/MainWindow.Localization.cs`
  - `ApplyLocalizedStaticText`
  - `T`
- `dump_tool_winui/MainWindow.DumpDiscovery.cs`
  - dump discovery refresh
  - location add/remove
  - recent dump analyze
- `dump_tool_winui/MainWindow.Analysis.cs`
  - `AnalyzeAsync`
  - artifact load/render
  - clipboard/share
  - output folder open
- `dump_tool_winui/MainWindow.Triage.cs`
  - triage editor populate/save helpers
- `dump_tool_winui/MainWindow.Layout.cs`
  - wheel chaining
  - adaptive layout
  - nav selection

### Helper bootstrap

현재 `helper/src/main.cpp`는 다음 책임이 섞여 있다.

- singleton/mutex + attach/bootstrap
- manual input handling
- crash/hang/manual loop
- process-exit cleanup
- grass cache special mode

분리 대상:

- `helper/src/main.cpp`
  - `wmain`
  - bootstrap orchestration only
- `helper/src/HelperMainInternal.h`
  - `HelperLoopState`
  - internal helper declarations
- `helper/src/HelperMain.Loop.cpp`
  - `RunHelperLoop`
  - manual input pump
  - crash-event retry
- `helper/src/HelperMain.Process.cpp`
  - process-exit classification
  - deferred viewer launch
  - artifact cleanup
- `helper/src/HelperMain.Startup.cpp`
  - singleton mutex
  - grass cache detect/loop
  - loop-state init

### Evidence builder

현재 `dump_tool/src/EvidenceBuilderEvidence.cpp`는 crash/freeze/context evidence가 한 파일에 섞여 있다.

분리 대상:

- `dump_tool/src/EvidenceBuilderEvidence.cpp`
  - `BuildEvidenceItems` orchestrator only
- `dump_tool/src/EvidenceBuilderEvidence.Context.cpp`
  - capture profile / recapture / symbol runtime / module / history / WCT
- `dump_tool/src/EvidenceBuilderEvidence.Crash.cpp`
  - crash logger / suspect / resource evidence
- `dump_tool/src/EvidenceBuilderEvidence.Freeze.cpp`
  - hitch/freeze / first-chance / freeze-analysis evidence
- `dump_tool/src/EvidenceBuilderEvidencePipeline.h`
  - extracted helper declarations

### Source-guard utility

현재 split source를 읽기 위해 여러 테스트가 수동으로 companion 파일을 이어붙인다.

추가 대상:

- `tests/SourceGuardTestUtils.h`
  - `ReadAllTextWithCompanions(path, companions...)`
  - `ReadKnownSplitSource(path)` 같은 공통 helper

적용 대상은 반복이 큰 테스트 위주로 제한한다.

- `plugin_rules_tests.cpp`
- `event_detail_guard_tests.cpp`
- `symbol_privacy_controls_tests.cpp`
- `incident_manifest_schema_tests.cpp`
- `triage_fields_tests.cpp`
- `output_snapshot_tests.cpp`
- `summary_schema_fields_tests.cpp`
- `diagnostic_logging_guard_tests.cpp`
- `graphics_injection_integration_tests.cpp`
- `crash_history_tests.cpp`
- `blackbox_loader_stall_tests.cpp`
- `system_module_guard_tests.cpp`
- `plugin_stream_tests.cpp`

## Verification

- Linux fast verify:
  - `cmake -S . -B build-linux-test -G Ninja`
  - `cmake --build build-linux-test`
  - `ctest --test-dir build-linux-test --output-on-failure`
- Windows:
  - `bash scripts/build-win-from-wsl.sh`
  - `bash scripts/build-winui-from-wsl.sh`

## Commit Plan

1. `docs: add mainwindow helper evidence cleanup plan`
2. `refactor: split main window code-behind helpers`
3. `refactor: split helper main loop helpers`
4. `refactor: split evidence builder helpers`
5. `refactor: deduplicate source guard companion reads`

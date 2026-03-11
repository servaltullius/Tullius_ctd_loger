# EXE Candidate Consensus UI Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the current EXE-crash top-suspect flow with an object-ref-centered candidate-consensus model, and update WinUI wording so the UI explains cross-signal agreement instead of overstating a single DLL winner.

**Architecture:** Keep the existing `plugin -> helper -> dump_tool -> WinUI` pipeline. Add a new candidate aggregation layer in `dump_tool` that converts raw clues (`CrashLogger object refs`, actionable stack DLLs, resource providers, history, plugin rules) into normalized actionable candidates. Feed the best candidate plus agreement metadata into summary/evidence JSON, then update WinUI to render those fields as "action-first candidate + evidence agreement" instead of "primary suspect + confidence only."

**Tech Stack:** C++17, nlohmann/json, WinUI 3 / C#, CMake, ctest

---

## Chunk 1: Analyzer Candidate Consensus

### Task 1: Add failing analyzer/runtime coverage for EXE-crash candidate consensus

**Files:**
- Modify: `tests/analysis_engine_runtime_tests.cpp`
- Modify: `tests/output_snapshot_tests.cpp`
- Modify: `tests/data/golden_summary_v2.json` (only if summary schema snapshot changes)

- [ ] **Step 1: Write failing runtime tests for the new EXE-crash decision model**
  Add runtime-oriented tests that assert:
  - EXE crash + strong `crash_logger.object_refs` + matching actionable stack/mod clue yields an actionable winner and agreement metadata.
  - EXE crash + `object_ref` only does **not** produce a hard "root cause" style winner.
  - EXE crash + conflicting `object_ref` vs actionable stack clue yields a conflict/ambiguous state instead of a single overconfident winner.

- [ ] **Step 2: Run the targeted analyzer tests and watch them fail**
  Run: `ctest --test-dir build-linux-test -R "skydiag_analysis_engine_runtime_tests|skydiag_output_snapshot_tests" --output-on-failure`
  Expected: FAIL because the summary/output schema does not yet contain the new candidate-consensus fields and wording.

- [ ] **Step 3: Add minimal schema assertions for the new summary fields**
  Extend snapshot/schema coverage to expect a top-level actionable-candidate block plus agreement metadata (for example candidate id/display label, evidence family count, agreement state, conflict flag).

- [ ] **Step 4: Re-run the targeted analyzer tests**
  Run: `ctest --test-dir build-linux-test -R "skydiag_analysis_engine_runtime_tests|skydiag_output_snapshot_tests" --output-on-failure`
  Expected: still FAIL, but now specifically on missing implementation rather than missing test wiring.

### Task 2: Implement candidate aggregation and consensus gating in `dump_tool`

**Files:**
- Modify: `dump_tool/src/Analyzer.h`
- Modify: `dump_tool/src/Analyzer.cpp`
- Modify: `dump_tool/src/EvidenceBuilder.cpp`
- Modify: `dump_tool/src/EvidenceBuilderPrivate.h`
- Modify: `dump_tool/src/EvidenceBuilderSummary.cpp`
- Modify: `dump_tool/src/EvidenceBuilderEvidence.cpp`
- Modify: `dump_tool/src/EvidenceBuilderRecommendations.cpp`
- Modify: `dump_tool/src/OutputWriter.cpp`
- Optional split if needed: `dump_tool/src/AnalyzerInternals*.cpp`

- [ ] **Step 1: Add new analysis result structures for actionable candidates**
  Add a normalized candidate model to `AnalysisResult`, including:
  - display label / entity kind
  - linked plugin/mod/DLL identifiers
  - evidence-family list
  - agreement family count
  - conflict/ambiguous state
  - consensus verdict text for UI/export

- [ ] **Step 2: Implement candidate normalization helpers**
  Add helpers that transform raw signals into candidate entities:
  - `CrashLogger object ref -> plugin candidate`
  - actionable stack DLL with `inferred_mod_name -> mod candidate`
  - resource provider hits near anchor -> mod candidate
  - history/plugin-rule hints -> candidate boosts only

- [ ] **Step 3: Implement EXE-crash consensus scoring**
  Only apply the new candidate-consensus ranking when the dump is crash-like and the fault location is the game EXE or otherwise not directly actionable. Use weighted family scoring plus gating:
  - no single-family hard winner
  - `object_ref` is the strongest seed
  - actionable stack/resource/history/plugin-rule can corroborate
  - conflicting strong clues cap confidence and mark ambiguity

- [ ] **Step 4: Update summary/evidence/recommendation generation**
  Rework summary sentence generation so it uses the new candidate-consensus result:
  - "cross-validated likely candidate"
  - "related likely candidate"
  - "conflicting candidates"
  - "insufficient agreement"
  Preserve old direct-DLL wording for non-EXE actionable crash cases.

- [ ] **Step 5: Emit the new fields in summary JSON/report text**
  Serialize the actionable candidate block and agreement metadata in `OutputWriter.cpp`, preserving backward-safe existing fields where possible.

- [ ] **Step 6: Run the targeted analyzer tests and make them pass**
  Run: `ctest --test-dir build-linux-test -R "skydiag_analysis_engine_runtime_tests|skydiag_output_snapshot_tests" --output-on-failure`
  Expected: PASS.

## Chunk 2: WinUI Meaning Alignment

### Task 3: Add failing WinUI guard tests for the new wording and candidate presentation

**Files:**
- Modify: `tests/winui_xaml_tests.cpp`
- Modify: `dump_tool_winui/AnalysisSummary.cs` (schema reader expectations)

- [ ] **Step 1: Write failing WinUI guard coverage**
  Add assertions that the XAML/C# layer contains:
  - actionable-candidate wording instead of `Primary suspect` in the quick summary area
  - agreement/consensus wording instead of confidence-only wording
  - copy/share text that avoids overclaiming a root cause when only one family supports the candidate

- [ ] **Step 2: Run the WinUI guard test and watch it fail**
  Run: `ctest --test-dir build-linux-test -R skydiag_winui_xaml_tests --output-on-failure`
  Expected: FAIL because the old labels and share wording are still present.

### Task 4: Update WinUI view model and text to match the new analysis model

**Files:**
- Modify: `dump_tool_winui/AnalysisSummary.cs`
- Modify: `dump_tool_winui/MainWindowViewModel.cs`
- Modify: `dump_tool_winui/MainWindow.xaml`
- Modify: `dump_tool_winui/MainWindow.xaml.cs`

- [ ] **Step 1: Load the new candidate-consensus fields**
  Extend `AnalysisSummary` to parse the actionable-candidate block and consensus metadata from summary JSON.

- [ ] **Step 2: Replace primary-suspect wording in the quick summary**
  Update the KPI row and summary labels to render:
  - action-first candidate
  - evidence agreement level / family count
  - key supporting families
  - next verification action

- [ ] **Step 3: Adjust candidate list presentation and copy/share text**
  Ensure the visible wording distinguishes:
  - cross-validated likely candidate
  - object-ref-only candidate
  - conflicting candidates
  Avoid "root cause" phrasing unless consensus gates are satisfied.

- [ ] **Step 4: Re-run the WinUI guard test**
  Run: `ctest --test-dir build-linux-test -R skydiag_winui_xaml_tests --output-on-failure`
  Expected: PASS.

## Chunk 3: Final Validation

### Task 5: Re-run relevant regression coverage and fast verification

**Files:**
- No new files expected

- [ ] **Step 1: Run focused regressions**
  Run:
  - `ctest --test-dir build-linux-test -R "skydiag_analysis_engine_runtime_tests|skydiag_output_snapshot_tests|skydiag_winui_xaml_tests|skydiag_crash_history_tests|skydiag_hook_framework_guard_tests" --output-on-failure`
  Expected: PASS.

- [ ] **Step 2: Run full Linux fast verification**
  Run:
  - `cmake -S . -B build-linux-test -G Ninja`
  - `cmake --build build-linux-test`
  - `ctest --test-dir build-linux-test --output-on-failure`
  Expected: PASS.

- [ ] **Step 3: Sanity-check git diff**
  Confirm that the diff is limited to analyzer/result serialization, WinUI wording/binding, tests, and the plan document.

- [ ] **Step 4: Commit the implementation**
  Suggested commit message: `feat: add candidate-consensus analysis for EXE crashes`


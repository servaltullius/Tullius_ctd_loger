# Helper Runtime Trigger Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 게임을 켜지 않고 synthetic runtime harness로 CTD·프리징 시 helper/dump tool 발동과 정상 종료 false positive 방지 경로를 검증하는 테스트를 추가한다.

**Architecture:** 기존 helper 내부 경계(`HandleCrashEventTick`, `HandleProcessExitTick`, `CleanupCrashArtifactsAfterZeroExit`, `SuppressHangAndLogIfNeeded`)를 직접 호출하는 C++ 테스트를 추가한다. 새 프레임워크는 도입하지 않고, temp dir·synthetic state·기존 internal helper를 사용한다.

**Tech Stack:** C++17 tests, existing helper internals, CMake, temp filesystem I/O

---

## Chunk 1: CTD Runtime Smoke

### Task 1: CTD runtime harness 계약을 failing test로 잠근다

**Files:**
- Create: `tests/helper_runtime_smoke_tests.cpp`
- Modify: `tests/CMakeLists.txt`
- Test: `tests/helper_runtime_smoke_tests.cpp`

- [ ] **Step 1: Write the failing test**

Lock at least these expectations:
- strong crash evidence survives `exit_code=0`
- deferred crash viewer path is preserved for strong-crash exit classification
- normal crash path can queue capture-side artifacts

- [ ] **Step 2: Run focused test to verify failure**

Run:
```bash
cmake --build build-linux-test --target skydiag_helper_runtime_smoke_tests
ctest --test-dir build-linux-test --output-on-failure -R helper_runtime_smoke
```

Expected:
- FAIL because the test target/implementation does not exist yet

### Task 2: minimal CTD synthetic runtime test를 구현한다

**Files:**
- Create: `tests/helper_runtime_smoke_tests.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Build synthetic helper state**

Include:
- temp output dir
- synthetic `HelperLoopState`
- synthetic crash exception code / exit classification inputs

- [ ] **Step 2: Verify strong-crash preservation path**

Use:
- `CleanupCrashArtifactsAfterZeroExit`
- `LaunchDeferredViewersAfterExit` or adjacent state effects when practical

Assert:
- strong crash path does not clear crash artifacts
- pending crash viewer path is not suppressed for strong evidence

- [ ] **Step 3: Run focused test to verify GREEN**

Run:
```bash
cmake --build build-linux-test --target skydiag_helper_runtime_smoke_tests
ctest --test-dir build-linux-test --output-on-failure -R helper_runtime_smoke
```

Expected:
- PASS

- [ ] **Step 4: Commit**

```bash
git add tests/helper_runtime_smoke_tests.cpp tests/CMakeLists.txt
git commit -m "test: add helper runtime smoke coverage"
```

## Chunk 2: False Positive Prevention

### Task 3: 정상 종료 false-positive 경로를 failing test로 잠근다

**Files:**
- Create: `tests/helper_false_positive_runtime_tests.cpp`
- Modify: `tests/CMakeLists.txt`
- Test: `tests/helper_false_positive_runtime_tests.cpp`

- [ ] **Step 1: Write the failing test**

Lock these expectations:
- `exit_code=0` + weak/handled crash => crash artifacts cleanup
- deferred crash viewer launch suppression on normal exit
- pending crash analysis process state is cleared when cleanup path runs

- [ ] **Step 2: Run focused test to verify failure**

Run:
```bash
cmake --build build-linux-test --target skydiag_helper_false_positive_runtime_tests
ctest --test-dir build-linux-test --output-on-failure -R helper_false_positive_runtime
```

Expected:
- FAIL

### Task 4: false-positive runtime test를 구현한다

**Files:**
- Create: `tests/helper_false_positive_runtime_tests.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Create temp artifact set**

Simulate:
- captured dump path
- optional ETW sidecar path
- pending viewer state

- [ ] **Step 2: Exercise zero-exit cleanup**

Use:
- `CleanupCrashArtifactsAfterZeroExit`

Assert:
- artifacts removed
- viewer path cleared
- crashCaptured reset

- [ ] **Step 3: Exercise deferred viewer suppression**

Use:
- `LaunchDeferredViewersAfterExit`

Assert:
- normal exit suppresses deferred viewer path

- [ ] **Step 4: Run focused test to verify GREEN**

Run:
```bash
cmake --build build-linux-test --target skydiag_helper_false_positive_runtime_tests
ctest --test-dir build-linux-test --output-on-failure -R helper_false_positive_runtime
```

Expected:
- PASS

- [ ] **Step 5: Commit**

```bash
git add tests/helper_false_positive_runtime_tests.cpp tests/CMakeLists.txt
git commit -m "test: cover helper false-positive runtime paths"
```

## Chunk 3: Freeze / Hang Trigger Boundaries

### Task 5: hang runtime 경계를 failing test로 잠근다

**Files:**
- Create: `tests/helper_hang_runtime_tests.cpp`
- Modify: `tests/CMakeLists.txt`
- Test: `tests/helper_hang_runtime_tests.cpp`

- [ ] **Step 1: Write the failing test**

Lock these expectations:
- heartbeat not initialized => suppression
- background / foreground grace => suppression
- actionable hang => suppression returns false

- [ ] **Step 2: Run focused test to verify failure**

Run:
```bash
cmake --build build-linux-test --target skydiag_helper_hang_runtime_tests
ctest --test-dir build-linux-test --output-on-failure -R helper_hang_runtime
```

Expected:
- FAIL

### Task 6: hang runtime test를 구현한다

**Files:**
- Create: `tests/helper_hang_runtime_tests.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Build synthetic hang state**

Include:
- `HangCaptureState`
- synthetic heartbeat/loading/foreground/window-responsive inputs

- [ ] **Step 2: Exercise suppression path**

Use:
- `SuppressHangAndLogIfNeeded`

Assert:
- not-foreground suppresses
- foreground grace suppresses
- responsive foreground suppresses when appropriate

- [ ] **Step 3: Exercise actionable hang path**

Assert:
- when conditions indicate real hang, suppression returns false

- [ ] **Step 4: Run focused test to verify GREEN**

Run:
```bash
cmake --build build-linux-test --target skydiag_helper_hang_runtime_tests
ctest --test-dir build-linux-test --output-on-failure -R helper_hang_runtime
```

Expected:
- PASS

- [ ] **Step 5: Commit**

```bash
git add tests/helper_hang_runtime_tests.cpp tests/CMakeLists.txt
git commit -m "test: add helper hang runtime boundaries"
```

## Chunk 4: Documentation

### Task 7: 개발 문서에 runtime 검증 목적과 실행법을 추가한다

**Files:**
- Modify: `docs/DEVELOPMENT.md`
- Modify: `tests/packaging_includes_cli_tests.py`

- [ ] **Step 1: Add a helper runtime validation section**

Document:
- purpose: runtime trigger / false-positive guard
- not for analysis quality
- focused commands for the three new test targets

- [ ] **Step 2: Add a light source-guard**

Lock that `docs/DEVELOPMENT.md` mentions:
- helper runtime validation
- the three test names or the grouped command

- [ ] **Step 3: Run docs/source guard**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R packaging_includes_cli
```

Expected:
- PASS

- [ ] **Step 4: Commit**

```bash
git add docs/DEVELOPMENT.md tests/packaging_includes_cli_tests.py
git commit -m "docs: add helper runtime validation workflow"
```

## Chunk 5: Final Verification

### Task 8: 전체 관련 검증을 마무리한다

**Files:**
- Verify only

- [ ] **Step 1: Run focused helper runtime tests**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R "helper_runtime_smoke|helper_false_positive_runtime|helper_hang_runtime"
```

Expected:
- PASS

- [ ] **Step 2: Run existing nearby helper guards**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R "crash_capture_false_positive_guard|hang_suppression|helper_crash_autopen_config"
```

Expected:
- PASS

- [ ] **Step 3: Run full Linux suite**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure
```

Expected:
- PASS

- [ ] **Step 4: Commit remaining plan docs**

```bash
git add docs/superpowers/specs/2026-03-15-helper-runtime-trigger-design.md docs/superpowers/plans/2026-03-15-helper-runtime-trigger.md
git commit -m "docs: add helper runtime trigger plan"
```

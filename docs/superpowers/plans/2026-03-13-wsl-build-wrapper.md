# WSL Build Wrapper Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** WSL에서 Windows 빌드 스크립트를 안정적으로 실행할 수 있는 전용 래퍼와 문서 진입점을 추가한다.

**Architecture:** 기존 `.cmd` 스크립트는 Windows 네이티브 진입점으로 유지하고, WSL 전용 bash 래퍼가 절대 Windows 경로 변환과 PowerShell 호출만 담당한다. 문서는 BUILD GUIDE와 개발 문서 두 곳만 고쳐 사용자가 어떤 진입점을 써야 하는지 명확히 한다.

**Tech Stack:** Bash, Windows PowerShell, Python source-guard tests

---

## Chunk 1: Guard Rails

### Task 1: Add failing source-guard expectations

**Files:**
- Modify: `tests/packaging_includes_cli_tests.py`

- [ ] **Step 1: Write the failing test**

Add assertions that require:
- `scripts/build-win-from-wsl.sh`
- `scripts/build-winui-from-wsl.sh`
- both wrappers contain `wslpath -w` and `powershell.exe`
- `AGENTS.md` and `docs/DEVELOPMENT.md` mention the wrapper entry points

- [ ] **Step 2: Run test to verify it fails**

Run: `ctest --test-dir build-linux-test --output-on-failure -R packaging_includes_cli`
Expected: FAIL because wrappers/docs do not exist yet

## Chunk 2: Wrapper Scripts

### Task 2: Implement minimal WSL wrapper scripts

**Files:**
- Create: `scripts/build-win-from-wsl.sh`
- Create: `scripts/build-winui-from-wsl.sh`

- [ ] **Step 1: Write minimal implementation**

Each wrapper should:
- require bash with `set -euo pipefail`
- locate sibling `.cmd`
- convert to Windows absolute path with `wslpath -w`
- invoke `/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe` (or `powershell.exe`) with that path

- [ ] **Step 2: Run source-guard test**

Run: `ctest --test-dir build-linux-test --output-on-failure -R packaging_includes_cli`
Expected: PASS for wrapper assertions, docs assertions may still fail until docs are updated

## Chunk 3: Documentation

### Task 3: Update build entrypoint docs

**Files:**
- Modify: `AGENTS.md`
- Modify: `docs/DEVELOPMENT.md`

- [ ] **Step 1: Add WSL wrapper section**

Document:
- Windows native entry points stay `scripts\\build-win.cmd`, `scripts\\build-winui.cmd`
- WSL entry points are `bash scripts/build-win-from-wsl.sh` and `bash scripts/build-winui-from-wsl.sh`
- relative `cmd.exe /c scripts\\...` from WSL is not supported

- [ ] **Step 2: Re-run source-guard test**

Run: `ctest --test-dir build-linux-test --output-on-failure -R packaging_includes_cli`
Expected: PASS

## Chunk 4: Verification

### Task 4: Full verification and wrapper smoke run

**Files:**
- Verify only

- [ ] **Step 1: Run Linux test suite**

Run: `ctest --test-dir build-linux-test --output-on-failure`
Expected: PASS

- [ ] **Step 2: Run WSL wrapper builds**

Run:
```bash
bash scripts/build-win-from-wsl.sh
bash scripts/build-winui-from-wsl.sh
```
Expected: both succeed and produce/update `build-win` and `build-winui`

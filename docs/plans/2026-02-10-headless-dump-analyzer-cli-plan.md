# Headless Dump Analyzer CLI Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a Windows console CLI (`SkyrimDiagDumpToolCli.exe`) that runs the existing dump analysis core without WinUI, then update `SkyrimDiagHelper.exe` and packaging to prefer this CLI for headless analysis (fallback to WinUI exe for backward compatibility).

**Architecture:** Keep dump analysis in `SkyrimDiagDumpToolCore` (existing). Add a thin CLI front-end (arg parsing + `AnalyzeDump` + `WriteOutputs`). Update helper to resolve and spawn the headless CLI when available.

**Tech Stack:** C++20, CMake, Win32 (process priority), existing `dump_tool` core (`AnalyzeDump`, `WriteOutputs`).

**Constraints / Non-goals:**
- No in-game inconvenience: new work runs only post-incident (crash/hang/manual capture) and should be low priority.
- Keep existing WinUI viewer unchanged.
- Backward compatible: if CLI is missing, helper continues to use the WinUI exe for headless mode exactly as today.

**Evidence / References (Research-first):**
- MSVC wide main entrypoint (`wmain`) docs: https://learn.microsoft.com/en-us/cpp/cpp/main-function-command-line-args?view=msvc-170
- Win32 process priority class overview (`SetPriorityClass`, `BELOW_NORMAL_PRIORITY_CLASS`): https://learn.microsoft.com/en-us/windows/win32/procthread/scheduling-priorities
- `SetPriorityClass` API reference: https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-setpriorityclass
- CMake `add_executable` docs: https://cmake.org/cmake/help/latest/command/add_executable.html

## Task 1: RED - Add CLI Arg Parsing Tests (compile-time red is acceptable for C++)

**Files:**
- Create: `tests/dump_tool_cli_args_tests.cpp`
- Modify: `tests/CMakeLists.txt`

**Step 1: Write failing tests**
- Add tests that expect:
  - positional dump path required
  - `--out-dir <dir>` captured
  - `--allow-online-symbols` sets `allow_online_symbols=true`
  - `--no-online-symbols` sets `allow_online_symbols=false`
  - `--headless` is accepted and ignored (for compatibility with existing helper invocation)

**Step 2: Run tests to verify RED**
Run:
```bash
cmake -S . -B build-linux-test -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-linux-test -j
ctest --test-dir build-linux-test --output-on-failure
```
Expected: build or test failure because CLI arg parser does not exist yet.

## Task 2: GREEN - Implement Minimal CLI Arg Parser (cross-platform, unit-testable)

**Files:**
- Create: `dump_tool/src/DumpToolCliArgs.h`

**Step 1: Minimal implementation**
- Implement a small, header-only parser:
  - input: `std::vector<std::wstring_view>` (argv)
  - output: struct with `dump_path`, `out_dir`, `allow_online_symbols` (tri-state), `debug`, `lang`
  - parse supported flags:
    - `--out-dir <dir>`
    - `--allow-online-symbols`
    - `--no-online-symbols`
    - `--debug`
    - `--lang <token>`
    - `--headless` (ignored)
    - `--help` (returns false + usage)
- Keep parser behavior deterministic and strict (unknown flags => error).

**Step 2: Run tests to verify GREEN**
Re-run the Task 1 command and confirm all tests pass again.

## Task 3: RED - Add Helper Headless Resolver Tests

**Files:**
- Create: `tests/dump_tool_headless_resolver_tests.cpp`
- Modify: `tests/CMakeLists.txt`

**Step 1: Write failing tests**
- Add a header-only helper resolver API (to be created in Task 4) that chooses:
  1) `SkyrimDiagDumpToolCli.exe` next to helper, if present
  2) else `SkyrimDiagDumpToolWinUI.exe` (existing location logic), if present
  3) else configured override path, if present
- Tests should create a temp directory, create dummy files, and validate resolution preference order.

**Step 2: Run tests to verify RED**
Run the same Linux build+ctest command.
Expected: build failure until resolver header exists.

## Task 4: GREEN - Implement Headless Resolver + Wire Helper

**Files:**
- Create: `helper/include/DumpToolResolve.h`
- Modify: `helper/src/main.cpp`

**Step 1: Implement resolver (header-only)**
- Implement:
  - `ResolveDumpToolHeadlessExe(helper_dir, winui_exe, configured_override)`
  - Use `std::filesystem::exists`.
  - Make it Windows-friendly but cross-platform testable.

**Step 2: Wire helper to prefer CLI for headless**
- Add `ResolveDumpToolHeadlessExe(cfg)` wrapper in `helper/src/main.cpp`.
- Update headless spawn paths:
  - `StartDumpToolHeadlessIfConfigured`
  - `StartDumpToolHeadlessAsync`
- Keep fallback to existing WinUI headless path if CLI is missing.

**Step 3: Run tests to verify GREEN**
Re-run Linux tests and confirm all green.

## Task 5: Add `SkyrimDiagDumpToolCli.exe` Target (Windows-only)

**Files:**
- Create: `dump_tool/src/DumpToolCliMain.cpp`
- Modify: `dump_tool/CMakeLists.txt`

**Step 1: Implement CLI main**
- `wmain(int argc, wchar_t** argv)`:
  - Parse args via `DumpToolCliArgs.h`
  - Create `AnalyzeOptions`:
    - `debug` from args
    - `language` from `--lang` (ascii token)
    - `allow_online_symbols`: args override, else env var default (same as NativeApi)
    - `redact_paths`: `!debug` (default safe)
  - Call `AnalyzeDump(dump_path, out_dir, opt, result, &err)`
  - Call `WriteOutputs(result, &err)`
  - Print errors to `stderr`, return non-zero on failure.
  - Call `SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS)` best-effort to reduce post-crash contention.

**Step 2: Add CMake target**
- `if(WIN32)`:
  - `add_executable(SkyrimDiagDumpToolCli ...)`
  - `target_link_libraries(... PRIVATE SkyrimDiagDumpToolCore)`

**Step 3: Windows build verification**
- After syncing to Windows mirror, run:
  - `scripts\\build-win.cmd`
  - `scripts\\build-winui.cmd`
- Confirm `SkyrimDiagDumpToolCli.exe` is produced.

## Task 6: Update Packaging To Ship CLI Next To Helper

**Files:**
- Modify: `scripts/package.py`

**Step 1: Include artifacts**
- Copy `SkyrimDiagDumpToolCli.exe` into `SKSE/Plugins/` in the output zip.
- Copy `.pdb` when available (best-effort).

**Step 2: Package verification**
Run:
```bash
python scripts/package.py --build-dir build-win --winui-dir build-winui --out dist/Tullius_ctd_loger.zip
```
Confirm zip contains:
- `SKSE/Plugins/SkyrimDiagDumpToolCli.exe`

## Task 7: Version Bump + Docs

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `plugin/src/PluginInfo.cpp`
- Modify: `CHANGELOG.md`
- Modify: `README.md`

**Step 1: Bump version to `0.2.10`**
- Update CMake project version.
- Update plugin version string.

**Step 2: Changelog**
- Add entry: "Add headless CLI analyzer; helper prefers it for headless analysis; WinUI viewer unchanged."

## Task 8: GitHub Tag + Release

**Step 1: Tag**
```bash
git tag v0.2.10
git push origin feat-headless-dump-cli
git push origin v0.2.10
```

**Step 2: Release**
- Create GitHub release `v0.2.10` (not prerelease) and upload `dist/Tullius_ctd_loger.zip`.


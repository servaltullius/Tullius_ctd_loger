# Default Output Subfolder Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** blank `OutputDir` 사용자를 위해 기본 출력 위치를 `Tullius Ctd Logs` 하위 폴더로 바꾸고, WinUI 자동 발견과 문서를 같은 계약으로 맞춘다.

**Architecture:** helper는 blank `OutputDir`를 explicit default subfolder로 해석하고, WinUI discovery는 새 기본 폴더를 우선 root로, 기존 blank-default root를 legacy root로 함께 본다. 명시적 `OutputDir` 경로 계약은 유지해 기존 사용자 설정을 깨지 않는다.

**Tech Stack:** C++20 helper runtime, WinUI 3/C#, source-guard tests, CMake/CTest, Windows MO2 smoke checklist

---

## Chunk 1: Lock the blank-default contract with failing tests

### Task 1: Add failing source-guard coverage for the new default output rule

**Files:**
- Modify: `tests/helper_crash_autopen_config_tests.cpp`
- Modify: `tests/winui_xaml_tests.cpp`
- Test: `tests/helper_crash_autopen_config_tests.cpp`
- Test: `tests/winui_xaml_tests.cpp`

- [ ] **Step 1: Extend helper config guard expectations**

Add expectations that:
- `helper/src/Config.cpp` contains the default subfolder name `Tullius Ctd Logs`
- blank `OutputDir` no longer resolves to bare `ExeDir()` alone
- explicit `OutputDir` read path still exists unchanged

- [ ] **Step 2: Extend WinUI discovery guard expectations**

Add expectations that:
- `dump_tool_winui/DumpDiscoveryService.cs` references `Tullius Ctd Logs`
- blank default logic includes a legacy fallback root
- MO2 blank default logic targets `overwrite\\SKSE\\Plugins\\Tullius Ctd Logs`

- [ ] **Step 3: Run narrow tests and confirm they fail before implementation**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R "skydiag_helper_crash_autopen_config_tests|skydiag_winui_xaml_tests"
```

Expected:
- `skydiag_helper_crash_autopen_config_tests` fails because `Config.cpp` does not mention the new default subfolder yet
- `skydiag_winui_xaml_tests` fails because `DumpDiscoveryService.cs` still treats blank output as root `overwrite\\SKSE\\Plugins`

- [ ] **Step 4: Commit the failing tests**

```bash
git add tests/helper_crash_autopen_config_tests.cpp tests/winui_xaml_tests.cpp
git commit -m "test: lock default output subfolder contract"
```

## Chunk 2: Implement the helper runtime default output subfolder

### Task 2: Make blank `OutputDir` resolve to the default subfolder without changing explicit paths

**Files:**
- Modify: `helper/src/Config.cpp`
- Verify: `helper/src/HelperCommon.cpp`
- Test: `tests/helper_crash_autopen_config_tests.cpp`

- [ ] **Step 1: Introduce an explicit blank-output resolver in `Config.cpp`**

Implement a helper along these lines:

```cpp
constexpr wchar_t kDefaultOutputSubdir[] = L"Tullius Ctd Logs";

std::wstring ResolveEffectiveOutputDir(std::wstring raw)
{
  if (!raw.empty()) {
    return raw;
  }
  return (std::filesystem::path(ExeDir()) / kDefaultOutputSubdir).wstring();
}
```

Use it so that:
- blank `OutputDir` becomes `ExeDir()/Tullius Ctd Logs`
- non-blank `OutputDir` stays untouched

- [ ] **Step 2: Keep output-base creation behavior simple**

Confirm `helper/src/HelperCommon.cpp` still only does:

```cpp
std::filesystem::path out(cfg.outputDir);
std::filesystem::create_directories(out, ec);
```

Do not add a second fallback there. Path resolution should stay centralized in `Config.cpp`.

- [ ] **Step 3: Run helper config guard test**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R skydiag_helper_crash_autopen_config_tests
```

Expected:
- PASS

- [ ] **Step 4: Commit the helper contract change**

```bash
git add helper/src/Config.cpp tests/helper_crash_autopen_config_tests.cpp
git commit -m "feat: default helper output to subfolder"
```

## Chunk 3: Update WinUI discovery for new and legacy blank defaults

### Task 3: Teach discovery to prefer the new default root and keep legacy root compatibility

**Files:**
- Modify: `dump_tool_winui/DumpDiscoveryService.cs`
- Modify: `tests/winui_xaml_tests.cpp`
- Test: `tests/winui_xaml_tests.cpp`

- [ ] **Step 1: Refactor blank-output discovery into explicit root builders**

Split the current blank/default logic into helpers such as:

```csharp
TryResolveConfiguredOutputRoot(...)
BuildDefaultOutputRoots(...)
TryInferMo2BaseDirectory(...)
```

`BuildDefaultOutputRoots(...)` should return roots in this order:
1. new blank default root
2. legacy blank default root

- [ ] **Step 2: Implement MO2 and non-MO2 new-default roots**

For blank `OutputDir`:
- MO2: `Path.Combine(mo2BaseDirectory, "overwrite", "SKSE", "Plugins", "Tullius Ctd Logs")`
- non-MO2: `Path.Combine(layout.HelperDirectoryPath, "Tullius Ctd Logs")`

Keep legacy roots:
- MO2: `Path.Combine(mo2BaseDirectory, "overwrite", "SKSE", "Plugins")`
- non-MO2: `layout.HelperDirectoryPath`

- [ ] **Step 3: Ensure configured `OutputDir` still wins**

Do not change this rule:
- absolute configured path stays absolute
- relative configured path is still resolved from `layout.HelperDirectoryPath`

- [ ] **Step 4: Run WinUI source-guard test**

Run:
```bash
ctest --test-dir build-linux-test --output-on-failure -R skydiag_winui_xaml_tests
```

Expected:
- PASS

- [ ] **Step 5: Commit the discovery update**

```bash
git add dump_tool_winui/DumpDiscoveryService.cs tests/winui_xaml_tests.cpp
git commit -m "feat: discover default output subfolder"
```

## Chunk 4: Update shipped config and user-facing docs

### Task 4: Document the new blank-default behavior without changing explicit path syntax

**Files:**
- Modify: `dist/SkyrimDiagHelper.ini`
- Modify: `README.md`
- Modify: `docs/README_KO.md`
- Modify: `docs/nexus-description.bbcode`
- Modify: `docs/BETA_TESTING.md`

- [ ] **Step 1: Update shipped ini comments**

Explain in `dist/SkyrimDiagHelper.ini`:
- blank = use the default `Tullius Ctd Logs` subfolder under the default output location
- explicit values still accept plain paths without quotes
- relative explicit paths still resolve from the helper folder

Do **not** set:

```ini
OutputDir=Tullius Ctd Logs
```

Keep the key blank and explain the new blank behavior in comments.

- [ ] **Step 2: Update English docs**

In `README.md` and English nexus copy:
- replace “blank = next to exe” language
- state that blank uses the default `Tullius Ctd Logs` subfolder
- keep the “no quotes needed” and “full path not required” guidance

- [ ] **Step 3: Update Korean docs**

In `docs/README_KO.md`, Korean nexus copy, and `docs/BETA_TESTING.md`:
- explain that blank is valid and now uses the default `Tullius Ctd Logs` folder
- show `OutputDir=Tullius Ctd Logs` only as an optional explicit override example, not as the shipped default

- [ ] **Step 4: Commit the config/doc updates**

```bash
git add dist/SkyrimDiagHelper.ini README.md docs/README_KO.md docs/nexus-description.bbcode docs/BETA_TESTING.md
git commit -m "docs: explain default output subfolder behavior"
```

## Chunk 5: Validate Linux guards, Windows builds, and MO2 smoke behavior

### Task 5: Verify both the contract and the real packaged runtime behavior

**Files:**
- Verify only

- [ ] **Step 1: Run Linux fast verification**

Run:
```bash
cmake -S . -B build-linux-test -G Ninja
cmake --build build-linux-test
ctest --test-dir build-linux-test --output-on-failure
```

Expected:
- PASS

- [ ] **Step 2: Run Windows builds from WSL**

Run:
```bash
bash scripts/build-win-from-wsl.sh
bash scripts/build-winui-from-wsl.sh
```

Expected:
- PASS

- [ ] **Step 3: Run MO2 smoke focusing on output location**

Use `docs/MO2_WINUI_SMOKE_TEST_CHECKLIST.md` and explicitly verify:
- blank `OutputDir` produces `Tullius Ctd Logs`
- `SkyrimDiagHelper.log` lands under the new default subfolder
- WinUI start screen auto-discovers dumps from the new subfolder
- at least one old dump copied to legacy root is still discoverable

- [ ] **Step 4: Update release-facing notes if the runtime result differs from wording assumptions**

If MO2 smoke shows wording mismatch, adjust:
- `README.md`
- `docs/README_KO.md`
- `docs/nexus-description.bbcode`

Then rerun the smallest affected verification.

- [ ] **Step 5: Commit the verification-driven adjustments**

```bash
git add README.md docs/README_KO.md docs/nexus-description.bbcode docs/MO2_WINUI_SMOKE_TEST_CHECKLIST.md
git commit -m "test: verify default output subfolder rollout"
```

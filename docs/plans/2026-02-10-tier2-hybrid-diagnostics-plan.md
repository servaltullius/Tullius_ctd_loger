# Tier 2 Hybrid Diagnostics Implementation Plan

**Goal:** Keep minidumps as the backbone, but raise real-world diagnosis quality by adding incident-level context capture (ETW + manifest + bundle) in a privacy-first, low-overhead way.

**Architecture:** Use the existing `plugin -> helper -> dump_tool` pipeline and add a new "incident bundle" layer in `helper`. Every capture (crash/hang/manual) produces a small manifest that links all artifacts (`.dmp`, WCT JSON, ETW, Summary/Report). ETW remains opt-in and best-effort, with separate policies for hang and crash. Output stays local-first; no new network upload path is introduced.

**Tech Stack:** C++20, Win32 process APIs, WPR/ETW (`wpr.exe`), nlohmann/json, existing CTest text-surface tests.

## Assumptions

- Local-first operation remains default (no remote telemetry endpoint).
- Privacy defaults remain strict (`AllowOnlineSymbols=0`, path redaction on summary/report).
- New ETW behavior must never block dump/WCT generation.

## Why This Direction (Industry Alignment)

- Crashpad/Chromium and Unreal both use **minidump + metadata + bucketing**, not dump-only.
- Microsoft guidance shows ETW is production-safe when enabled dynamically and kept scoped.
- Unity-style operations combine crash + exception + telemetry/ANR signals in one workflow.

### Task 1: Add RED tests for new Tier 2 surfaces

**Files:**
- Create: `tests/tier2_hybrid_config_tests.cpp`
- Create: `tests/incident_manifest_schema_tests.cpp`
- Modify: `tests/CMakeLists.txt`

**Step 1: Write failing config-surface tests**

Add assertions that these keys exist in `dist/SkyrimDiagHelper.ini` and are loaded in `helper/src/Config.cpp`:
- `EnableEtwCaptureOnCrash`
- `EtwCrashProfile`
- `EtwCrashCaptureSeconds`
- `EnableIncidentManifest`
- `IncidentManifestIncludeConfigSnapshot`

**Step 2: Write failing manifest/output tests**

Add assertions in `tests/incident_manifest_schema_tests.cpp` for new helper output writer strings:
- `SkyrimDiag_Incident_`
- `incident_id`
- `capture_kind`
- `artifacts`
- `etw`
- `privacy`

**Step 3: Register targets in CMake**

Add test executables and `add_test(...)` entries in `tests/CMakeLists.txt`.

**Step 4: Verify RED**

Run:
```bash
cmake --build build-linux-test --target skydiag_tier2_hybrid_config_tests skydiag_incident_manifest_schema_tests
ctest --test-dir build-linux-test -R "skydiag_tier2_hybrid_config_tests|skydiag_incident_manifest_schema_tests" --output-on-failure
```
Expected: tests fail on missing keys/strings.

### Task 2: Add config model for crash ETW + manifest controls

**Files:**
- Modify: `helper/include/SkyrimDiagHelper/Config.h`
- Modify: `helper/src/Config.cpp`
- Modify: `dist/SkyrimDiagHelper.ini`

**Step 1: Extend `HelperConfig`**

Add fields:
- `bool enableEtwCaptureOnCrash = false;`
- `std::wstring etwCrashProfile = L"GeneralProfile";`
- `std::uint32_t etwCrashCaptureSeconds = 8;`
- `bool enableIncidentManifest = true;`
- `bool incidentManifestIncludeConfigSnapshot = true;`

**Step 2: Parse INI keys in loader**

Read each key with safe defaults and bounds for `etwCrashCaptureSeconds` (e.g., clamp 1..30).

**Step 3: Add documented defaults to `dist/SkyrimDiagHelper.ini`**

Add comments + defaults:
- `EnableEtwCaptureOnCrash=0`
- `EtwCrashProfile=GeneralProfile`
- `EtwCrashCaptureSeconds=8`
- `EnableIncidentManifest=1`
- `IncidentManifestIncludeConfigSnapshot=1`

**Step 4: Verify GREEN for Task 1 config tests**

Run:
```bash
ctest --test-dir build-linux-test -R skydiag_tier2_hybrid_config_tests --output-on-failure
```
Expected: pass.

### Task 3: Generalize ETW capture flow and add crash-window ETW

**Files:**
- Modify: `helper/src/main.cpp`

**Step 1: Extract ETW capture mode helpers**

Refactor current hang-only ETW helpers into generic helpers:
- `StartEtwCapture(profile)`
- `StopEtwCapture(etlPath)`

Preserve existing hang behavior and fallback profile logic.

**Step 2: Add crash ETW window policy (best-effort)**

When crash event is detected and `EnableEtwCaptureOnCrash=1`:
- Start ETW with `EtwCrashProfile`
- Wait `EtwCrashCaptureSeconds` (or until process exits)
- Stop ETW and write `<stem>.etl`
- Never block dump writing on ETW failure

**Step 3: Add explicit logging for ETW lifecycle**

Emit helper log lines for start/stop/failure, including profile and elapsed seconds.

**Step 4: Verify no regression for hang ETW path**

Run existing tests:
```bash
ctest --test-dir build-linux-test -R "skydiag_helper_crash_autopen_config_tests|skydiag_retention_tests" --output-on-failure
```
Expected: pass.

### Task 4: Create incident manifest sidecar and wire it into all capture paths

**Files:**
- Create: `helper/include/SkyrimDiagHelper/IncidentManifest.h`
- Modify: `helper/src/main.cpp`
- Modify: `helper/include/SkyrimDiagHelper/Retention.h`

**Step 1: Add manifest model + write helper**

Create a minimal JSON schema per incident:
```json
{
  "schema": {"name":"SkyrimDiagIncident","version":1},
  "incident_id":"...",
  "capture_kind":"crash|hang|manual",
  "trigger": {"seconds_since_heartbeat":0,"threshold_sec":0,"is_loading":false},
  "artifacts": {"dump":"...","wct_json":"...","etl":"...","summary":"...","report":"..."},
  "etw": {"enabled":false,"profile":"","status":"skipped|ok|failed"},
  "privacy": {"allow_online_symbols":false,"path_redaction_expected":true}
}
```

**Step 2: Write manifest on crash/hang/manual capture**

In `helper/src/main.cpp`, after artifact generation, write `SkyrimDiag_Incident_<timestamp>.json` when `EnableIncidentManifest=1`.

**Step 3: Add retention cleanup for manifest sidecars**

When old dump timestamps are pruned, also delete matching `SkyrimDiag_Incident_<timestamp>.json`.

**Step 4: Verify GREEN for manifest test**

Run:
```bash
ctest --test-dir build-linux-test -R skydiag_incident_manifest_schema_tests --output-on-failure
```
Expected: pass.

### Task 5: Expose incident context in DumpTool outputs

**Files:**
- Modify: `dump_tool/src/Analyzer.h`
- Modify: `dump_tool/src/Analyzer.cpp`
- Modify: `dump_tool/src/OutputWriter.cpp`
- Modify: `tests/summary_schema_fields_tests.cpp`

**Step 1: Add optional incident context to analysis result**

Add optional fields for incident id, capture kind, and ETW status/profile when sidecar is present.

**Step 2: Read sidecar by timestamp/stem match**

In analyzer, attempt to load `SkyrimDiag_Incident_<timestamp>.json` adjacent to dump and map known fields.

**Step 3: Emit to summary/report**

Add `summary["incident"]` and report lines for:
- incident id
- capture kind
- etw status/profile

**Step 4: Extend schema tests**

Assert `incident` object and core fields are emitted.

**Step 5: Verify GREEN**

Run:
```bash
ctest --test-dir build-linux-test -R "skydiag_summary_schema_fields_tests|skydiag_symbol_privacy_controls_tests" --output-on-failure
```
Expected: pass.

### Task 6: Add one-command incident bundle export

**Files:**
- Create: `scripts/package_incident_bundle.py`
- Create: `tests/incident_bundle_script_tests.py`
- Modify: `tests/CMakeLists.txt`
- Modify: `README.md`

**Step 1: Add script**

Create a script that accepts `--dump <path>` and emits a zip containing:
- dump
- matching summary/report/blackbox/wct/etl/incident manifest (if exists)
- optional redacted manifest copy for sharing

**Step 2: Add script test**

Add a synthetic temp-dir test that creates fake files and validates zip content list.

**Step 3: Register test**

Add Python test target to `tests/CMakeLists.txt` via `Python3_EXECUTABLE`.

**Step 4: Verify GREEN**

Run:
```bash
ctest --test-dir build-linux-test -R "incident_bundle|bucket_quality_script_tests" --output-on-failure
```
Expected: pass.

### Task 7: Documentation and operator playbook updates

**Files:**
- Modify: `README.md`
- Modify: `docs/README_KO.md`
- Create: `docs/OPERATIONS_HYBRID_DIAGNOSTICS.md`

**Step 1: Document the Tier 2 model**

Add a concise section: "Dump + WCT + ETW + Incident Manifest" with default/off-by-default controls.

**Step 2: Add troubleshooting matrix**

Document when to use:
- mini dump only
- mini + ETW
- full recapture

**Step 3: Add privacy notes**

Clarify what is redacted and what should not be publicly shared.

### Task 8: Final verification and release checklist

**Files:**
- (No new files expected)

**Step 1: Full tests (Linux)**

Run:
```bash
cmake --build build-linux-test
ctest --test-dir build-linux-test --output-on-failure
```

**Step 2: Windows validation**

Run (Windows mirror):
```bat
scripts\build-win.cmd
scripts\build-winui.cmd
ctest --test-dir build-win --output-on-failure
py -3 scripts\package.py --build-dir build-win --out dist\Tullius_ctd_loger.zip
```

**Step 3: Evidence capture for PR/release note**

Record:
- test pass counts
- zip path + SHA-256
- key config defaults (`EnableEtwCaptureOnCrash`, `EnableIncidentManifest`)

**Step 4: Commit sequence**

Use small commits:
1. tests RED scaffolding
2. config/model changes
3. ETW crash-window feature
4. incident manifest + retention
5. analyzer/output schema
6. incident bundle script + docs

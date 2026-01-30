# Manual Capture + Heartbeat Fix Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use `superpowers:executing-plans` to implement this plan task-by-task.

**Goal:** Manual hotkey capture always produces `SkyrimDiag_Manual_*.dmp`, and auto-hang capture no longer misfires in 정상 상태.

**Architecture:** Make the helper long-running (no exit after first capture) with hang capture rate-limited per hang episode. Make the plugin heartbeat update on the main thread reliably via an input event sink (registered on `kInputLoaded`).

**Tech Stack:** Win32 + SKSE (CommonLibSSE-NG), shared memory, MiniDump writing, WCT capture, nlohmann/json.

---

### Task 1: Helper capture loop reliability

**Files:**
- Modify: `helper/src/main.cpp`

**Steps:**
1. Make helper continue running until target process exits (don’t exit after a hang/manual capture).
2. Keep manual capture priority (WM_HOTKEY + `GetAsyncKeyState` polling).
3. Rate-limit hang dumps: capture only once per hang episode; reset when heartbeat recovers.
4. If heartbeat never advances after attach (e.g., version mismatch/broken heartbeat), log a warning and disable auto-hang capture to prevent false positives.

**How to test (manual):**
- Launch via SKSE.
- Press `Ctrl+Shift+F12` → expect `SkyrimDiag_Manual_*.dmp` + `SkyrimDiag_WCT_Manual_*.json`.
- Check `SkyrimDiagHelper.log` contains `Manual capture triggered via ...`.

---

### Task 2: Plugin heartbeat on main thread

**Files:**
- Modify: `plugin/src/Heartbeat.cpp`
- Modify: `plugin/include/SkyrimDiag/Heartbeat.h`
- Modify: `plugin/src/PluginMain.cpp`

**Steps:**
1. Register an `RE::BSTEventSink<RE::InputEvent*>` on `SKSE::MessagingInterface::kInputLoaded`.
2. Update `SharedHeader.last_heartbeat_qpc` inside the sink callback.
3. Keep hitch logging semantics (based on delta between updates).

**How to test (manual):**
- After game is running, wait > 10 seconds without any freeze.
- Ensure helper does **not** write `SkyrimDiag_Hang_*.dmp` due to false hang decisions.

---

### Task 3: Build + package

**Files:**
- Run: `scripts/build-win.cmd`
- Run: `python3 scripts/package.py --build-dir build-win --out dist/SkyrimDiag.zip`

**How to test (manual):**
- Install `dist/SkyrimDiag.zip` as an MO2 mod.
- Verify new binaries are active by checking `SkyrimDiagHelper.log` and dump filenames.


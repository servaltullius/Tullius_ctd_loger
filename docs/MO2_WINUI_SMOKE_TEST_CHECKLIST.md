# MO2 WinUI Smoke Test Checklist

## Scope

This checklist verifies that the WinUI-first DumpTool flow works for real MO2 users, while legacy fallback remains safe.

Target build:
- `dist/Tullius_ctd_loger.zip`

Target audience:
- Beginner Skyrim users who can install mods in MO2

## 1) Install + Basic Layout

1. Install `Tullius_ctd_loger.zip` as a mod in MO2.
2. Confirm these files exist under the mod:
   - `SKSE/Plugins/SkyrimDiag.dll`
   - `SKSE/Plugins/SkyrimDiagHelper.exe`
   - `SKSE/Plugins/SkyrimDiagDumpTool.exe`
   - `SKSE/Plugins/SkyrimDiagWinUI/SkyrimDiagDumpToolWinUI.exe`
3. Confirm `SkyrimDiagHelper.ini` contains:
   - `DumpToolExe=SkyrimDiagWinUI\SkyrimDiagDumpToolWinUI.exe`

Expected:
- Mod installs cleanly.
- No missing-file warning from MO2.

## 2) Runtime Boot Check

1. Launch Skyrim via SKSE from MO2.
2. Wait until main menu.
3. Exit game normally.
4. Open MO2 `overwrite\SKSE\Plugins\SkyrimDiagHelper.log`.

Expected:
- Helper starts and exits without repeated process launch failures.

## 3) CTD Auto-Open Path

1. In `SkyrimDiag.ini`, set `EnableTestHotkeys=1`.
2. Launch Skyrim via SKSE.
3. Press `Ctrl+Shift+F10` (intentional crash).

Expected:
- A crash dump is created.
- WinUI viewer auto-opens for that dump.
- WinUI screen shows summary card + top suspects + recommendations.

## 4) Hang Auto-Open Path

1. Launch Skyrim via SKSE.
2. Press `Ctrl+Shift+F11` (intentional hang).
3. Wait for helper capture, then close Skyrim process.

Expected:
- Hang dump and WCT are produced.
- Viewer opens after process exit (not during active hang).

## 5) Manual Capture Behavior

1. Launch Skyrim via SKSE.
2. Press `Ctrl+Shift+F12` (manual capture).

Expected (default config):
- Dump is produced.
- Viewer does NOT auto-open (`AutoOpenViewerOnManualCapture=0`).

## 6) WinUI UX Sanity

1. Open `SkyrimDiagWinUI\SkyrimDiagDumpToolWinUI.exe`.
2. Pick a dump manually.
3. Click `Analyze now`.
4. Click `Open advanced viewer`.
5. Click `Open report folder`.

Expected:
- Analysis completes with status message.
- Advanced button opens legacy tabbed viewer.
- Output folder opens in Explorer.

## 7) Legacy Fallback Safety

1. Temporarily rename `SKSE/Plugins/SkyrimDiagWinUI` to `SkyrimDiagWinUI_OFF`.
2. Trigger one crash/hang capture again.

Expected:
- Helper falls back to `SkyrimDiagDumpTool.exe` automatically.
- Analysis/report generation still works.

3. Restore folder name to `SkyrimDiagWinUI`.

## 8) Output Artifact Check

For at least one captured dump, verify:
- `<stem>_SkyrimDiagSummary.json`
- `<stem>_SkyrimDiagReport.txt`
- `<stem>_SkyrimDiagBlackbox.jsonl` (when available)
- `SkyrimDiag_WCT_*.json` (for hang/manual capture path)

Expected:
- JSON and text report files are generated and readable.

## 9) Pass/Fail Rule

Pass:
- All 8 sections pass without manual file surgery beyond test setup.

Fail:
- Any of:
  - viewer launch failure on CTD/hang
  - missing summary/report output
  - fallback to legacy viewer fails when WinUI is absent

## 10) Bug Report Template (Quick)

If failed, report:
- Which checklist section failed
- Exact timestamp (local time)
- `SkyrimDiagHelper.log` excerpt near failure
- Related dump/report filenames
- MO2 profile name + Skyrim SE/AE version

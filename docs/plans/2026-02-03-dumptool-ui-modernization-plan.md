# DumpTool UI Modernization (Win32, Low-Risk) Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Modernize the Win32 DumpTool viewer UI (spacing/typography/controls) without changing the overall layout (tabs + evidence/checklist split + bottom buttons).

**Architecture:** Keep the existing control tree and analysis pipeline intact. Restrict changes to `dump_tool/src/GuiApp.cpp` (painting, control styles, layout). Prefer incremental “Quick Wins”: better padding/margins, modern buttons (owner-draw), ListView readability (tooltips/selection/striping), and safer DWM cosmetic tweaks (rounded corners) with graceful fallback.

**Tech Stack:** C++20, Win32 (comctl32), DWM attributes (Windows 11 optional), existing Windows MSVC build + `scripts/package.py`.

---

### Task 1: Remove conflicting theming + improve ListView defaults

**Files:**
- Modify: `dump_tool/src/GuiApp.cpp`

**Step 1: Change ListView style helper**
- In `ApplyListViewModernStyle`:
  - Add `LVS_EX_INFOTIP` so truncated cells show tooltips.
  - Ensure we do not overwrite the dark theme later with an `Explorer` theme call.

**Step 2: Ensure ListViews are consistently styled**
- Remove any post-creation calls that overwrite ListView theme (`ApplyExplorerTheme`) when we already set `DarkMode_Explorer`.

**Step 3: Build (Windows)**
- Run: `scripts\\build-win.cmd`
- Expected: build succeeds.

---

### Task 2: Modern bottom buttons (owner-draw)

**Files:**
- Modify: `dump_tool/src/GuiApp.cpp`

**Step 1: Switch buttons to `BS_OWNERDRAW`**
- Apply to: Open dump / Open folder / Copy summary / Copy checklist / Language toggle.

**Step 2: Implement `WM_DRAWITEM` handler**
- Draw rounded rectangle background + border, proper hover/pressed/disabled states.
- Keep one “primary” action (Open dump) visually emphasized.

**Step 3: Manual verify**
- Launch `SkyrimDiagDumpTool.exe` and confirm:
  - hover/pressed states render correctly
  - focus rectangle appears for keyboard navigation

---

### Task 3: Improve text readability (padding + mono font for WCT)

**Files:**
- Modify: `dump_tool/src/GuiApp.cpp`

**Step 1: Add padding inside EDIT controls**
- Use `EM_SETMARGINS` (left/right) for:
  - Summary edit
  - WCT edit
  - Events filter edit

**Step 2: Use a monospaced font for WCT**
- Create a `Consolas` (fallback to `Segoe UI`) font sized with DPI scaling.
- Apply only to `st->wctEdit`.

**Step 3: Build (Windows)**
- Run: `scripts\\build-win.cmd`
- Expected: build succeeds.

---

### Task 4: Optional DWM cosmetics (Windows 11 safe)

**Files:**
- Modify: `dump_tool/src/GuiApp.cpp`

**Step 1: Rounded corners (best-effort)**
- Call `DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, DWMWCP_ROUND/ROUNDSMALL)` if available.
- Ignore failures (older Windows / unsupported builds).

**Step 2: Manual verify**
- On Windows 11: corners look modern.
- On Windows 10: no behavior change.

---

### Task 5: Package updated zip

**Files:**
- (No code changes required unless packaging fails)

**Step 1: Sync WSL → Windows mirror**
- `rsync` repo to `C:\\Users\\kdw73\\Tullius_ctd_loger`

**Step 2: Build + package**
- Build: `scripts\\build-win.cmd`
- Package:
  - `python scripts\\package.py --build-dir build-win --out dist\\Tullius_ctd_loger-v0.2.2-ui.zip`
  - `python scripts\\package.py --build-dir build-win --out dist\\Tullius_ctd_loger-v0.2.2-ui-no-pdb.zip --no-pdb`


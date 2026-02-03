# Repo Agent Instructions (SkyrimDiag / Tullius_ctd_loger)

<!-- skills-scout:start -->
## Skills (Auto-Pinned by skills-scout)

This section is generated. Re-run pinning to update.

### Available skills
- (none matched this repo)
<!-- skills-scout:end -->

## Recommended Skills for This Repo (Manual Pin)

The auto-pinner currently doesn’t match any skills for this repo, but we *do* have relevant global skills installed.
When doing code review / refactoring / decoupling / architecture work on this repo, prefer:

- `receiving-code-review`: Evaluate external review feedback before implementing.
- `code-review-excellence`: Produce thorough, actionable code review notes.
- `refactor`: Surgical refactors without behavior change.
- `changelog-automation`: Draft consistent release notes / changelog entries.
- `architecture-designer`: Architecture review + trade-offs.
- `architecture-patterns`: Decoupling guidance (Clean/Hexagonal patterns).
- `architecture-decision-records`: Write/maintain ADRs for major design decisions.
- `c4-architecture`: Generate C4 Mermaid diagrams for documentation.

Superpowers (invoke via `~/.codex/superpowers/.codex/superpowers-codex use-skill <name>`):
- `superpowers:systematic-debugging`: Root-cause-first bug fixing.
- `superpowers:verification-before-completion`: Evidence before “fixed”.
- `superpowers:test-driven-development`: Add minimal failing tests before fixes.

## Project Overview

- **Purpose:** Skyrim SE/AE용 진단 도구 (SKSE 플러그인 + 외부 Helper + DumpTool Viewer)
- **Components**
  - `plugin/` : `SkyrimDiag.dll` (SKSE 플러그인, 블랙박스 이벤트/리소스 기록)
  - `helper/` : `SkyrimDiagHelper.exe` (out-of-proc, CTD/프리징 감지 및 덤프 생성)
  - `dump_tool/` : `SkyrimDiagDumpTool.exe` (덤프/스트림 분석 + UI 뷰어)

## Build / Package (Windows)

이 레포는 WSL에서 편집하지만, **Windows/MSVC로 빌드 + zip 패키징**합니다.

### Windows 빌드 워크스페이스(현재 기준)

- **Windows mirror 소스:** `C:\Users\kdw73\Tullius_ctd_loger`
- **WSL 경로:** `/mnt/c/Users/kdw73/Tullius_ctd_loger`

> 참고: `/home/kdw73/Tullius_ctd_loger`(WSL git repo)와 `C:\Users\kdw73\Tullius_ctd_loger`(Windows mirror)는 별도 경로입니다.  
> WSL에서 수정 후, Windows mirror로 변경분을 동기화한 다음 빌드/패키징을 수행합니다.

### Build

- Run: `scripts\build-win.cmd`
- Output build dir: `C:\Users\kdw73\SkyrimDiag\build-win`

### Package (MO2 설치용 zip)

- Run: `python scripts\package.py --build-dir build-win --out dist\Tullius_ctd_loger.zip`
- **ZIP output (필수 경로):** `C:\Users\kdw73\Tullius_ctd_loger\dist\Tullius_ctd_loger.zip`
- **WSL 경로:** `/mnt/c/Users/kdw73/Tullius_ctd_loger/dist/Tullius_ctd_loger.zip`

## Runtime Notes (MO2)

- MO2에서 zip를 모드로 설치하면 기본적으로 `SKSE/Plugins/`에 배치됩니다.
- 결과물(덤프/JSON 등)은 보통 MO2 `overwrite\SKSE\Plugins\`에 생성됩니다.
- **수동 캡처 핫키:** `Ctrl+Shift+F12`

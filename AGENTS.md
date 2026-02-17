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

Superpowers:
- Superpowers skills are available under `~/.agents/skills/superpowers/` (auto-discovered).
- Open the corresponding `SKILL.md` and follow the checklist.
- `superpowers:systematic-debugging`: Root-cause-first bug fixing.
- `superpowers:verification-before-completion`: Evidence before “fixed”.
- `superpowers:test-driven-development`: Add minimal failing tests before fixes.

## Project Overview

- **Purpose:** Skyrim SE/AE용 진단 도구 (SKSE 플러그인 + 외부 Helper + DumpTool Viewer)
- **Components**
  - `plugin/` : `SkyrimDiag.dll` (SKSE 플러그인, 블랙박스 이벤트/리소스 기록)
  - `helper/` : `SkyrimDiagHelper.exe` (out-of-proc, CTD/프리징 감지 및 덤프 생성)
  - `dump_tool/` : `SkyrimDiagDumpToolNative.dll` (덤프/스트림 분석 엔진)
  - `dump_tool_cli/` : `SkyrimDiagDumpToolCli.exe` (헤드리스 분석기, 창 없음)
  - `dump_tool_winui/` : `SkyrimDiagDumpToolWinUI.exe` (WinUI 뷰어 셸)

## Docs (Quick Links)

- End-users:
  - `README.md` (KO main)
  - `docs/README_KO.md` (KO expanded)
  - `docs/BETA_TESTING.md` (issue template / reporting guide)
- Contributors:
  - `docs/DEVELOPMENT.md`

## Tests (Fast)

Linux:
```bash
ctest --test-dir build-linux-test --output-on-failure
```

## Build / Package (Windows)

이 레포는 WSL에서 편집하지만, **Windows/MSVC로 빌드 + zip 패키징**합니다.

## Release Notes Policy

- GitHub Release 패치노트(Release notes)는 **한국어(KO)를 필수**로 작성합니다.
- 영어가 필요하면 한국어 아래에 짧게 추가(선택)하되, **한국어를 우선**합니다.

### Windows 빌드 워크스페이스(현재 기준)

- **Windows mirror 소스:** `C:\Users\kdw73\Tullius_ctd_loger`
- **WSL 경로:** `/mnt/c/Users/kdw73/Tullius_ctd_loger`

> 참고: `/home/kdw73/Tullius_ctd_loger`(WSL git repo)와 `C:\Users\kdw73\Tullius_ctd_loger`(Windows mirror)는 별도 경로입니다.  
> WSL에서 수정 후, Windows mirror로 변경분을 동기화한 다음 빌드/패키징을 수행합니다.

### Build

- Run: `scripts\build-win.cmd`
- WinUI: `scripts\build-winui.cmd`
- Output build dir: `C:\Users\kdw73\SkyrimDiag\build-win`

### Package (MO2 설치용 zip)

- Run: `python scripts\package.py --build-dir build-win --out dist\Tullius_ctd_loger.zip`
- **ZIP output (필수 경로):** `C:\Users\kdw73\Tullius_ctd_loger\dist\Tullius_ctd_loger.zip`
- **WSL 경로:** `/mnt/c/Users/kdw73/Tullius_ctd_loger/dist/Tullius_ctd_loger.zip`

### Release Hard Gate (WinUI 실행 회귀 방지)

아래 항목을 **모두 통과하기 전에는 릴리즈(프리/정식) 생성 금지**.

1. WSL -> Windows mirror 스크립트 동기화 확인(최소 아래 2개):
   - `scripts/build-winui.cmd`
   - `scripts/package.py`
   - 권장: `sha256sum /home/kdw73/Tullius_ctd_loger/scripts/build-winui.cmd /mnt/c/Users/kdw73/Tullius_ctd_loger/scripts/build-winui.cmd`
   - 권장: `sha256sum /home/kdw73/Tullius_ctd_loger/scripts/package.py /mnt/c/Users/kdw73/Tullius_ctd_loger/scripts/package.py`
2. WinUI 빌드 산출물 필수 파일 존재 확인(`build-winui`):
   - `SkyrimDiagDumpToolWinUI.exe`
   - `SkyrimDiagDumpToolWinUI.pri`
   - `App.xbf`
   - `MainWindow.xbf`
3. 패키징은 기본적으로 `--no-pdb` 사용:
   - `python scripts/package.py --build-dir build-win --out dist/Tullius_ctd_loger.zip --no-pdb`
4. ZIP 내부 필수 파일 검증:
   - `SKSE/Plugins/SkyrimDiagWinUI/SkyrimDiagDumpToolWinUI.exe`
   - `SKSE/Plugins/SkyrimDiagWinUI/SkyrimDiagDumpToolWinUI.pri`
   - `SKSE/Plugins/SkyrimDiagWinUI/App.xbf`
   - `SKSE/Plugins/SkyrimDiagWinUI/MainWindow.xbf`
5. 위 1~4 중 하나라도 실패하면 원인 수정 후 재빌드/재패키징. 예외 없이 릴리즈 업로드 중단.

## Runtime Notes (MO2)

- MO2에서 zip를 모드로 설치하면 기본적으로 `SKSE/Plugins/`에 배치됩니다.
- 결과물(덤프/JSON 등)은 보통 MO2 `overwrite\SKSE\Plugins\`에 생성됩니다.
- **수동 캡처 핫키:** `Ctrl+Shift+F12`

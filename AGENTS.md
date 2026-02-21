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
5. ZIP 용량 가드(중복 패키징 회귀 방지):
   - `ls -lh /mnt/c/Users/kdw73/Tullius_ctd_loger/dist/Tullius_ctd_loger.zip`
   - 정상 범위 가이드: 대략 `8MB ~ 25MB`
   - `25MB` 초과 시 릴리즈 업로드 금지(중첩 산출물 포함 여부 재점검 후 재패키징)
6. ZIP 중첩 경로 가드(WinUI 중복 파일 금지):
   - `unzip -l /mnt/c/Users/kdw73/Tullius_ctd_loger/dist/Tullius_ctd_loger.zip | rg "SKSE/Plugins/SkyrimDiagWinUI/(publish|win-x64|x64)/"`
   - 출력이 한 줄이라도 있으면 릴리즈 업로드 금지
7. 위 1~6 중 하나라도 실패하면 원인 수정 후 재빌드/재패키징. 예외 없이 릴리즈 업로드 중단.

## Runtime Notes (MO2)

- MO2에서 zip를 모드로 설치하면 기본적으로 `SKSE/Plugins/`에 배치됩니다.
- 결과물(덤프/JSON 등)은 보통 MO2 `overwrite\SKSE\Plugins\`에 생성됩니다.
- **수동 캡처 핫키:** `Ctrl+Shift+F12`


## Planning mode policy
- 모든 작업은 시작 전에 반드시 Plan Mode에서 계획을 수립한다.
- 구현/수정/실행(변경을 유발하는 작업)은 사용자가 명시적으로 "플랜 종료" 또는 "실행"을 지시한 후에만 진행한다.
- 하나의 플랜이 완료되어도 자동 전환하지 않고, 다음 작업도 기본적으로 Plan Mode에서 시작한다.
- 상위 시스템/플랫폼 정책이 강제하는 경우에는 해당 정책을 우선 적용한다.

## Experimental Features (Codex)

- Experimental toggles in `config.toml` control feature availability.
- If enabled, the agent may use these features when relevant even if not explicitly listed in this repo file.
- Usage policy still follows user request + AGENTS policy precedence.
- `use_linux_sandbox_bwrap`: prefer on Linux for safer command isolation.
- `multi_agent`: use only for independent parallel tasks (no shared state/conflicting edits).
- `apps`: use only when installed/connected; prefer explicit user intent (for example `$AppName`).

## Multi-agent Practical Ops (Recommended)

<IMPORTANT>
This section defines operational defaults so `multi_agent` is used aggressively where it helps, and avoided where it hurts.

Decision rule:
- Use multi-agent when there are 2+ independent tasks with no shared state, no file ownership conflict, and no strict ordering dependency.
- Use single-agent when tasks are tightly coupled, touch the same files, or require sequential architecture decisions.

Execution playbook:
1) Triage first: split work into independent domains and identify shared-risk areas.
2) Dispatch in parallel for exploration/investigation tasks.
3) Dispatch in parallel for implementation only when modules are independent and ownership is explicit.
4) Integrate under one owner agent to resolve conflicts and keep design consistency.
5) Final verification stays single-owner: run full tests/lint/typecheck/build once after integration.

Default operating stance:
- Exploration/analysis phase: aggressively use multi-agent.
- Implementation phase: use multi-agent only for independent modules.
- Integration/verification phase: one owner agent finalizes.

Guardrails:
- Prefer a small number of focused agents over many broad agents.
- If overlap is discovered mid-run, stop parallel edits and switch to single-owner integration.
- Report per-agent outcomes and a single merged verification summary.
</IMPORTANT>

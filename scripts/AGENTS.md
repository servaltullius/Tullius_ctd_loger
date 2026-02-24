# SCRIPTS KNOWLEDGE BASE

## OVERVIEW
`scripts/` holds operational entry points for Windows builds, WinUI packaging, release zip creation, and release gate checks.

## STRUCTURE
```text
scripts/
|- build-win.cmd           # C++ build entry (MSVC/Ninja/vcpkg)
|- build-winui.cmd         # WinUI output validation and copy
|- package.py              # zip assembly and artifact checks
|- verify_release_gate.sh  # pre-release hard gate
`- vibe*.py/cmd            # local tooling helpers
```

## WHERE TO LOOK
| Task | Location | Notes |
|------|----------|-------|
| Windows C++ build | `scripts/build-win.cmd` | VS detection + Ninja + vcpkg |
| WinUI build | `scripts/build-winui.cmd` | validates `.exe/.pri/.xbf` outputs |
| Zip packaging | `scripts/package.py` | MO2 layout assembly and artifact checks |
| Release gate | `scripts/verify_release_gate.sh` | hard blockers before release upload |
| Vibe tooling | `scripts/vibe.py` | repo-local assistant diagnostics/config |

## CONVENTIONS
- Release packaging defaults to `--no-pdb` for shipping artifacts.
- Keep WSL path and Windows mirror path assumptions intact in gate scripts.
- Validate required WinUI assets before packaging success.
- Keep package naming in `Tullius_ctd_loger_v{version}.zip` format.

## COMMANDS
```bash
scripts\\build-win.cmd
scripts\\build-winui.cmd
python scripts/package.py --build-dir build-win --out dist/Tullius_ctd_loger.zip --no-pdb
bash scripts/verify_release_gate.sh
```

## ANTI-PATTERNS
- Do not remove zip size/nested-path guards.
- Do not change package layout under `SKSE/Plugins` without matching tests/docs.
- Do not hardcode behavior that breaks WSL<->Windows mirror verification.
- Do not skip required WinUI artifact presence checks in build/package path.

## NOTES
- `verify_release_gate.sh` is a release blocker, not advisory guidance.
- Changes here often require parallel updates to docs and packaging tests.

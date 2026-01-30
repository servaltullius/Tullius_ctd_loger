# AGENT_CHECKLIST (vibe-kit)

## Quick start (do this first)
- Read: `.vibe/context/LATEST_CONTEXT.md`
- (Recommended once, after install) Auto-configure for this repo: `python3 scripts/vibe.py configure --apply`
- If you run only one command:
  - (WSL/Linux) `python3 scripts/vibe.py doctor --full`
  - (Windows) `scripts\\vibe.cmd doctor --full`

## Before coding
- Read: `.vibe/agent_memory/DONT_DO_THIS.md`
- Check impact for shared/core files: `python3 scripts/vibe.py impact <path>`
- Find entry points fast: `python3 scripts/vibe.py search "<keyword>"`
- (Optional) Detect boundary violations (architecture rules): `python3 scripts/vibe.py boundaries`
- (Optional) Find logical coupling from git history: `python3 scripts/vibe.py coupling`
- (Optional) Configure repo-specific checks in `.vibe/config.json` (`checks.doctor`, `checks.precommit`).
- (Optional) Make a compact context pack for an agent:
  - `python3 scripts/vibe.py pack --scope=staged|changed|path|recent --out .vibe/context/PACK.md`

## While coding
- Keep changes small and localized.
- Treat placeholders/tokens as runtime contracts (e.g. `<...>`, `{0}`, `%s`) and update tests if you change them.

## Before finishing
- Run: `python3 scripts/vibe.py doctor --full` (or at least `python3 scripts/vibe.py doctor`)
- Run the repo's normal tests/lint (e.g. `pytest`, `npm test`, `dotnet test`, etc.)

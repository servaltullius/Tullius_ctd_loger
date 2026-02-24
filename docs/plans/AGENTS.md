# PLANS KNOWLEDGE BASE

## OVERVIEW
`docs/plans/` stores implementation plans and verification checklists used as execution records.

## STRUCTURE
```text
docs/plans/
|- YYYY-MM-DD-*.md   # dated plan and implementation records
`- AGENTS.md         # local authoring constraints
```

## WHERE TO LOOK
| Task | Location | Notes |
|------|----------|-------|
| Current execution plans | `docs/plans/*.md` | date-prefixed plan files |
| Verification commands | `docs/plans/*.md` | ctest/build command history |
| Safety constraints | `docs/plans/*.md` | explicit do-not-change guidance |
| Refactor phases | `docs/plans/*refactor*.md` | multi-step migration records |

## CONVENTIONS
- Keep plans concrete: target files, commands, expected outputs.
- Keep verification commands reproducible (`cmake --build`, `ctest --output-on-failure`).
- Keep policy language explicit when introducing guardrails.
- Keep dates and scope clear in filenames for chronological traceability.

## COMMANDS
```bash
grep -n "ctest --test-dir" docs/plans/*.md
grep -n "DO NOT\|NEVER\|절대" docs/plans/*.md
```

## ANTI-PATTERNS
- Do not document speculative changes without verification steps.
- Do not remove prior constraints unless superseded and justified.
- Do not write generic templates disconnected from repo files/commands.
- Do not rewrite execution history after implementation without explicit correction notes.

## NOTES
- These plans encode many repository-specific hard constraints.
- When policy conflicts appear, prefer newer dated plans plus ADR decisions.

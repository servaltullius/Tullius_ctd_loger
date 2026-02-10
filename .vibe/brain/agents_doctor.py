#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path

from context_db import is_excluded, load_config


AGENT_FILES = [
    "AGENTS.md",
    "AGENTS.override.md",
    "CLAUDE.md",
    "GEMINI.md",
    ".github/copilot-instructions.md",
    ".cursor/rules/vibekit.md",
]

REQUIRED_CONTEXT_HINTS = [
    ".vibe/AGENT_CHECKLIST.md",
    ".vibe/context/LATEST_CONTEXT.md",
]

REQUIRED_DOCTOR_HINTS = [
    "python3 scripts/vibe.py doctor --full",
    "scripts\\vibe.cmd doctor --full",
]


def _discover_agent_files(root: Path, exclude_dirs: list[str]) -> list[Path]:
    seen: set[Path] = set()
    out: list[Path] = []

    for rel in AGENT_FILES:
        p = root / rel
        if not p.exists() or not p.is_file():
            continue
        rel_p = p.relative_to(root)
        if is_excluded(rel_p, exclude_dirs):
            continue
        if rel_p in seen:
            continue
        seen.add(rel_p)
        out.append(rel_p)

    # Also discover nested AGENTS.md files to catch sub-package overrides.
    for name in ("AGENTS.md", "AGENTS.override.md"):
        for p in root.rglob(name):
            try:
                rel_p = p.relative_to(root)
            except ValueError:
                continue
            if is_excluded(rel_p, exclude_dirs):
                continue
            if rel_p in seen:
                continue
            seen.add(rel_p)
            out.append(rel_p)

    return sorted(out)


def _check_file(content: str) -> list[str]:
    issues: list[str] = []
    if not any(token in content for token in REQUIRED_CONTEXT_HINTS):
        issues.append("missing context entrypoint (.vibe/AGENT_CHECKLIST.md or .vibe/context/LATEST_CONTEXT.md)")
    if not any(token in content for token in REQUIRED_DOCTOR_HINTS):
        issues.append("missing first-action command (`python3 scripts/vibe.py doctor --full`)")
    return issues


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Validate agent instruction files include vibe-kit entrypoints.")
    ap.add_argument("--fail", action="store_true", help="Exit non-zero when warnings are found.")
    args = ap.parse_args(argv)

    cfg = load_config()
    files = _discover_agent_files(cfg.root, cfg.exclude_dirs)
    if not files:
        print("[agents-doctor] WARN: no agent instruction files found")
        return 1 if args.fail else 0

    warnings = 0
    for rel in files:
        path = cfg.root / rel
        try:
            content = path.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            warnings += 1
            print(f"[agents-doctor] WARN: {rel} unreadable")
            continue

        issues = _check_file(content)
        if issues:
            warnings += 1
            for issue in issues:
                print(f"[agents-doctor] WARN: {rel} {issue}")
        else:
            print(f"[agents-doctor] OK: {rel}")

    if warnings and args.fail:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(__import__("sys").argv[1:]))

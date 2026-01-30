#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import stat
import sys
from pathlib import Path


HOOK_TEMPLATE = """#!/usr/bin/env python3
from __future__ import annotations

import runpy
from pathlib import Path

repo_root = Path(__file__).resolve().parents[2]
runpy.run_path(str(repo_root / ".vibe" / "brain" / "precommit.py"), run_name="__main__")
"""


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Install git hooks for vibe-kit.")
    parser.add_argument("--force", action="store_true", help="Overwrite existing hooks.")
    args = parser.parse_args(argv)

    root = _repo_root()
    git_dir = root / ".git"
    if not git_dir.exists():
        print("[vibe-kit] no .git directory found; skipping hook install.")
        return 0

    hooks_dir = git_dir / "hooks"
    hooks_dir.mkdir(parents=True, exist_ok=True)
    precommit_path = hooks_dir / "pre-commit"

    if precommit_path.exists() and not args.force:
        print("[vibe-kit] pre-commit hook already exists (use --force to overwrite).")
        return 0

    precommit_path.write_text(HOOK_TEMPLATE, encoding="utf-8", newline="\n")
    try:
        mode = precommit_path.stat().st_mode
        precommit_path.chmod(mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    except OSError:
        pass

    print(f"[vibe-kit] installed: {precommit_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

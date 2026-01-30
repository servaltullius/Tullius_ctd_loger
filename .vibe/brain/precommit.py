#!/usr/bin/env python3
from __future__ import annotations

import argparse
import fnmatch
import subprocess
import sys
from pathlib import Path

from context_db import is_excluded, load_config
from custom_checks import run_custom_checks


def _git_root(cfg) -> Path | None:
    git_dir = cfg.root / ".git"
    return cfg.root if git_dir.exists() else None


def _staged_files(cfg) -> list[str]:
    cmd = ["git", "diff", "--cached", "--name-only", "--diff-filter=ACM"]
    p = subprocess.run(cmd, cwd=str(cfg.root), stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    if p.returncode != 0:
        return []
    files = [line.strip() for line in p.stdout.splitlines() if line.strip()]
    return files


def _run(py: Path, args: list[str]) -> int:
    cmd = [sys.executable, str(py), *args]
    p = subprocess.run(cmd)
    return p.returncode


def _matches_include(rel_posix: str, include_globs: list[str]) -> bool:
    if not include_globs:
        return True
    for g in include_globs:
        if fnmatch.fnmatch(rel_posix, g):
            return True
        # Treat "**/" prefix as optional so patterns like "**/*.md" match root files too.
        if g.startswith("**/") and fnmatch.fnmatch(rel_posix, g[3:]):
            return True
    return False


def _matches_any(rel_posix: str, globs: list[str]) -> bool:
    for g in globs:
        if fnmatch.fnmatch(rel_posix, g):
            return True
        if g.startswith("**/") and fnmatch.fnmatch(rel_posix, g[3:]):
            return True
    return False


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="vibe-kit pre-commit (staged-only, fast).")
    parser.add_argument("--run-tests", action="store_true", help="Also run core tests (slower).")
    args = parser.parse_args(argv)

    cfg = load_config()
    if _git_root(cfg) is None:
        print("[precommit] no .git directory found; skipping.")
        return 0

    staged = _staged_files(cfg)

    if not staged:
        print("[precommit] no staged files.")
        return 0

    brain = cfg.root / ".vibe" / "brain"

    staged_rel: list[str] = []

    # Update index for staged files only.
    for f in staged:
        rel = Path(f)
        if rel.is_absolute():
            continue
        if is_excluded(rel, cfg.exclude_dirs):
            continue
        if not _matches_include(rel.as_posix(), cfg.include_globs):
            continue
        if not (cfg.root / rel).exists():
            continue
        staged_rel.append(f)
        _run(brain / "indexer.py", ["--file", f])

    rc = 0

    # Typecheck gate only if C# code/project changed.
    staged_cs = [f for f in staged_rel if f.endswith(".cs")]
    staged_proj = [f for f in staged_rel if f.endswith(".csproj") or f.endswith(".sln")]

    typecheck_on_precommit = bool(cfg.quality_gates.get("typecheck_run_on_precommit", False))
    typecheck_globs_raw = cfg.quality_gates.get("typecheck_when_any_glob", None)
    typecheck_globs = (
        [str(g).strip() for g in typecheck_globs_raw if isinstance(g, str) and g.strip()]
        if isinstance(typecheck_globs_raw, list)
        else []
    )
    typecheck_by_glob = bool(typecheck_globs) and any(_matches_any(Path(f).as_posix(), typecheck_globs) for f in staged_rel)

    if staged_cs or staged_proj or typecheck_on_precommit or typecheck_by_glob:
        rc = max(rc, _run(brain / "typecheck_baseline.py", []))

    # Cycle detection only if project files changed.
    if staged_proj:
        rc = max(rc, _run(brain / "check_circular.py", []))

    # Complexity: warnings only (staged .cs).
    if staged_cs:
        _run(brain / "check_complexity.py", ["--files", *staged_cs])

    if args.run_tests:
        rc = max(rc, _run(brain / "run_core_tests.py", ["--fast"]))

    checks_cfg = cfg.checks or {}
    precommit_checks = checks_cfg.get("precommit")
    if isinstance(precommit_checks, list) and precommit_checks:
        report_path = cfg.root / ".vibe" / "reports" / "custom_checks_precommit.json"
        _, failures = run_custom_checks(cfg, precommit_checks, report_path=report_path, default_timeout_sec=60)
        if failures:
            rc = 1

    if rc != 0:
        print("[precommit] FAIL")
    else:
        print("[precommit] OK")
    return 1 if rc != 0 else 0


if __name__ == "__main__":
    raise SystemExit(main(__import__("sys").argv[1:]))

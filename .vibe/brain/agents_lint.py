#!/usr/bin/env python3
from __future__ import annotations

import argparse
import sys
from pathlib import Path

from context_db import is_excluded, load_config


def _find_agents_files(root: Path, exclude_dirs: list[str]) -> list[Path]:
    patterns = ["AGENTS.md", "AGENTS.override.md"]
    found: list[Path] = []
    for name in patterns:
        for p in root.rglob(name):
            try:
                rel = p.relative_to(root)
            except ValueError:
                continue
            if is_excluded(rel, exclude_dirs):
                continue
            found.append(rel)
    return sorted(set(found))


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Lint AGENTS.md files for Codex size/truncation risk.")
    ap.add_argument("--max-kb", type=int, default=32, help="Warn if file is near/over this size (KiB).")
    ap.add_argument("--fail", action="store_true", help="Exit non-zero when warnings are found.")
    args = ap.parse_args(argv)

    cfg = load_config()
    root = cfg.root
    max_bytes = max(1, int(args.max_kb)) * 1024
    warn_at = int(max_bytes * 0.9)

    files = _find_agents_files(root, cfg.exclude_dirs)
    if not files:
        print("[agents] no AGENTS.md files found")
        return 0

    warnings = 0
    for rel in files:
        path = root / rel
        try:
            size = path.stat().st_size
        except FileNotFoundError:
            continue

        if size > max_bytes:
            warnings += 1
            print(f"[agents] WARN: {rel} is {size} bytes (>{max_bytes}); risk of truncation")
        elif size >= warn_at:
            warnings += 1
            print(f"[agents] WARN: {rel} is {size} bytes (near {max_bytes}); consider slimming/splitting")
        else:
            print(f"[agents] OK: {rel} is {size} bytes")

    if warnings and args.fail:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(__import__("sys").argv[1:]))


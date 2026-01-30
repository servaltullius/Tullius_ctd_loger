#!/usr/bin/env python3
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

from context_db import is_excluded, load_config


def _run(cmd: list[str], cwd: Path) -> tuple[int, str]:
    if shutil.which(cmd[0]) is None:
        return 127, f"{cmd[0]} not found"
    p = subprocess.run(cmd, cwd=str(cwd), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return p.returncode, p.stdout


def _pick_dotnet_test_project(cfg) -> Path | None:
    root = cfg.root
    csprojs: list[Path] = []
    for p in root.rglob("*.csproj"):
        rel = p.relative_to(root)
        if is_excluded(rel, cfg.exclude_dirs):
            continue
        csprojs.append(p)
    if not csprojs:
        return None
    csprojs.sort(key=lambda x: x.as_posix())

    def score(p: Path) -> int:
        s = 0
        name = p.name.lower()
        path = p.as_posix().lower()
        if "/tests/" in path or name.endswith(".tests.csproj") or ".tests" in name:
            s += 100
        if "test" in name:
            s += 50
        if "core" in name:
            s -= 10
        if "app" in name or "ui" in name:
            s -= 30
        return s

    best = sorted(csprojs, key=lambda p: (-score(p), p.as_posix()))[0]
    if score(best) <= 0:
        return None
    return best


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Run core tests (optional).")
    parser.add_argument("--fast", action="store_true", help="Fast mode (default).")
    args = parser.parse_args(argv)

    cfg = load_config()
    test_proj = _pick_dotnet_test_project(cfg)
    if test_proj is None:
        print("[tests] SKIP: no dotnet test project found")
        return 0

    cmd = ["dotnet", "test", str(test_proj.relative_to(cfg.root)), "-c", "Release"]
    if args.fast:
        cmd += ["--no-build"]

    rc, out = _run(cmd, cfg.root)
    if rc == 0:
        print("[tests] OK")
        return 0

    log_path = cfg.root / ".vibe" / "reports" / "test_failures.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_text(out, encoding="utf-8")
    print(f"[tests] FAIL (see {log_path})")
    return 1


if __name__ == "__main__":
    raise SystemExit(main(__import__("sys").argv[1:]))

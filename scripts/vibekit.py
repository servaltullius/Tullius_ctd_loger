#!/usr/bin/env python3
from __future__ import annotations

import subprocess
import sys
from pathlib import Path


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def main(argv: list[str]) -> int:
    root = _repo_root()
    vibe = root / "scripts" / "vibe.py"
    if not vibe.exists():
        print("[vibekit] missing scripts/vibe.py", file=sys.stderr)
        return 1
    return subprocess.call([sys.executable, str(vibe), *argv], cwd=str(root))


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

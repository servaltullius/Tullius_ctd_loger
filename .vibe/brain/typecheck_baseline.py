#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import time
from pathlib import Path

from context_db import is_excluded, load_config


DOTNET_DIAG_RE = re.compile(
    r"^\s*(?P<file>[^:]+):(?P<line>\d+):(?P<col>\d+):\s+(?P<kind>error|warning)\s+(?P<code>[A-Z]{2}\d+):\s+(?P<msg>.*)$"
)

# Example:
#   src/index.ts(12,3): error TS2322: Type 'string' is not assignable to type 'number'.
TSC_DIAG_RE = re.compile(
    r"^\s*(?P<file>[^()]+)\((?P<line>\d+),(?P<col>\d+)\):\s+(?P<kind>error|warning)\s+(?P<code>[A-Za-z]{2}\d+):\s+(?P<msg>.*)$"
)

# Example:
#   src/app.py:10: error: Incompatible return value type  [return-value]
MYPY_DIAG_RE = re.compile(
    r"^\s*(?P<file>[^:]+):(?P<line>\d+):\s+(?P<kind>error|warning|note):\s+(?P<msg>.*?)(?:\s+\[(?P<code>[^\]]+)\])?\s*$"
)


def _iter_dotnet_projects(cfg) -> tuple[list[Path], list[Path]]:
    root = cfg.root
    slns: list[Path] = []
    csprojs: list[Path] = []
    for p in root.rglob("*.sln"):
        rel = p.relative_to(root)
        if is_excluded(rel, cfg.exclude_dirs):
            continue
        slns.append(p)
    for p in root.rglob("*.csproj"):
        rel = p.relative_to(root)
        if is_excluded(rel, cfg.exclude_dirs):
            continue
        csprojs.append(p)
    slns.sort(key=lambda x: x.as_posix())
    csprojs.sort(key=lambda x: x.as_posix())
    return slns, csprojs


def _pick_dotnet_build_target(cfg) -> Path | None:
    slns, csprojs = _iter_dotnet_projects(cfg)
    prefer_solution = bool(cfg.quality_gates.get("typecheck_prefer_solution", False))
    if prefer_solution and slns:
        # Prefer root-level solution, else take the first one.
        root_slns = [p for p in slns if p.parent == cfg.root]
        return root_slns[0] if root_slns else slns[0]

    if not csprojs:
        return None

    def score(p: Path) -> int:
        s = 0
        name = p.name.lower()
        path = p.as_posix().lower()
        if "core" in name:
            s += 50
        if "lib" in name or "library" in name:
            s += 30
        if "/tests/" in path or name.endswith(".tests.csproj") or ".tests" in name:
            s -= 100
        if "test" in name:
            s -= 80
        if "app" in name or "ui" in name or "wpf" in name:
            s -= 30
        if "/src/" in path:
            s += 10
        return s

    return sorted(csprojs, key=lambda p: (-score(p), p.as_posix()))[0]


def _default_cmd(cfg) -> list[str] | None:
    target = _pick_dotnet_build_target(cfg)
    if target is None:
        return None
    return ["dotnet", "build", str(target.relative_to(cfg.root)), "-c", "Release"]


def _custom_cmd(cfg) -> list[str] | None:
    raw = cfg.quality_gates.get("typecheck_cmd", None)
    if raw is None:
        raw = cfg.quality_gates.get("typecheck_command", None)

    if isinstance(raw, list) and raw and all(isinstance(x, str) and x.strip() for x in raw):
        return [str(x) for x in raw]
    if isinstance(raw, str) and raw.strip():
        # Best-effort; prefer list[str] for cross-platform correctness.
        try:
            return shlex.split(raw)
        except ValueError:
            return raw.split()
    return None


def _run(cmd: list[str], cwd: Path) -> tuple[int, str]:
    if shutil.which(cmd[0]) is None:
        return 127, f"{cmd[0]} not found"
    p = subprocess.run(cmd, cwd=str(cwd), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return p.returncode, p.stdout


def _parse_diagnostics(output: str) -> dict[str, list[str]]:
    errors: list[str] = []
    warnings: list[str] = []
    for line in output.splitlines():
        m = DOTNET_DIAG_RE.match(line) or TSC_DIAG_RE.match(line) or MYPY_DIAG_RE.match(line)
        if m:
            kind = (m.group("kind") or "").strip().lower()
            code = (m.groupdict().get("code") or "").strip()
            msg = (m.group("msg") or "").strip()
            if code:
                key = f"{code}:{msg}"
            else:
                key = msg

            if kind == "error":
                errors.append(key)
            elif kind in {"warning", "note"}:
                warnings.append(key)
            continue

        # Fallback heuristics for unknown formats.
        lo = line.lower()
        if " error " in f" {lo} ":
            errors.append(line.strip())
        elif " warning " in f" {lo} ":
            warnings.append(line.strip())
    return {"errors": errors, "warnings": warnings}


def _baseline_path(cfg) -> Path:
    return cfg.root / ".vibe" / "context" / "typecheck_baseline.json"


def _status_path(cfg) -> Path:
    return cfg.root / ".vibe" / "reports" / "typecheck_status.json"


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="typecheck baseline gate (errors must not increase).")
    parser.add_argument("--init", action="store_true", help="Initialize baseline from current output.")
    parser.add_argument("--cmd", nargs=argparse.REMAINDER, help="Override command (after --cmd).")
    args = parser.parse_args(argv)

    cfg = load_config()
    cmd = args.cmd if args.cmd else (_custom_cmd(cfg) or _default_cmd(cfg))
    if not cmd:
        status_path = _status_path(cfg)
        status_path.parent.mkdir(parents=True, exist_ok=True)
        status_path.write_text(
            json.dumps(
                {"skipped": True, "reason": "no .NET solution/project found", "rc": 0},
                ensure_ascii=False,
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )
        print("[typecheck] SKIP: no .NET solution/project found")
        return 0

    rc, out = _run(cmd, cfg.root)
    diags = _parse_diagnostics(out)
    cur_err = len(diags["errors"])
    cur_warn = len(diags["warnings"])

    base_path = _baseline_path(cfg)
    status_path = _status_path(cfg)
    status_path.parent.mkdir(parents=True, exist_ok=True)

    if args.init or not base_path.exists():
        payload = {
            "timestamp": time.time(),
            "cmd": cmd,
            "errors_count": cur_err,
            "warnings_count": cur_warn,
            "errors": sorted(set(diags["errors"]))[:500],
            "warnings": sorted(set(diags["warnings"]))[:500],
            "cmd_rc": rc,
        }
        base_path.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        status_path.write_text(
            json.dumps(
                {"baseline_errors": cur_err, "current_errors": cur_err, "increased": False, "rc": rc},
                ensure_ascii=False,
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )
        print(f"[typecheck] baseline saved: {base_path} (errors={cur_err}, warnings={cur_warn}, rc={rc})")
        if rc == 127:
            print(f"[typecheck] note: {cmd[0]} not found; baseline is informational only.", file=sys.stderr)
        return 0

    baseline = json.loads(base_path.read_text(encoding="utf-8"))
    base_err = int(baseline.get("errors_count", 0))
    increased = cur_err > base_err

    status_path.write_text(
        json.dumps(
            {"baseline_errors": base_err, "current_errors": cur_err, "increased": increased, "rc": rc},
            ensure_ascii=False,
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )

    if increased and cfg.quality_gates.get("dotnet_build_block_on_increase", True):
        print(f"[typecheck] FAIL: errors increased {base_err} -> {cur_err}")
        print("[typecheck] command:", " ".join(cmd))
        print(out)
        return 1

    print(f"[typecheck] OK: errors {base_err} -> {cur_err} (warnings={cur_warn}, rc={rc})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(__import__("sys").argv[1:]))

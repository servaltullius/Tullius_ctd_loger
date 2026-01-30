#!/usr/bin/env python3
from __future__ import annotations

import json
import subprocess
import time
from pathlib import Path
from typing import Any

from context_db import is_excluded


def truncate(text: str, limit: int = 8000) -> tuple[str, bool]:
    if len(text) <= limit:
        return text, False
    return text[:limit] + "\n... (truncated)\n", True


def repo_has_any_glob(root: Path, globs: list[str], exclude_dirs: list[str]) -> bool:
    for g in globs:
        try:
            for p in root.glob(g):
                if not p.is_file():
                    continue
                try:
                    rel = p.relative_to(root)
                except ValueError:
                    continue
                if is_excluded(rel, exclude_dirs):
                    continue
                return True
        except Exception:
            # Best-effort: treat invalid glob as non-match.
            continue
    return False


def run_custom_checks(
    cfg,
    checks: list[dict[str, Any]],
    *,
    report_path: Path,
    default_timeout_sec: float | None = None,
) -> tuple[list[dict[str, Any]], list[tuple[str, int]]]:
    root = cfg.root
    results: list[dict[str, Any]] = []
    failures: list[tuple[str, int]] = []

    for raw in checks:
        name = str(raw.get("name") or "").strip() or "custom_check"
        enabled = raw.get("enabled", True)
        if enabled is not True:
            results.append({"name": name, "status": "skipped", "reason": "disabled"})
            continue

        cmd = raw.get("cmd")
        if not (isinstance(cmd, list) and cmd and all(isinstance(x, str) and x for x in cmd)):
            results.append({"name": name, "status": "skipped", "reason": "invalid cmd (expected list[str])"})
            continue

        cwd_raw = raw.get("cwd")
        cwd = (root / cwd_raw).resolve(strict=False) if isinstance(cwd_raw, str) and cwd_raw else root
        try:
            cwd.relative_to(root.resolve())
        except ValueError:
            results.append({"name": name, "status": "skipped", "reason": f"cwd outside repo: {cwd_raw!r}"})
            continue

        when_any_glob = raw.get("when_any_glob")
        if isinstance(when_any_glob, list) and when_any_glob:
            globs = [str(g) for g in when_any_glob if isinstance(g, str) and g.strip()]
            if globs and not repo_has_any_glob(root, globs, cfg.exclude_dirs):
                results.append({"name": name, "status": "skipped", "reason": "when_any_glob: no matches", "globs": globs})
                continue

        timeout_sec = raw.get("timeout_sec", default_timeout_sec)
        timeout: float | None = None
        if isinstance(timeout_sec, (int, float)) and timeout_sec > 0:
            timeout = float(timeout_sec)

        on_fail = str(raw.get("on_fail") or "warn").strip().lower()
        if on_fail not in {"warn", "fail"}:
            on_fail = "warn"

        start = time.time()
        try:
            p = subprocess.run(
                cmd,
                cwd=str(cwd),
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=timeout,
            )
            rc = int(p.returncode)
            out = p.stdout or ""
            timed_out = False
        except OSError as e:
            rc = 127
            out = f"{e.__class__.__name__}: {e}"
            timed_out = False
        except subprocess.TimeoutExpired as e:
            rc = 124
            out = (e.stdout or "") + "\n(timeout)\n"
            timed_out = True

        dur = time.time() - start
        out2, truncated_flag = truncate(out)

        status = "ok" if rc == 0 else "fail"
        results.append(
            {
                "name": name,
                "status": status,
                "cmd": cmd,
                "cwd": str(cwd.relative_to(root)),
                "rc": rc,
                "duration_sec": round(dur, 3),
                "timed_out": timed_out,
                "output": out2,
                "output_truncated": truncated_flag,
                "on_fail": on_fail,
            }
        )

        if rc != 0 and on_fail == "fail":
            failures.append((name, rc))

    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(results, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return results, failures

#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from pathlib import Path

from context_db import load_config


def _summarize_existing(cfg) -> dict:
    reports = cfg.root / ".vibe" / "reports"
    perf_log = reports / "performance.log"
    stats = {"top": []}
    if not perf_log.exists():
        return stats

    # Minimal, format-agnostic parser:
    # - Look for lines like: "name\tcount\tavg_ms\tmax_ms"
    # - Otherwise, leave empty (user can paste structured data later).
    rows: list[dict] = []
    for line in perf_log.read_text(encoding="utf-8", errors="ignore").splitlines():
        parts = [p.strip() for p in line.split("\t")]
        if len(parts) < 4:
            continue
        name, count_s, avg_s, max_s = parts[:4]
        try:
            rows.append(
                {
                    "name": name,
                    "count": int(count_s),
                    "avg_ms": float(avg_s),
                    "max_ms": float(max_s),
                }
            )
        except ValueError:
            continue

    rows.sort(key=lambda r: (-r["avg_ms"], -r["max_ms"]))
    stats["top"] = rows[:10]
    return stats


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Optional perf profiler helper (no source injection).")
    parser.add_argument("--summarize-only", action="store_true", help="Only summarize existing logs.")
    args = parser.parse_args(argv)

    cfg = load_config()
    reports = cfg.root / ".vibe" / "reports"
    reports.mkdir(parents=True, exist_ok=True)

    stats = _summarize_existing(cfg)
    out = reports / "performance_stats.json"
    out.write_text(json.dumps(stats, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    if stats.get("top"):
        print(f"[profile] summarized: {out} (top={len(stats['top'])})")
    else:
        print(f"[profile] no data; wrote empty stats: {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(__import__("sys").argv[1:]))


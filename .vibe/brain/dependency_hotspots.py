#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json

from context_db import connect, load_config


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Hotspot summaries (fan-in + large files).")
    parser.add_argument("--out", default=".vibe/reports/hotspots.json")
    args = parser.parse_args(argv)

    cfg = load_config()
    con = connect()
    try:
        # ProjectReference fan-in
        fan_in = con.execute(
            "SELECT to_file AS target, COUNT(*) AS n FROM deps WHERE kind='ProjectReference' GROUP BY to_file ORDER BY n DESC LIMIT 20"
        ).fetchall()
        fan_in = [{"target": r["target"], "count": int(r["n"])} for r in fan_in]

        # Largest files (LOC)
        largest = con.execute("SELECT path, loc FROM files ORDER BY loc DESC LIMIT 20").fetchall()
        largest = [{"path": r["path"], "loc": int(r["loc"])} for r in largest]

        # Most symbols per file
        sym = con.execute(
            "SELECT file AS path, COUNT(*) AS n FROM symbols GROUP BY file ORDER BY n DESC LIMIT 20"
        ).fetchall()
        sym = [{"path": r["path"], "symbols": int(r["n"])} for r in sym]
    finally:
        con.close()

    payload = {"fan_in": fan_in, "largest_files": largest, "symbol_hotspots": sym}
    out_path = cfg.root / args.out
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    print("Hotspots:")
    if fan_in:
        print("- Project fan-in (top 5):")
        for r in fan_in[:5]:
            print(f"  - {r['target']}: {r['count']}")
    if largest:
        print("- Largest files by LOC (top 5):")
        for r in largest[:5]:
            print(f"  - {r['path']}: {r['loc']}")
    if sym:
        print("- Most symbols per file (top 5):")
        for r in sym[:5]:
            print(f"  - {r['path']}: {r['symbols']}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(__import__("sys").argv[1:]))


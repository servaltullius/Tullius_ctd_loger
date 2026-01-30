#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import time
from pathlib import Path

from context_db import connect, load_config


def _read_json(path: Path) -> dict | list | None:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except OSError:
        return None
    except json.JSONDecodeError:
        return None


def _meta_get(con, key: str) -> str | None:
    row = con.execute("SELECT value FROM meta WHERE key = ?", (key,)).fetchone()
    return str(row["value"]) if row else None


def _meta_set(con, key: str, value: str) -> None:
    con.execute(
        "INSERT INTO meta(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value",
        (key, value),
    )


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Generate `.vibe/context/LATEST_CONTEXT.md`.")
    parser.add_argument("--full", action="store_true", help="Treat as a full refresh (reset recent window).")
    args = parser.parse_args(argv)

    cfg = load_config()
    con = connect()
    try:
        last_ts_s = _meta_get(con, "last_summary_ts")
        last_ts = float(last_ts_s) if last_ts_s and not args.full else 0.0

        recent = con.execute(
            "SELECT path, mtime, loc FROM files WHERE mtime > ? ORDER BY mtime DESC LIMIT ?",
            (last_ts, cfg.max_recent_files),
        ).fetchall()
        if not recent:
            recent = con.execute(
                "SELECT path, mtime, loc FROM files ORDER BY mtime DESC LIMIT ?",
                (cfg.max_recent_files,),
            ).fetchall()

        critical = con.execute(
            "SELECT name, file, line FROM symbols WHERE tags_json IS NOT NULL AND tags_json != '[]' ORDER BY file, line LIMIT 50"
        ).fetchall()
        vibe = cfg.root / ".vibe"
        typecheck_status = _read_json(vibe / "reports" / "typecheck_status.json") or {}
        hotspots = _read_json(vibe / "reports" / "hotspots.json") or {}
        boundaries = _read_json(vibe / "reports" / "boundaries.json") or {}
        coupling = _read_json(vibe / "reports" / "change_coupling.json") or {}
        complexity = _read_json(vibe / "reports" / "complexity.json") or []

        lines: list[str] = []
        lines.append("# LATEST_CONTEXT\n")
        lines.append("## [1] Recent changes (Top N)\n")
        for r in recent:
            lines.append(f"- {r['path']} (loc={r['loc']})")

        lines.append("\n## [2] Critical map\n")
        if critical:
            for r in critical[:30]:
                lines.append(f"- {r['file']}:{r['line']} {r['name']}")
        else:
            lines.append("- (none found; add `@critical` in doc/comments to tag key functions)")

        lines.append("\n## [3] Warnings\n")
        if typecheck_status:
            if typecheck_status.get("skipped"):
                reason = typecheck_status.get("reason") or "skipped"
                lines.append(f"- typecheck: skipped ({reason})")
            else:
                lines.append(
                    f"- typecheck: baseline_errors={typecheck_status.get('baseline_errors')} current_errors={typecheck_status.get('current_errors')} increased={typecheck_status.get('increased')}"
                )
        else:
            lines.append("- typecheck: (not run yet) — run `python3 scripts/vibe.py doctor --full`")

        if boundaries and not boundaries.get("skipped"):
            stats = boundaries.get("stats") if isinstance(boundaries, dict) else None
            v = stats.get("violations") if isinstance(stats, dict) else None
            if isinstance(v, int) and v > 0:
                md_path = boundaries.get("md_path") if isinstance(boundaries, dict) else None
                suffix = f" (see `{md_path.strip()}`)" if isinstance(md_path, str) and md_path.strip() else ""
                lines.append(f"- boundaries: violations={v}{suffix}")

        # Complexity top 10
        if isinstance(complexity, list) and complexity:
            lines.append("- complexity (top 5):")
            for r in complexity[:5]:
                lines.append(
                    f"  - {r.get('file')}:{r.get('line')} {r.get('name')} (lines={r.get('lines')}, nesting={r.get('nesting')}, params={r.get('params')})"
                )
        else:
            lines.append("- complexity: (no warnings)")

        lines.append("\n## [4] Hotspots\n")
        fan_in = hotspots.get("fan_in") if isinstance(hotspots, dict) else None
        largest = hotspots.get("largest_files") if isinstance(hotspots, dict) else None
        symbol_hotspots = hotspots.get("symbol_hotspots") if isinstance(hotspots, dict) else None
        if fan_in:
            lines.append("- project fan-in (top 5):")
            for r in fan_in[:5]:
                lines.append(f"  - {r['target']}: {r['count']}")
        if largest:
            lines.append("- largest files by LOC (top 5):")
            for r in largest[:5]:
                lines.append(f"  - {r['path']}: {r['loc']}")
        if symbol_hotspots:
            lines.append("- most symbols per file (top 5):")
            for r in symbol_hotspots[:5]:
                lines.append(f"  - {r['path']}: {r['symbols']}")
        if not fan_in and not largest and not symbol_hotspots:
            lines.append("- (run doctor to generate hotspots)")

        pairs = coupling.get("pairs") if isinstance(coupling, dict) else None
        if isinstance(pairs, list) and pairs:
            lines.append("- change coupling (top 3 pairs):")
            for r in pairs[:3]:
                a = r.get("a")
                b = r.get("b")
                n = r.get("count")
                j = r.get("jaccard")
                if a and b and n is not None:
                    lines.append(f"  - {a} <-> {b} (count={n}, jaccard={j})")

        clusters = coupling.get("clusters") if isinstance(coupling, dict) else None
        if isinstance(clusters, list) and clusters:
            c0 = clusters[0] if isinstance(clusters[0], dict) else None
            nodes = c0.get("nodes") if isinstance(c0, dict) else None
            if isinstance(nodes, list) and nodes:
                show = ", ".join([str(x) for x in nodes[:6]])
                suffix = " ..." if len(nodes) > 6 else ""
                lines.append(f"- change coupling cluster (top): {show}{suffix}")

        leaks = coupling.get("boundary_leaks") if isinstance(coupling, dict) else None
        if isinstance(leaks, list) and leaks:
            r0 = leaks[0] if isinstance(leaks[0], dict) else None
            a = r0.get("a") if isinstance(r0, dict) else None
            b = r0.get("b") if isinstance(r0, dict) else None
            n = r0.get("count") if isinstance(r0, dict) else None
            j = r0.get("jaccard") if isinstance(r0, dict) else None
            if a and b and n is not None:
                lines.append(f"- change coupling boundary leak (top): {a} <-> {b} (count={n}, jaccard={j})")

        md_path = coupling.get("decoupling_suggestions_md_path") if isinstance(coupling, dict) else None
        if isinstance(md_path, str) and md_path.strip():
            lines.append(f"- decoupling playbooks: see `{md_path.strip()}`")

        lines.append("\n## [5] Next actions\n")
        next_actions: list[str] = []
        if typecheck_status.get("increased"):
            next_actions.append("Fix new typecheck errors (baseline gate).")
        if boundaries and not boundaries.get("skipped"):
            stats = boundaries.get("stats") if isinstance(boundaries, dict) else None
            v = stats.get("violations") if isinstance(stats, dict) else None
            if isinstance(v, int) and v > 0:
                next_actions.append("Fix boundary violations (run: `python3 scripts/vibe.py boundaries`).")
        if isinstance(complexity, list) and complexity:
            next_actions.append("Consider splitting the top complexity hotspot method(s).")
        if not next_actions:
            next_actions.append("Generate a compact context pack: `python3 scripts/vibe.py pack --scope=changed --out .vibe/context/PACK.md`")
            next_actions.append("Before touching shared/core files: `python3 scripts/vibe.py impact <file>`")
            next_actions.append("Run your repo's normal tests/lint/typecheck.")
        for a in next_actions[:3]:
            lines.append(f"- {a}")

        cfg.latest_file.parent.mkdir(parents=True, exist_ok=True)
        cfg.latest_file.write_text("\n".join(lines).rstrip() + "\n", encoding="utf-8")

        # Update timestamp only after successful write.
        _meta_set(con, "last_summary_ts", str(time.time()))
        con.commit()

        print(f"[summarizer] wrote: {cfg.latest_file}")
        return 0
    finally:
        con.close()


if __name__ == "__main__":
    raise SystemExit(main(__import__("sys").argv[1:]))

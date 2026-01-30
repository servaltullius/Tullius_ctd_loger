#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

from context_db import connect, load_config, normalize_rel


def _risk_score(impact_files: int, is_critical: bool) -> int:
    score = impact_files * 2
    if is_critical:
        score *= 5
    return score


def _fts_query(name: str) -> str:
    # Avoid FTS5 syntax errors on dotted namespaces (e.g. A.B.C).
    q = re.sub(r"[^A-Za-z0-9_]+", " ", name).strip()
    return q


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Impact analysis (heuristic, FTS-based).")
    parser.add_argument("path", help="Target file path (repo-relative).")
    parser.add_argument("--limit", type=int, default=40)
    args = parser.parse_args(argv)

    cfg = load_config()
    target_path = Path(args.path)
    root_res = cfg.root.resolve()
    if target_path.is_absolute():
        target_path = target_path.resolve(strict=False)
    else:
        target_path = (cfg.root / target_path).resolve(strict=False)
    try:
        rel = target_path.relative_to(root_res)
    except ValueError:
        print(f"[impact] refuse: outside repo root: {target_path}", file=sys.stderr)
        return 2
    rel_s = normalize_rel(rel)

    con = connect()
    try:
        syms = con.execute(
            "SELECT name, exported_int, tags_json FROM symbols WHERE file = ? ORDER BY exported_int DESC, name LIMIT 50",
            (rel_s,),
        ).fetchall()
        names = [r["name"] for r in syms if r["name"]]
        is_critical = any(r["tags_json"] and r["tags_json"] != "[]" for r in syms)

        impacted: dict[str, int] = {}
        for n in names[:20]:
            q = _fts_query(str(n))
            if not q:
                continue
            rows = con.execute(
                "SELECT path FROM fts_files WHERE fts_files MATCH ? LIMIT 200",
                (q,),
            ).fetchall()
            for r in rows:
                p = str(r["path"])
                if p == rel_s:
                    continue
                impacted[p] = impacted.get(p, 0) + 1

        ranked = sorted(impacted.items(), key=lambda kv: (-kv[1], kv[0]))
        top = ranked[: args.limit]
    finally:
        con.close()

    score = _risk_score(len(impacted), is_critical)
    print(f"Impact for: {rel_s}")
    print(f"- impacted_files: {len(impacted)}")
    print(f"- critical: {is_critical}")
    print(f"- risk_score: {score}")
    if score > 50:
        print("- NOTE: consider a checkpoint before large changes.")

    if top:
        print("\nTop impacted files:")
        for p, n in top:
            print(f"- {p} (hits={n})")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(__import__("sys").argv[1:]))

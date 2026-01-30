#!/usr/bin/env python3
from __future__ import annotations

import argparse
import sqlite3
import sys
import textwrap
from pathlib import Path

from context_db import connect, load_config


def _snippet(text: str, width: int = 120) -> str:
    s = " ".join(text.split())
    if len(s) <= width:
        return s
    return s[: width - 1] + "…"


def _search_fts(con: sqlite3.Connection, query: str, limit: int) -> None:
    rows = con.execute(
        "SELECT name,file,doc,signature FROM fts_symbols WHERE fts_symbols MATCH ? LIMIT ?",
        (query, limit),
    ).fetchall()

    rows_files = con.execute(
        "SELECT path, snippet(fts_files, 1, '[', ']', '…', 12) AS snip "
        "FROM fts_files WHERE fts_files MATCH ? LIMIT ?",
        (query, limit),
    ).fetchall()

    print(f"# Symbols (top {limit})")
    for r in rows:
        sig = r["signature"] or r["name"]
        print(f"- {r['file']}:{sig}")
        if r["doc"]:
            print(f"  {_snippet(r['doc'])}")

    print(f"\n# Files (top {limit})")
    for r in rows_files:
        print(f"- {r['path']}")
        if r["snip"]:
            print(textwrap.indent(str(r["snip"]), "  "))


def _search_literal(con: sqlite3.Connection, cfg_root: Path, query: str, limit: int) -> None:
    like = f"%{query}%"
    print(f"# Symbols (LIKE, top {limit})")
    rows = con.execute(
        "SELECT name,file,line,doc,signature FROM symbols "
        "WHERE name LIKE ? OR doc LIKE ? "
        "ORDER BY exported_int DESC, name "
        "LIMIT ?",
        (like, like, limit),
    ).fetchall()
    for r in rows:
        sig = r["signature"] or r["name"]
        print(f"- {r['file']}:{r['line']}:{sig}")
        if r["doc"]:
            print(f"  {_snippet(r['doc'])}")

    print(f"\n# Files (literal, top {limit})")
    paths = con.execute("SELECT path FROM files ORDER BY mtime DESC").fetchall()
    matches = 0
    for r in paths:
        rel = Path(str(r["path"]))
        full = cfg_root / rel
        try:
            with full.open("r", encoding="utf-8", errors="ignore") as f:
                for line_no, line in enumerate(f, 1):
                    if query not in line:
                        continue
                    print(f"- {rel.as_posix()}:{line_no}")
                    print(f"  {_snippet(line.strip())}")
                    matches += 1
                    if matches >= limit:
                        return
        except OSError:
            continue
    if matches == 0:
        print("(no matches)")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Search vibe-kit context DB (SQLite FTS).")
    parser.add_argument("query", help="FTS query (e.g. PlaceholderMasker OR TokenValidation)")
    parser.add_argument("--limit", type=int, default=10)
    args = parser.parse_args(argv)

    cfg = load_config()
    con = connect()
    try:
        try:
            _search_fts(con, args.query, args.limit)
        except sqlite3.OperationalError as e:
            print(f"[search] fts error: {e}", file=sys.stderr)
            _search_literal(con, cfg.root, args.query, args.limit)
        return 0
    finally:
        con.close()


if __name__ == "__main__":
    raise SystemExit(main(__import__("sys").argv[1:]))

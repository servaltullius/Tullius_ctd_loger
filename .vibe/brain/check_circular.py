#!/usr/bin/env python3
from __future__ import annotations

import argparse
from collections import defaultdict

from context_db import connect, load_config


def _find_cycle(graph: dict[str, list[str]]) -> list[str] | None:
    visited: set[str] = set()
    stack: set[str] = set()
    parent: dict[str, str | None] = {}

    def dfs(node: str) -> list[str] | None:
        visited.add(node)
        stack.add(node)
        for nxt in graph.get(node, []):
            if nxt not in visited:
                parent[nxt] = node
                res = dfs(nxt)
                if res:
                    return res
            elif nxt in stack:
                # reconstruct cycle: node -> ... -> nxt
                cycle = [nxt]
                cur = node
                while cur != nxt and cur is not None:
                    cycle.append(cur)
                    cur = parent.get(cur)
                cycle.append(nxt)
                cycle.reverse()
                return cycle
        stack.remove(node)
        return None

    for n in list(graph.keys()):
        if n not in visited:
            parent[n] = None
            res = dfs(n)
            if res:
                return res
    return None


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Detect cycles in the csproj ProjectReference graph.")
    parser.add_argument("--json", action="store_true", help="Emit machine-readable JSON to stdout.")
    args = parser.parse_args(argv)

    cfg = load_config()
    con = connect()
    try:
        rows = con.execute("SELECT from_file,to_file FROM deps WHERE kind = 'ProjectReference'").fetchall()
    finally:
        con.close()

    graph: dict[str, list[str]] = defaultdict(list)
    for r in rows:
        graph[str(r["from_file"])].append(str(r["to_file"]))

    cycle = _find_cycle(graph)
    if not cycle:
        if args.json:
            print('{"ok":true,"cycle":null}')
        else:
            print("No cycles detected.")
        return 0

    if args.json:
        import json

        print(json.dumps({"ok": False, "cycle": cycle}, ensure_ascii=False))
    else:
        print("Cycle detected:")
        print("  " + " -> ".join(cycle))
        print("Suggested cut candidates:")
        print("  - Remove/replace one ProjectReference edge in the cycle.")
        print("  - Split shared code into a third project referenced by both sides.")

    return 1 if cfg.quality_gates.get("cycle_block", True) else 0


if __name__ == "__main__":
    raise SystemExit(main(__import__("sys").argv[1:]))


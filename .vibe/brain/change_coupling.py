#!/usr/bin/env python3
from __future__ import annotations

import argparse
import fnmatch
import json
import shutil
import subprocess
import sys
import time
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any, Iterable

from context_db import is_excluded, load_config


COMMIT_MARKER = "__VIBE_COMMIT__"


@dataclass(frozen=True)
class Edge:
    a: str
    b: str
    count: int
    jaccard: float


@dataclass(frozen=True)
class CommitMeta:
    files: list[str]
    churn: int
    numstat: list[tuple[str, int]]


def _matches_include(rel_posix: str, include_globs: list[str]) -> bool:
    if not include_globs:
        return True
    for g in include_globs:
        if fnmatch.fnmatch(rel_posix, g):
            return True
        if g.startswith("**/") and fnmatch.fnmatch(rel_posix, g[3:]):
            return True
    return False


def parse_git_log_name_only(text: str) -> list[list[str]]:
    commits: list[list[str]] = []
    cur: list[str] = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        if line.startswith(COMMIT_MARKER):
            if cur:
                commits.append(cur)
            cur = []
            continue
        cur.append(line)
    if cur:
        commits.append(cur)
    return commits


def parse_git_log_name_status(
    text: str,
    *,
    detect_renames: bool,
    include_numstat: bool,
) -> list[CommitMeta]:
    commits: list[CommitMeta] = []
    cur: list[str] = []
    churn = 0
    cur_numstat: list[tuple[str, int]] = []
    rename_map: dict[str, str] = {}

    def normalize(p: str) -> str:
        s = str(p).strip().replace("\\", "/")
        if s.startswith("./"):
            s = s[2:]
        return s

    def canonical(p: str) -> str:
        cur_p = normalize(p)
        seen: set[str] = set()
        while cur_p in rename_map and cur_p not in seen:
            seen.add(cur_p)
            cur_p = rename_map[cur_p]
        return cur_p

    def flush() -> None:
        nonlocal cur, churn, cur_numstat
        if not cur:
            churn = 0
            cur_numstat = []
            return
        commits.append(CommitMeta(files=cur, churn=int(churn), numstat=cur_numstat))
        cur = []
        churn = 0
        cur_numstat = []

    for raw in text.splitlines():
        line = raw.rstrip("\n")
        if not line.strip():
            continue
        if line.startswith(COMMIT_MARKER):
            flush()
            continue

        if include_numstat:
            parts = line.split("\t", 2)
            if len(parts) >= 3:
                a, d, path = parts[0], parts[1], parts[2]
                if (a.isdigit() or a == "-") and (d.isdigit() or d == "-"):
                    aa = int(a) if a.isdigit() else 0
                    dd = int(d) if d.isdigit() else 0
                    delta = aa + dd
                    churn += delta
                    cur_numstat.append((normalize(path), int(delta)))
                    continue

        parts = line.split("\t")
        if not parts:
            continue

        status = parts[0].strip()
        code = status[:1]

        # name-status entries
        if code in {"R", "C"} and len(parts) >= 3:
            old = canonical(parts[1])
            new = canonical(parts[2])
            if detect_renames and code == "R" and old and new:
                rename_map[old] = new
            if new:
                cur.append(new)
            continue

        if len(parts) >= 2 and code and code.isalpha():
            p = canonical(parts[1])
            if p:
                cur.append(p)
            continue

        # Fallback: treat as a path.
        p = canonical(line)
        if p:
            cur.append(p)

    flush()
    return commits


def group_path(rel_posix: str, *, group_by: str, dir_depth: int) -> str:
    p = PurePosixPath(rel_posix)
    if group_by == "file":
        return rel_posix
    # group_by == "dir"
    parent = p.parent
    if str(parent) in {".", ""}:
        return "."
    parts = parent.parts
    if dir_depth <= 0:
        return parts[0]
    return "/".join(parts[:dir_depth])


def filter_paths(
    cfg,
    paths: Iterable[str],
    *,
    group_by: str,
    dir_depth: int,
) -> list[str]:
    out: set[str] = set()
    for raw in paths:
        s = str(raw).strip()
        if not s:
            continue
        s = s.replace("\\", "/")
        if s.startswith("./"):
            s = s[2:]
        if not s or s.startswith("/"):
            continue

        rel_p = Path(s)
        if is_excluded(rel_p, cfg.exclude_dirs):
            continue
        if not _matches_include(rel_p.as_posix(), cfg.include_globs):
            continue

        out.add(group_path(rel_p.as_posix(), group_by=group_by, dir_depth=dir_depth))
    return sorted(out)


def _path_in_scope(cfg, raw: str) -> bool:
    s = str(raw).strip()
    if not s:
        return False
    s = s.replace("\\", "/")
    if s.startswith("./"):
        s = s[2:]
    if not s or s.startswith("/"):
        return False
    rel_p = Path(s)
    if is_excluded(rel_p, cfg.exclude_dirs):
        return False
    if not _matches_include(rel_p.as_posix(), cfg.include_globs):
        return False
    return True


def compute_change_coupling(
    commits: Iterable[Iterable[str]],
    *,
    max_files_per_commit: int,
) -> tuple[dict[tuple[str, str], int], dict[str, int], dict[str, int], int]:
    pair_counts: dict[tuple[str, str], int] = defaultdict(int)
    file_commit_counts: dict[str, int] = defaultdict(int)
    skipped_large_commits = 0

    for files in commits:
        uniq = sorted(set(files))
        if len(uniq) < 2:
            continue
        if max_files_per_commit > 0 and len(uniq) > max_files_per_commit:
            skipped_large_commits += 1
            continue

        for f in uniq:
            file_commit_counts[f] += 1
        for i in range(len(uniq)):
            a = uniq[i]
            for j in range(i + 1, len(uniq)):
                b = uniq[j]
                pair_counts[(a, b)] += 1

    sum_couplings: dict[str, int] = defaultdict(int)
    for (a, b), n in pair_counts.items():
        sum_couplings[a] += n
        sum_couplings[b] += n

    return dict(pair_counts), dict(file_commit_counts), dict(sum_couplings), skipped_large_commits


def compute_edges(
    *,
    pair_counts: dict[tuple[str, str], int],
    file_commit_counts: dict[str, int],
    min_pair_count: int,
) -> list[Edge]:
    edges: list[Edge] = []
    for (a, b), n in pair_counts.items():
        if min_pair_count > 0 and n < min_pair_count:
            continue
        denom = file_commit_counts.get(a, 0) + file_commit_counts.get(b, 0) - n
        jaccard = (n / denom) if denom > 0 else 0.0
        edges.append(Edge(a=a, b=b, count=int(n), jaccard=float(jaccard)))
    edges.sort(key=lambda e: (-e.count, -e.jaccard, e.a, e.b))
    return edges


def _edge_to_json(e: Edge) -> dict[str, Any]:
    return {"a": e.a, "b": e.b, "count": int(e.count), "jaccard": round(float(e.jaccard), 4)}


def build_report(
    *,
    edges: list[Edge],
    file_commit_counts: dict[str, int],
    sum_couplings: dict[str, int],
    max_pairs: int,
) -> dict[str, Any]:
    pairs = [_edge_to_json(e) for e in (edges[:max_pairs] if max_pairs > 0 else edges)]

    files: list[dict[str, Any]] = []
    for p, commits in file_commit_counts.items():
        files.append({"path": p, "commits": int(commits), "sum_couplings": int(sum_couplings.get(p, 0))})
    files.sort(key=lambda r: (-int(r["sum_couplings"]), -int(r["commits"]), str(r["path"])))

    return {"pairs": pairs, "files": files[:200]}


def compute_clusters(
    strong_edges: list[Edge],
    *,
    min_cluster_size: int,
    max_clusters: int,
) -> tuple[list[dict[str, Any]], dict[str, int]]:
    adj: dict[str, list[int]] = defaultdict(list)
    for i, e in enumerate(strong_edges):
        adj[e.a].append(i)
        adj[e.b].append(i)

    visited: set[str] = set()
    clusters_all: list[dict[str, Any]] = []
    node_to_cluster: dict[str, int] = {}

    for start in sorted(adj.keys()):
        if start in visited:
            continue
        stack = [start]
        visited.add(start)
        nodes: set[str] = set()
        edge_ids: set[int] = set()
        while stack:
            cur = stack.pop()
            nodes.add(cur)
            for eid in adj.get(cur, []):
                edge_ids.add(eid)
                e = strong_edges[eid]
                other = e.b if e.a == cur else e.a
                if other not in visited:
                    visited.add(other)
                    stack.append(other)

        if len(nodes) < max(2, int(min_cluster_size)):
            continue

        nodes_sorted = sorted(nodes)
        internal_edges = [strong_edges[eid] for eid in sorted(edge_ids)]
        internal_edge_count = len(internal_edges)
        internal_count_sum = sum(e.count for e in internal_edges)
        internal_jaccard_avg = (sum(e.jaccard for e in internal_edges) / internal_edge_count) if internal_edge_count else 0.0
        internal_edges_sorted = sorted(internal_edges, key=lambda e: (-e.count, -e.jaccard, e.a, e.b))
        top_internal = [_edge_to_json(e) for e in internal_edges_sorted[:5]]

        clusters_all.append(
            {
                "nodes": nodes_sorted,
                "node_count": len(nodes_sorted),
                "internal_edges": internal_edge_count,
                "internal_count_sum": int(internal_count_sum),
                "internal_jaccard_avg": round(float(internal_jaccard_avg), 4),
                "top_internal_pairs": top_internal,
                "suggestion": "Candidate component boundary (modules that frequently co-change).",
            }
        )

    clusters_all.sort(
        key=lambda c: (
            -int(c["internal_count_sum"]),
            -int(c["internal_edges"]),
            -int(c["node_count"]),
            -float(c["internal_jaccard_avg"]),
            ",".join(c["nodes"]),
        )
    )

    clusters = clusters_all[: int(max_clusters)] if max_clusters > 0 else clusters_all

    for idx, c in enumerate(clusters, 1):
        c["id"] = idx
        for n in c["nodes"]:
            node_to_cluster[n] = idx

    return clusters, node_to_cluster


def _classify_node(node: str) -> list[str]:
    s = node.lower()
    tags: list[str] = []

    shared_words = ("shared", "common", "utils", "util", "core", "lib", "libs", "base", "foundation")
    infra_words = ("infra", "infrastructure", "adapter", "adapters", "gateway", "client", "clients", "integration", "external")
    domain_words = ("domain", "model", "models", "entity", "entities", "aggregate", "aggregates")
    ui_words = ("ui", "web", "frontend", "mobile", "app")
    data_words = ("db", "data", "repo", "repository", "storage", "persistence")

    if any(w in s for w in shared_words):
        tags.append("shared")
    if any(w in s for w in infra_words):
        tags.append("infra")
    if any(w in s for w in domain_words):
        tags.append("domain")
    if any(w in s for w in ui_words):
        tags.append("ui")
    if any(w in s for w in data_words):
        tags.append("data")

    return tags


def _boundary_leak_playbooks(a: str, b: str) -> list[dict[str, Any]]:
    tags_a = set(_classify_node(a))
    tags_b = set(_classify_node(b))

    shared_side = None
    if "shared" in tags_a and "shared" not in tags_b:
        shared_side = a
    elif "shared" in tags_b and "shared" not in tags_a:
        shared_side = b

    playbooks: list[dict[str, Any]] = []

    playbooks.append(
        {
            "title": "Extract shared contract module (공유 계약/타입 모듈 추출)",
            "when_to_use": "Two areas co-change because of shared DTOs/types/config keys or shared validation rules.",
            "steps": [
                "List the top shared items that force co-change (DTO/type/config key/enum/constant).",
                "Create a small `<contracts>/` (or `<shared>/contracts/`) area that contains ONLY stable contracts (no business logic).",
                f"Move the shared items there and update {a} and {b} to depend on the contracts module instead of each other.",
                "Add lightweight compatibility tests at the boundary (serialization/validation) if applicable.",
            ],
            "acceptance": [
                "Edits to internal logic in one side no longer require touching the other side.",
                "Only contract changes require coordinated updates (and those are explicit).",
            ],
            "safety_checks": [
                "Run your normal test/typecheck/lint suite.",
                f"Run: `python3 scripts/vibe.py impact <moved-file>` before/after moves to control blast radius.",
            ],
        }
    )

    playbooks.append(
        {
            "title": "Introduce façade API (파사드로 경계 고정)",
            "when_to_use": "One side is reaching into the other side’s internals; many call sites depend on implementation details.",
            "steps": [
                "Identify the minimal set of operations that the consumer truly needs (the public boundary).",
                "Create a single entry surface (facade/service) that exposes those operations with stable inputs/outputs.",
                "Make the consumer depend only on that facade; forbid internal imports/usages across the boundary.",
                "Refactor internals behind the facade freely (keep the facade stable).",
            ],
            "acceptance": [
                "Cross-boundary references go through one narrow surface (facade).",
                "Internal refactors stop causing widespread ripple edits in the other side.",
            ],
            "safety_checks": [
                "Add a small boundary test suite for the facade (happy path + one failure).",
                "Prefer multiple small PRs (facade first, then move call sites, then internal cleanup).",
            ],
        }
    )

    acl_when = "Upstream/downstream models diverge; you want to prevent one model from leaking into the other."
    if ("infra" in tags_a and "domain" in tags_b) or ("infra" in tags_b and "domain" in tags_a):
        acl_when = "Infra/integration code and domain code co-change together; translate at the boundary (ports/adapters or ACL)."

    playbooks.append(
        {
            "title": "Anti-corruption layer / Adapter (ACL/어댑터 번역 레이어)",
            "when_to_use": acl_when,
            "steps": [
                "Pick a ‘consumer-owned’ boundary model (DTOs/interfaces) that will not leak upstream terms.",
                "Implement an adapter/translator layer that is the ONLY place allowed to talk across the boundary.",
                "Translate data and errors at the adapter boundary (map upstream concepts -> local concepts).",
                "Gradually move cross-boundary usages behind the adapter until the rest of the code is insulated.",
            ],
            "acceptance": [
                "Only adapter code changes when the other side changes its schema/API.",
                "Core domain code stays stable and uses its own language/model.",
            ],
            "safety_checks": [
                "Add mapping tests for the adapter (golden fixtures if possible).",
                "If behavior is risky, do a strangler-style migration: keep old path + new adapter path behind a flag until stable.",
            ],
        }
    )

    # Add a small hint if we think one side is shared.
    if shared_side:
        playbooks[0]["note"] = f"Hint: `{shared_side}` looks like a shared area; keep it thin/stable and push variability outward."

    return playbooks


def compute_boundary_leaks(
    weak_edges: list[Edge],
    *,
    node_to_cluster: dict[str, int],
    max_boundary_leaks: int,
) -> list[dict[str, Any]]:
    leaks: list[dict[str, Any]] = []
    for e in weak_edges:
        ca = node_to_cluster.get(e.a)
        cb = node_to_cluster.get(e.b)
        if ca is None or cb is None or ca == cb:
            continue
        leaks.append(
            {
                **_edge_to_json(e),
                "cluster_a": int(ca),
                "cluster_b": int(cb),
                "suggestion": "Possible boundary leak between components; consider making the dependency explicit or extracting shared abstractions.",
                "playbooks": _boundary_leak_playbooks(e.a, e.b),
            }
        )

    leaks.sort(key=lambda r: (-int(r["count"]), -float(r["jaccard"]), str(r["a"]), str(r["b"])))
    if max_boundary_leaks > 0:
        leaks = leaks[: int(max_boundary_leaks)]
    return leaks


def compute_hubs(
    weak_edges: list[Edge],
    *,
    file_commit_counts: dict[str, int],
    sum_couplings: dict[str, int],
    node_to_cluster: dict[str, int],
    max_hubs: int,
) -> list[dict[str, Any]]:
    clustered_nodes = set(node_to_cluster.keys())
    weak_adj: dict[str, list[Edge]] = defaultdict(list)
    for e in weak_edges:
        weak_adj[e.a].append(e)
        weak_adj[e.b].append(e)

    hubs: list[dict[str, Any]] = []
    for node in sorted(clustered_nodes):
        own_cluster = node_to_cluster.get(node)
        connected_clusters: set[int] = set()
        cross_sum = 0
        for e in weak_adj.get(node, []):
            other = e.b if e.a == node else e.a
            other_cluster = node_to_cluster.get(other)
            if other_cluster is None or other_cluster == own_cluster:
                continue
            connected_clusters.add(int(other_cluster))
            cross_sum += int(e.count)

        if not connected_clusters:
            continue

        hubs.append(
            {
                "node": node,
                "cluster": int(own_cluster) if own_cluster is not None else None,
                "commits": int(file_commit_counts.get(node, 0)),
                "sum_couplings": int(sum_couplings.get(node, 0)),
                "connected_clusters_count": len(connected_clusters),
                "connected_clusters": sorted(connected_clusters),
                "cross_edge_count_sum": int(cross_sum),
                "suggestion": "High cross-component coupling; consider splitting responsibilities or introducing a façade/port to reduce ripple effects.",
            }
        )

    hubs.sort(
        key=lambda h: (
            -int(h["connected_clusters_count"]),
            -int(h["cross_edge_count_sum"]),
            -int(h["sum_couplings"]),
            -int(h["commits"]),
            str(h["node"]),
        )
    )
    if max_hubs > 0:
        hubs = hubs[: int(max_hubs)]
    return hubs


def _run_git_log(
    root: Path,
    *,
    max_commits: int,
    since: str | None,
    name_status: bool,
    numstat: bool,
    detect_renames: bool,
) -> tuple[int, str, str]:
    if shutil.which("git") is None:
        return 127, "", "git not found"
    cmd = [
        "git",
        "log",
        f"--pretty=format:{COMMIT_MARKER}%H",
        "--no-merges",
        f"--max-count={max_commits}",
    ]
    if name_status:
        cmd.append("--name-status")
        if detect_renames:
            cmd.append("--find-renames")
    else:
        cmd.append("--name-only")
    if numstat:
        cmd.append("--numstat")
    if since:
        cmd.append(f"--since={since}")
    p = subprocess.run(cmd, cwd=str(root), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    return int(p.returncode), p.stdout or "", p.stderr or ""


def _write_json(path: Path, payload: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def _write_text(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def render_decoupling_suggestions_md(payload: dict[str, Any]) -> str:
    ts = payload.get("timestamp")
    ts_s = ""
    if isinstance(ts, (int, float)):
        ts_s = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(float(ts)))

    lines: list[str] = []
    lines.append("# Decoupling suggestions (from change coupling)\n")
    if ts_s:
        lines.append(f"- Generated: `{ts_s}`")
    params = payload.get("params")
    if isinstance(params, dict):
        group_by = params.get("group_by")
        dir_depth = params.get("dir_depth")
        min_pair = params.get("min_pair_count")
        min_j = params.get("min_jaccard")
        lines.append(f"- Params: group_by={group_by!r} dir_depth={dir_depth!r} min_pair_count={min_pair!r} min_jaccard={min_j!r}")
    lines.append("")

    lines.append("## How to use\n")
    lines.append("- Pick 1 boundary leak, choose 1 playbook, and keep the first PR small.")
    lines.append("- Use `python3 scripts/vibe.py impact <path>` before moving shared/core files.")
    lines.append("- After a few commits, re-run `python3 scripts/vibe.py coupling` to see if the coupling decreases.")
    lines.append("")

    clusters = payload.get("clusters")
    if isinstance(clusters, list) and clusters:
        lines.append("## Clusters (module candidates)\n")
        for c in clusters[:5]:
            if not isinstance(c, dict):
                continue
            cid = c.get("id")
            nodes = c.get("nodes")
            if not isinstance(nodes, list) or not nodes:
                continue
            nodes_s = ", ".join(str(x) for x in nodes[:10]) + (" ..." if len(nodes) > 10 else "")
            lines.append(f"### Cluster #{cid}\n")
            lines.append(f"- Nodes: {nodes_s}")
            lines.append(f"- Internal edges: {c.get('internal_edges')}, internal_count_sum: {c.get('internal_count_sum')}, avg_jaccard: {c.get('internal_jaccard_avg')}")
            tip = c.get("suggestion")
            if tip:
                lines.append(f"- Tip: {tip}")
            lines.append("")

    leaks = payload.get("boundary_leaks")
    if isinstance(leaks, list) and leaks:
        lines.append("## Boundary leaks (cross-component coupling)\n")
        for i, r in enumerate(leaks[:10], 1):
            if not isinstance(r, dict):
                continue
            a = r.get("a")
            b = r.get("b")
            if not a or not b:
                continue
            lines.append(f"### Leak {i}: `{a}` <-> `{b}`\n")
            lines.append(f"- count: {r.get('count')}  /  jaccard: {r.get('jaccard')}")
            lines.append(f"- clusters: {r.get('cluster_a')} ↔ {r.get('cluster_b')}")
            if r.get("suggestion"):
                lines.append(f"- summary: {r.get('suggestion')}")
            if r.get("note"):
                lines.append(f"- note: {r.get('note')}")
            lines.append("")

            playbooks = r.get("playbooks")
            if isinstance(playbooks, list) and playbooks:
                lines.append("#### Refactor playbooks (choose one)\n")
                for pb in playbooks[:3]:
                    if not isinstance(pb, dict):
                        continue
                    title = pb.get("title") or "Playbook"
                    lines.append(f"##### {title}\n")
                    when = pb.get("when_to_use")
                    if when:
                        lines.append(f"- When: {when}")
                    note = pb.get("note")
                    if note:
                        lines.append(f"- Note: {note}")
                    steps = pb.get("steps")
                    if isinstance(steps, list) and steps:
                        lines.append("- Steps:")
                        for s in steps:
                            lines.append(f"  - {s}")
                    acceptance = pb.get("acceptance")
                    if isinstance(acceptance, list) and acceptance:
                        lines.append("- Acceptance:")
                        for s in acceptance:
                            lines.append(f"  - {s}")
                    checks = pb.get("safety_checks")
                    if isinstance(checks, list) and checks:
                        lines.append("- Safety checks:")
                        for s in checks:
                            lines.append(f"  - {s}")
                    lines.append("")

            lines.append("---\n")

    hubs = payload.get("hubs")
    if isinstance(hubs, list) and hubs:
        lines.append("## Hubs (cross-cluster change drivers)\n")
        for r in hubs[:10]:
            if not isinstance(r, dict):
                continue
            node = r.get("node")
            if not node:
                continue
            lines.append(f"- `{node}`: clusters={r.get('connected_clusters_count')}, cross_sum={r.get('cross_edge_count_sum')}, sum_couplings={r.get('sum_couplings')}")
        lines.append("")

    return "\n".join(lines).rstrip() + "\n"


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Change coupling (co-change) report from git history.")
    ap.add_argument("--max-commits", type=int, default=200)
    ap.add_argument("--min-pair-count", type=int, default=3)
    ap.add_argument("--min-jaccard", type=float, default=0.2, help="Jaccard threshold for building strong clusters.")
    ap.add_argument("--max-files-per-commit", type=int, default=80)
    ap.add_argument(
        "--max-churn-per-commit",
        type=int,
        default=0,
        help="Skip commits with >N total added+deleted lines (from `git log --numstat`) to reduce noisy formatting commits. 0 disables.",
    )
    ap.add_argument("--top", type=int, default=20, help="How many top pairs/files to print.")
    ap.add_argument("--since", help="Git --since filter (e.g. '6 months ago').")
    ap.add_argument(
        "--detect-renames",
        action="store_true",
        help="Best-effort: treat renames/moves as the same node when mining history (requires name-status parsing).",
    )
    ap.add_argument("--out", default=".vibe/reports/change_coupling.json")
    ap.add_argument("--group-by", choices=["file", "dir"], default="file")
    ap.add_argument("--dir-depth", type=int, default=2)
    ap.add_argument("--min-cluster-size", type=int, default=2)
    ap.add_argument("--max-clusters", type=int, default=10)
    ap.add_argument("--max-boundary-leaks", type=int, default=20)
    ap.add_argument("--max-hubs", type=int, default=20)
    ap.add_argument("--best-effort", action="store_true", help="Never fail the process (exit 0).")
    args = ap.parse_args(argv)

    cfg = load_config()
    root = cfg.root
    out_path = root / str(args.out)

    if not (root / ".git").exists():
        payload = {
            "ok": True,
            "skipped": True,
            "reason": "no .git directory found",
            "timestamp": time.time(),
        }
        _write_json(out_path, payload)
        print("[coupling] SKIP: no .git directory found")
        return 0

    start = time.time()
    max_churn = max(0, int(args.max_churn_per_commit))
    want_numstat = max_churn > 0
    want_name_status = bool(args.detect_renames) or want_numstat

    rc, out, err = _run_git_log(
        root,
        max_commits=int(args.max_commits),
        since=args.since,
        name_status=want_name_status,
        numstat=want_numstat,
        detect_renames=bool(args.detect_renames),
    )
    if rc != 0:
        payload = {
            "ok": False,
            "error": err.strip() or f"git log failed (rc={rc})",
            "rc": rc,
            "timestamp": time.time(),
            "out_path": str(args.out),
        }
        _write_json(out_path, payload)
        print(f"[coupling] ERROR: git log failed (rc={rc})", file=sys.stderr)
        if err.strip():
            print(err.strip(), file=sys.stderr)
        return 0 if args.best_effort else 1

    commits_meta: list[CommitMeta]
    if want_name_status:
        commits_meta = parse_git_log_name_status(out, detect_renames=bool(args.detect_renames), include_numstat=want_numstat)
    else:
        commits_raw = parse_git_log_name_only(out)
        commits_meta = [CommitMeta(files=files, churn=0, numstat=[]) for files in commits_raw]

    commits_filtered: list[list[str]] = []
    skipped_high_churn = 0
    for cm in commits_meta:
        if want_numstat and cm.numstat:
            churn_in_scope = sum(int(delta) for p, delta in cm.numstat if _path_in_scope(cfg, p))
            if churn_in_scope > max_churn:
                skipped_high_churn += 1
                continue

        filtered = filter_paths(cfg, cm.files, group_by=args.group_by, dir_depth=int(args.dir_depth))
        if filtered:
            commits_filtered.append(filtered)

    pair_counts, file_commit_counts, sum_couplings, skipped_large = compute_change_coupling(
        commits_filtered,
        max_files_per_commit=int(args.max_files_per_commit),
    )
    edges = compute_edges(
        pair_counts=pair_counts,
        file_commit_counts=file_commit_counts,
        min_pair_count=int(args.min_pair_count),
    )

    min_jaccard = float(args.min_jaccard)
    strong_edges = [e for e in edges if e.jaccard >= min_jaccard]
    weak_edges = [e for e in edges if e.jaccard < min_jaccard]

    clusters, node_to_cluster = compute_clusters(
        strong_edges,
        min_cluster_size=int(args.min_cluster_size),
        max_clusters=int(args.max_clusters),
    )
    boundary_leaks = compute_boundary_leaks(
        weak_edges,
        node_to_cluster=node_to_cluster,
        max_boundary_leaks=int(args.max_boundary_leaks),
    )
    hubs = compute_hubs(
        weak_edges,
        file_commit_counts=file_commit_counts,
        sum_couplings=sum_couplings,
        node_to_cluster=node_to_cluster,
        max_hubs=int(args.max_hubs),
    )
    report_core = build_report(
        edges=edges,
        file_commit_counts=file_commit_counts,
        sum_couplings=sum_couplings,
        max_pairs=500,
    )

    payload = {
        "ok": True,
        "skipped": False,
        "timestamp": time.time(),
        "duration_sec": round(time.time() - start, 3),
        "params": {
            "max_commits": int(args.max_commits),
            "since": args.since,
            "min_pair_count": int(args.min_pair_count),
            "min_jaccard": min_jaccard,
            "max_files_per_commit": int(args.max_files_per_commit),
            "max_churn_per_commit": int(args.max_churn_per_commit),
            "group_by": args.group_by,
            "dir_depth": int(args.dir_depth),
            "min_cluster_size": int(args.min_cluster_size),
            "max_clusters": int(args.max_clusters),
            "max_boundary_leaks": int(args.max_boundary_leaks),
            "max_hubs": int(args.max_hubs),
            "detect_renames": bool(args.detect_renames),
        },
        "stats": {
            "commits_seen": len(commits_meta),
            "commits_used": len(commits_filtered),
            "skipped_large_commits": int(skipped_large),
            "skipped_high_churn_commits": int(skipped_high_churn),
            "unique_nodes": len(file_commit_counts),
            "pair_edges": len(pair_counts),
            "strong_edges": len(strong_edges),
        },
        "clusters": clusters,
        "boundary_leaks": boundary_leaks,
        "hubs": hubs,
        **report_core,
    }

    decoupling_rel = ".vibe/reports/decoupling_suggestions.md"
    payload["decoupling_suggestions_md_path"] = decoupling_rel
    decoupling_path = root / decoupling_rel
    _write_text(decoupling_path, render_decoupling_suggestions_md(payload))

    _write_json(out_path, payload)

    print(f"[coupling] wrote: {out_path}")
    print(f"[coupling] wrote: {decoupling_path}")
    print(
        f"[coupling] commits_used={payload['stats']['commits_used']} unique_nodes={payload['stats']['unique_nodes']} edges={payload['stats']['pair_edges']} skipped_large={skipped_large} skipped_high_churn={skipped_high_churn}"
    )

    top = max(0, int(args.top))
    if top and payload.get("pairs"):
        print("\nTop pairs:")
        for r in payload["pairs"][:top]:
            print(f"- {r['a']} <-> {r['b']} (count={r['count']}, jaccard={r['jaccard']})")

    if top and payload.get("files"):
        print("\nTop sum-of-couplings:")
        for r in payload["files"][:top]:
            print(f"- {r['path']} (sum_couplings={r['sum_couplings']}, commits={r['commits']})")

    if top and payload.get("clusters"):
        print("\nTop clusters:")
        for c in payload["clusters"][: min(top, 5)]:
            nodes = c.get("nodes") or []
            nodes_s = ", ".join(list(nodes)[:6]) + (" ..." if isinstance(nodes, list) and len(nodes) > 6 else "")
            print(f"- #{c.get('id')} (nodes={c.get('node_count')}, internal_sum={c.get('internal_count_sum')}): {nodes_s}")

    if top and payload.get("boundary_leaks"):
        print("\nTop boundary leaks:")
        for r in payload["boundary_leaks"][: min(top, 5)]:
            print(f"- {r['a']} <-> {r['b']} (count={r['count']}, jaccard={r['jaccard']})")

    if top and payload.get("hubs"):
        print("\nTop hubs:")
        for r in payload["hubs"][: min(top, 5)]:
            print(
                f"- {r['node']} (clusters={r['connected_clusters_count']}, cross_sum={r['cross_edge_count_sum']}, sum_couplings={r['sum_couplings']})"
            )

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

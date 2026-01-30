#!/usr/bin/env python3
from __future__ import annotations

import argparse
import fnmatch
import json
import subprocess
import sys
import time
from pathlib import Path

import indexer as vibe_indexer
from context_db import connect, is_excluded, load_config, normalize_rel


def _git_available(root: Path) -> bool:
    return (root / ".git").exists()


def _run_git(root: Path, args: list[str]) -> list[str]:
    p = subprocess.run(["git", *args], cwd=str(root), text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if p.returncode != 0:
        raise RuntimeError(p.stderr.strip() or f"git failed: {' '.join(args)}")
    return [ln.strip() for ln in p.stdout.splitlines() if ln.strip()]


def _files_staged(root: Path) -> list[str]:
    return _run_git(root, ["diff", "--cached", "--name-only", "--diff-filter=ACM"])


def _files_changed(root: Path) -> list[str]:
    changed = _run_git(root, ["diff", "--name-only", "--diff-filter=ACM"])
    # Include untracked files for "changed" scope.
    try:
        untracked = _run_git(root, ["ls-files", "-o", "--exclude-standard"])
    except Exception:
        untracked = []
    return sorted(set([*changed, *untracked]))


def _matches_include(rel_posix: str, include_globs: list[str]) -> bool:
    if not include_globs:
        return True
    for g in include_globs:
        if fnmatch.fnmatch(rel_posix, g):
            return True
        # Treat "**/" prefix as optional so patterns like "**/*.md" match root files too.
        if g.startswith("**/") and fnmatch.fnmatch(rel_posix, g[3:]):
            return True
    return False


def _files_from_path(root: Path, cfg, scope_path: str) -> list[str]:
    p = Path(scope_path)
    if not p.is_absolute():
        p = root / p
    p = p.resolve(strict=False)

    try:
        rel_root = p.relative_to(root)
    except ValueError:
        return []

    if p.is_file():
        rel = normalize_rel(rel_root)
        if is_excluded(Path(rel), cfg.exclude_dirs):
            return []
        if not _matches_include(rel, cfg.include_globs):
            return []
        return [rel]

    if not p.exists():
        return []

    files: list[str] = []
    for child in p.rglob("*"):
        if not child.is_file():
            continue
        try:
            rel = child.relative_to(root)
        except ValueError:
            continue
        if is_excluded(rel, cfg.exclude_dirs):
            continue
        rel_s = normalize_rel(rel)
        if _matches_include(rel_s, cfg.include_globs):
            files.append(rel_s)
    return sorted(set(files))


def _filter_repo_files(root: Path, cfg, rel_paths: list[str]) -> list[str]:
    out: list[str] = []
    for s in rel_paths:
        if not s:
            continue
        rel = Path(s)
        if rel.is_absolute():
            try:
                rel = rel.relative_to(root)
            except ValueError:
                continue
        if is_excluded(rel, cfg.exclude_dirs):
            continue
        rel_s = normalize_rel(rel)
        if not _matches_include(rel_s, cfg.include_globs):
            continue
        if not (root / rel).exists():
            continue
        out.append(rel_s)
    return sorted(set(out))


def _load_json(path: Path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return None


def _format_ts(ts: float) -> str:
    return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(ts))


class _MdBuilder:
    def __init__(self, max_bytes: int):
        self._max_bytes = max_bytes
        self._used = 0
        self._lines: list[str] = []
        self._truncated = False

    def add(self, line: str = "") -> bool:
        if self._truncated:
            return False
        b = (line + "\n").encode("utf-8")
        if self._used + len(b) > self._max_bytes:
            self._truncated = True
            return False
        self._lines.append(line)
        self._used += len(b)
        return True

    def finish(self) -> str:
        if self._truncated:
            # Best effort: ensure the truncation marker fits.
            marker = "\n... (truncated)\n"
            b = marker.encode("utf-8")
            if self._used + len(b) <= self._max_bytes:
                self._lines.append("... (truncated)")
        return "\n".join(self._lines).rstrip() + "\n"


def _files_recent(con, limit: int) -> list[str]:
    rows = con.execute("SELECT path FROM files ORDER BY mtime DESC LIMIT ?", (int(limit),)).fetchall()
    return [r["path"] for r in rows]


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Generate a compact context PACK.md for LLMs.")
    ap.add_argument("--scope", choices=["staged", "changed", "path", "recent"], default="staged")
    ap.add_argument("--path", help="Path for --scope=path (file or directory).")
    ap.add_argument("--max-kb", type=int, default=24)
    ap.add_argument("--out", default=".vibe/context/PACK.md")
    ap.add_argument("--symbols-per-file", type=int, default=5)
    ap.add_argument("--refresh-index", action="store_true", help="Index the selected files before packing.")
    args = ap.parse_args(argv)

    cfg = load_config()
    root = cfg.root
    max_bytes = max(4, int(args.max_kb)) * 1024

    con = connect()
    try:
        scope = args.scope
        if scope in ("staged", "changed") and not _git_available(root):
            print(f"[pack] scope={scope} requires git; falling back to scope=recent", file=sys.stderr)
            scope = "recent"

        if scope == "staged":
            rel_paths = _files_staged(root)
        elif scope == "changed":
            rel_paths = _files_changed(root)
        elif scope == "recent":
            rel_paths = _files_recent(con, cfg.max_recent_files)
        else:
            if not args.path:
                print("[pack] --path is required for --scope=path", file=sys.stderr)
                return 2
            rel_paths = _files_from_path(root, cfg, args.path)

        rel_paths = _filter_repo_files(root, cfg, rel_paths)
        if not rel_paths:
            print("[pack] no matching files for scope", file=sys.stderr)
            return 2

        if args.refresh_index:
            changed = 0
            for rel in rel_paths:
                p = root / rel
                try:
                    if vibe_indexer.index_file(p, cfg, con=con):
                        changed += 1
                except Exception:
                    # best-effort: keep packing even if indexing fails for a file
                    pass
            if changed:
                print(f"[pack] refreshed index for {changed} files", file=sys.stderr)

        complexity = _load_json(root / ".vibe" / "reports" / "complexity.json") or []
        complexity_by_file: dict[str, list[dict]] = {}
        if isinstance(complexity, list):
            for row in complexity:
                f = (row or {}).get("file")
                if not isinstance(f, str) or not f:
                    continue
                complexity_by_file.setdefault(f, []).append(row)
            for f in list(complexity_by_file.keys()):
                complexity_by_file[f] = sorted(
                    complexity_by_file[f],
                    key=lambda r: (-int((r or {}).get("score") or 0), -int((r or {}).get("lines") or 0)),
                )

        b = _MdBuilder(max_bytes=max_bytes)
        b.add("# PACK")
        b.add("")
        b.add(f"- Project: `{cfg.project_name}`")
        b.add(f"- Generated: `{_format_ts(time.time())}`")
        b.add(f"- Scope: `{scope}` ({len(rel_paths)} files)")
        if scope == "path":
            b.add(f"- Path: `{args.path}`")
        b.add("")

        b.add("## Files")
        for rel in rel_paths:
            file_row = con.execute("SELECT loc,mtime FROM files WHERE path = ?", (rel,)).fetchone()
            loc = int(file_row["loc"]) if file_row else 0
            mtime = float(file_row["mtime"]) if file_row else 0.0
            if not b.add(f"- `{rel}` (loc={loc}, mtime={_format_ts(mtime) if mtime else 'n/a'})"):
                break

            warnings = complexity_by_file.get(rel) or []
            if warnings:
                w = warnings[0]
                name = (w.get("name") or "").strip()
                lines = int(w.get("lines") or 0)
                nesting = int(w.get("nesting") or 0)
                params = int(w.get("params") or 0)
                b.add(f"  - complexity: {name} (lines={lines}, nesting={nesting}, params={params})")

            syms = con.execute(
                "SELECT name,line,kind,signature,exported_int FROM symbols WHERE file = ? "
                "ORDER BY exported_int DESC, line ASC LIMIT ?",
                (rel, int(args.symbols_per_file)),
            ).fetchall()
            if syms:
                parts = []
                for s in syms:
                    sig = (s["signature"] or s["name"]).strip()
                    parts.append(f"{sig} @L{s['line']}")
                b.add("  - symbols: " + " | ".join(parts))

        b.add("")
        b.add("## Commands")
        b.add("- Full scan: `python3 scripts/vibe.py doctor --full`")
        b.add("- Tests: `dotnet test tests/XTranslatorAi.Tests/XTranslatorAi.Tests.csproj -c Release`")
        b.add("- Search: `python3 scripts/vibe.py search <query>`")
        b.add("")
        b.add("## Notes")
        b.add("- Treat runtime placeholders/tokens as a contract (`<mag>`, `<dur>`, `__XT_*__`, `[pagebreak]`).")

        out_path = root / args.out
        out_path.parent.mkdir(parents=True, exist_ok=True)
        text = b.finish()
        out_path.write_text(text, encoding="utf-8")
        print(f"[pack] wrote: {out_path} ({len(text.encode('utf-8'))} bytes)")
        return 0
    finally:
        con.close()


if __name__ == "__main__":
    raise SystemExit(main(__import__("sys").argv[1:]))

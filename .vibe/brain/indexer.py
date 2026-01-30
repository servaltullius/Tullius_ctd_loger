#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

from context_db import VibeConfig, connect, is_excluded, load_config, normalize_rel, sha1_text


TYPE_RE = re.compile(
    r"^\s*(?P<attrs>(\[[^\]]+\]\s*)*)"
    r"(?P<access>public|internal|protected|private)?\s*"
    r"(?P<mods>(?:static|partial|sealed|abstract|readonly|ref|unsafe|new)\s+)*"
    r"(?P<kind>class|struct|interface|record|enum)\s+"
    r"(?P<name>[A-Za-z_][A-Za-z0-9_]*)",
    re.MULTILINE,
)

METHOD_RE = re.compile(
    r"^\s*(?P<attrs>(\[[^\]]+\]\s*)*)"
    r"(?P<access>public|internal|protected|private)?\s*"
    r"(?P<mods>(?:static|async|virtual|override|sealed|new|unsafe|extern|partial)\s+)*"
    r"(?P<ret>[A-Za-z_][A-Za-z0-9_<>,\[\]\?\.\s]*)\s+"
    r"(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*"
    r"\((?P<params>[^)]*)\)\s*"
    r"(?P<trailer>(?:where\s+[^{=]+)?\s*)(?:\{|=>)",
    re.MULTILINE,
)

NAMESPACE_RE = re.compile(r"^\s*namespace\s+(?P<ns>[A-Za-z_][A-Za-z0-9_\.]*)\s*;?", re.MULTILINE)

XMLDOC_LINE_RE = re.compile(r"^\s*///\s?(?P<text>.*)$")
ATTR_LINE_RE = re.compile(r"^\s*\[(?P<attr>[^\]]+)\]\s*$")


@dataclass
class Symbol:
    name: str
    file: str
    line: int
    kind: str
    signature: str | None
    access: str | None
    doc: str | None
    tags: list[str]
    attrs: list[str]
    exported: int


def _iter_files(cfg: VibeConfig) -> list[Path]:
    root = cfg.root
    out: list[Path] = []
    for pattern in cfg.include_globs:
        for p in root.glob(pattern):
            if not p.is_file():
                continue
            rel = p.relative_to(root)
            if is_excluded(rel, cfg.exclude_dirs):
                continue
            out.append(p)
    # de-dup (glob can overlap)
    uniq = {p.resolve(): p for p in out}
    return sorted(uniq.values(), key=lambda x: x.as_posix())


def _read_text(path: Path) -> str | None:
    try:
        return path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        return path.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return None


def _line_number(text: str, idx: int) -> int:
    return text.count("\n", 0, idx) + 1


def _extract_preceding_xmldoc(lines: list[str], decl_line_idx: int) -> str | None:
    doc_lines: list[str] = []
    i = decl_line_idx - 1
    while i >= 0:
        m = XMLDOC_LINE_RE.match(lines[i])
        if not m:
            break
        doc_lines.append(m.group("text").rstrip())
        i -= 1
    if not doc_lines:
        return None
    doc_lines.reverse()
    return "\n".join(doc_lines).strip()


def _extract_preceding_attrs(lines: list[str], decl_line_idx: int) -> list[str]:
    attrs: list[str] = []
    i = decl_line_idx - 1
    # capture up to 5 contiguous attribute lines directly above declaration
    while i >= 0 and len(attrs) < 5:
        line = lines[i].strip()
        if not line:
            i -= 1
            continue
        m = ATTR_LINE_RE.match(lines[i])
        if not m:
            break
        attrs.append(m.group("attr").strip())
        i -= 1
    attrs.reverse()
    return attrs


def _tags_from_text(text: str | None, critical_tags: list[str]) -> list[str]:
    if not text:
        return []
    tags: list[str] = []
    for t in critical_tags:
        if t in text:
            tags.append(t)
    return tags


def _extract_symbols_cs(text: str, rel_file: str, cfg: VibeConfig) -> list[Symbol]:
    lines = text.splitlines()
    syms: list[Symbol] = []
    ns_match = NAMESPACE_RE.search(text)
    current_ns = ns_match.group("ns") if ns_match else None

    def qualify(name: str) -> str:
        return f"{current_ns}.{name}" if current_ns else name

    for m in TYPE_RE.finditer(text):
        line = _line_number(text, m.start())
        kind = m.group("kind")
        name = qualify(m.group("name"))
        access = m.group("access")
        exported = 1 if access == "public" else 0
        decl_line_idx = line - 1
        doc = _extract_preceding_xmldoc(lines, decl_line_idx)
        attrs = _extract_preceding_attrs(lines, decl_line_idx)
        tags = _tags_from_text(doc, cfg.critical_tags)
        signature = f"{access or ''} {kind} {name}".strip()
        syms.append(
            Symbol(
                name=name,
                file=rel_file,
                line=line,
                kind=kind,
                signature=signature,
                access=access,
                doc=doc,
                tags=tags,
                attrs=attrs,
                exported=exported,
            )
        )

    for m in METHOD_RE.finditer(text):
        line = _line_number(text, m.start())
        name_raw = m.group("name")
        # Skip common false positives: constructors in record/class where ret matches name.
        ret = (m.group("ret") or "").strip()
        if ret.endswith(name_raw):
            # heuristic: likely constructor; still useful, but mark as ctor.
            kind = "ctor"
        else:
            kind = "method"

        access = m.group("access")
        exported = 1 if access == "public" else 0
        params = " ".join((m.group("params") or "").split())
        name = qualify(name_raw)
        decl_line_idx = line - 1
        doc = _extract_preceding_xmldoc(lines, decl_line_idx)
        attrs = _extract_preceding_attrs(lines, decl_line_idx)
        tags = _tags_from_text(doc, cfg.critical_tags)
        signature = f"{access or ''} {ret} {name_raw}({params})".strip()
        syms.append(
            Symbol(
                name=name,
                file=rel_file,
                line=line,
                kind=kind,
                signature=signature,
                access=access,
                doc=doc,
                tags=tags,
                attrs=attrs,
                exported=exported,
            )
        )

    return syms


def _index_project_refs(con, cfg: VibeConfig) -> None:
    # Rebuild deps as csproj graph only.
    con.execute("DELETE FROM deps")
    root = cfg.root
    csprojs = list(root.rglob("*.csproj"))
    for p in csprojs:
        rel_path = p.relative_to(root)
        if is_excluded(rel_path, cfg.exclude_dirs):
            continue
        rel_from = normalize_rel(rel_path)
        text = _read_text(p)
        if not text:
            continue
        for m in re.finditer(r'<ProjectReference\s+Include="(?P<inc>[^"]+)"', text):
            inc = m.group("inc")
            inc_norm = inc.replace("\\", "/")
            to_path = (p.parent / inc_norm).resolve()
            try:
                rel_to = normalize_rel(to_path.relative_to(root))
            except ValueError:
                rel_to = inc_norm
            con.execute(
                "INSERT INTO deps(from_file,to_file,kind) VALUES (?,?,?)",
                (rel_from, rel_to, "ProjectReference"),
            )
    con.commit()


def index_file(path: Path, cfg: VibeConfig, con=None) -> bool:
    root = cfg.root
    rel = path.relative_to(root)
    if is_excluded(rel, cfg.exclude_dirs):
        return False
    rel_s = normalize_rel(rel)

    st = path.stat()
    mtime = float(st.st_mtime)
    size = st.st_size

    own_con = False
    if con is None:
        con = connect()
        own_con = True
    try:
        row = con.execute("SELECT mtime,size,hash FROM files WHERE path = ?", (rel_s,)).fetchone()

        fast_skip = bool(cfg.quality_gates.get("indexer_fast_skip", True))
        if fast_skip and row and float(row["mtime"]) == mtime and int(row["size"]) == size:
            return False

        text = _read_text(path)
        if text is None:
            print(f"[indexer] warn: failed to read {rel_s}", file=sys.stderr)
            return False

        file_hash = sha1_text(text)
        loc = text.count("\n") + 1 if text else 0

        if row and row["hash"] == file_hash:
            con.execute(
                "UPDATE files SET mtime=?, size=?, loc=? WHERE path=?",
                (mtime, size, loc, rel_s),
            )
            con.commit()
            return False

        con.execute(
            "INSERT INTO files(path,mtime,hash,loc,size) VALUES (?,?,?,?,?) "
            "ON CONFLICT(path) DO UPDATE SET mtime=excluded.mtime, hash=excluded.hash, loc=excluded.loc, size=excluded.size",
            (rel_s, mtime, file_hash, loc, size),
        )

        # Replace symbol + fts rows for this file.
        con.execute("DELETE FROM symbols WHERE file = ?", (rel_s,))
        con.execute("DELETE FROM fts_symbols WHERE file = ?", (rel_s,))
        con.execute("DELETE FROM fts_files WHERE path = ?", (rel_s,))

        symbols: list[Symbol] = []
        if path.suffix.lower() == ".cs":
            symbols = _extract_symbols_cs(text, rel_s, cfg)

        for sym in symbols:
            con.execute(
                "INSERT INTO symbols(name,file,line,kind,signature,access,doc,tags_json,attrs_json,exported_int) "
                "VALUES (?,?,?,?,?,?,?,?,?,?)",
                (
                    sym.name,
                    sym.file,
                    sym.line,
                    sym.kind,
                    sym.signature,
                    sym.access,
                    sym.doc,
                    json.dumps(sym.tags, ensure_ascii=False),
                    json.dumps(sym.attrs, ensure_ascii=False),
                    sym.exported,
                ),
            )
            con.execute(
                "INSERT INTO fts_symbols(name,file,doc,tags,signature,attrs) VALUES (?,?,?,?,?,?)",
                (
                    sym.name,
                    sym.file,
                    sym.doc or "",
                    " ".join(sym.tags),
                    sym.signature or "",
                    " ".join(sym.attrs),
                ),
            )

        # File content index (FTS) – skip extremely large files.
        max_bytes = int(cfg.quality_gates.get("fts_files_max_bytes", 2_000_000))
        if max_bytes > 0 and size <= max_bytes:
            con.execute("INSERT INTO fts_files(path,content) VALUES (?,?)", (rel_s, text))

        con.commit()
        return True
    finally:
        if own_con:
            con.close()


def scan_all(cfg: VibeConfig) -> tuple[int, int]:
    files = _iter_files(cfg)
    present = {normalize_rel(p.relative_to(cfg.root)) for p in files}
    total = len(files)
    changed = 0
    start = time.time()
    con = connect()
    try:
        for i, p in enumerate(files, 1):
            if index_file(p, cfg, con=con):
                changed += 1
            if i % 200 == 0:
                print(f"[indexer] {i}/{total} files...", file=sys.stderr)

        # Remove stale rows for files that no longer exist, so reports stay accurate after renames/deletes.
        stale = {
            r["path"]
            for r in con.execute("SELECT path FROM files").fetchall()
            if r["path"] not in present
        }
        if stale:
            _purge_stale_files(con, sorted(stale))

        # Rebuild project reference deps once per full scan.
        _index_project_refs(con, cfg)
    finally:
        con.close()
    dur = time.time() - start

    return total, changed


def _purge_stale_files(con, paths: list[str]) -> None:
    # Chunk deletes to stay well below SQLite parameter limits.
    for i in range(0, len(paths), 200):
        chunk = paths[i : i + 200]
        q = ",".join("?" for _ in chunk)
        con.execute(f"DELETE FROM symbols WHERE file IN ({q})", chunk)
        con.execute(f"DELETE FROM fts_symbols WHERE file IN ({q})", chunk)
        con.execute(f"DELETE FROM fts_files WHERE path IN ({q})", chunk)
        con.execute(f"DELETE FROM files WHERE path IN ({q})", chunk)
    con.commit()


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="vibe-kit indexer (C# + docs).")
    g = parser.add_mutually_exclusive_group(required=True)
    g.add_argument("--scan-all", action="store_true", help="Index all configured files.")
    g.add_argument("--file", help="Index a single file (repo-relative or absolute).")
    args = parser.parse_args(argv)

    cfg = load_config()
    if args.file:
        p = Path(args.file)
        if not p.is_absolute():
            p = cfg.root / p
        p = p.resolve(strict=False)
        root_res = cfg.root.resolve()
        try:
            p.relative_to(root_res)
        except ValueError:
            print(f"[indexer] refuse: outside repo root: {p}", file=sys.stderr)
            return 2
        if not p.exists():
            print(f"[indexer] file not found: {args.file}", file=sys.stderr)
            return 2
        index_file(p, cfg)
        return 0

    total, changed = scan_all(cfg)
    print(f"[indexer] done: {total} files indexed (changed≈{changed})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

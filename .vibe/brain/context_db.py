#!/usr/bin/env python3
from __future__ import annotations

import hashlib
import json
import os
import sqlite3
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable


@dataclass(frozen=True)
class VibeConfig:
    project_name: str
    root: Path
    exclude_dirs: list[str]
    include_globs: list[str]
    critical_tags: list[str]
    latest_file: Path
    max_recent_files: int
    checks: dict[str, Any]
    quality_gates: dict[str, Any]
    placeholders: dict[str, Any]
    profiling: dict[str, Any]
    architecture: dict[str, Any]


def repo_root() -> Path:
    here = Path(__file__).resolve()
    return here.parents[2]


def vibe_dir() -> Path:
    return repo_root() / ".vibe"


def load_config() -> VibeConfig:
    cfg_path = vibe_dir() / "config.json"
    data = json.loads(cfg_path.read_text(encoding="utf-8"))

    root = repo_root()
    context = data.get("context") or {}
    latest_file = root / str(context.get("latest_file", ".vibe/context/LATEST_CONTEXT.md"))
    max_recent_files = int(context.get("max_recent_files", 12))

    return VibeConfig(
        project_name=str(data.get("project_name", root.name)),
        root=root,
        exclude_dirs=list(data.get("exclude_dirs") or []),
        include_globs=list(data.get("include_globs") or []),
        critical_tags=list(data.get("critical_tags") or ["@critical", "CRITICAL:"]),
        latest_file=latest_file,
        max_recent_files=max_recent_files,
        checks=dict(data.get("checks") or {}),
        quality_gates=dict(data.get("quality_gates") or {}),
        placeholders=dict(data.get("placeholders") or {}),
        profiling=dict(data.get("profiling") or {}),
        architecture=dict(data.get("architecture") or {}),
    )


def db_path() -> Path:
    return vibe_dir() / "db" / "context.sqlite"


def connect() -> sqlite3.Connection:
    path = db_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    con = sqlite3.connect(str(path))
    con.row_factory = sqlite3.Row
    con.execute("PRAGMA journal_mode=WAL;")
    con.execute("PRAGMA synchronous=NORMAL;")
    con.execute("PRAGMA foreign_keys=ON;")
    con.execute("PRAGMA busy_timeout=2500;")
    ensure_schema(con)
    return con


def ensure_schema(con: sqlite3.Connection) -> None:
    con.executescript(
        """
        CREATE TABLE IF NOT EXISTS files (
          path TEXT PRIMARY KEY,
          mtime REAL NOT NULL,
          hash TEXT NOT NULL,
          loc INTEGER NOT NULL,
          size INTEGER NOT NULL
        );

        CREATE TABLE IF NOT EXISTS symbols (
          id INTEGER PRIMARY KEY AUTOINCREMENT,
          name TEXT NOT NULL,
          file TEXT NOT NULL,
          line INTEGER NOT NULL,
          kind TEXT NOT NULL,
          signature TEXT,
          access TEXT,
          doc TEXT,
          tags_json TEXT,
          attrs_json TEXT,
          exported_int INTEGER NOT NULL DEFAULT 0
        );
        CREATE INDEX IF NOT EXISTS idx_symbols_file ON symbols(file);
        CREATE INDEX IF NOT EXISTS idx_symbols_name ON symbols(name);

        CREATE TABLE IF NOT EXISTS deps (
          from_file TEXT NOT NULL,
          to_file TEXT NOT NULL,
          kind TEXT NOT NULL
        );
        CREATE INDEX IF NOT EXISTS idx_deps_from ON deps(from_file);
        CREATE INDEX IF NOT EXISTS idx_deps_to ON deps(to_file);

        CREATE VIRTUAL TABLE IF NOT EXISTS fts_symbols
          USING fts5(name, file, doc, tags, signature, attrs);

        CREATE VIRTUAL TABLE IF NOT EXISTS fts_files
          USING fts5(path, content);

        CREATE TABLE IF NOT EXISTS meta (
          key TEXT PRIMARY KEY,
          value TEXT NOT NULL
        );
        """
    )


def sha1_text(text: str) -> str:
    return hashlib.sha1(text.encode("utf-8", errors="ignore")).hexdigest()


def is_excluded(rel_path: Path, exclude_dirs: Iterable[str]) -> bool:
    parts = {p.lower() for p in rel_path.parts}
    for ex in exclude_dirs:
        if ex.lower() in parts:
            return True
    return False


def normalize_rel(path: Path) -> str:
    return path.as_posix()

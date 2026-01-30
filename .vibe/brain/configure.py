#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import time
from pathlib import Path
from typing import Any


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def _load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def _dump_json(obj: Any) -> str:
    return json.dumps(obj, ensure_ascii=False, indent=2) + "\n"


def _write_text(path: Path, text: str, *, apply: bool) -> None:
    if not apply:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def _ensure_dict(parent: dict[str, Any], key: str) -> dict[str, Any]:
    cur = parent.get(key)
    if isinstance(cur, dict):
        return cur
    d: dict[str, Any] = {}
    parent[key] = d
    return d


def _set_dotted(
    root: dict[str, Any],
    dotted: str,
    value: Any,
    *,
    force: bool,
    changes: list[dict[str, Any]],
    skipped: list[dict[str, Any]],
) -> None:
    parts = dotted.split(".")
    cur: dict[str, Any] = root
    for k in parts[:-1]:
        nxt = cur.get(k)
        if isinstance(nxt, dict):
            cur = nxt
            continue
        if nxt is None or force:
            created: dict[str, Any] = {}
            cur[k] = created
            cur = created
            continue
        skipped.append({"path": dotted, "reason": f"non-dict parent at {k!r}"})
        return

    leaf = parts[-1]
    had_key = leaf in cur
    before = cur.get(leaf, None)
    if before == value:
        return
    # Treat explicit JSON null as "unset" for auto-config.
    if had_key and before is not None and not force:
        skipped.append({"path": dotted, "before": before, "after": value, "reason": "already set"})
        return
    cur[leaf] = value
    overwrote = bool(had_key and before is not None)
    changes.append({"path": dotted, "before": before, "after": value, "forced": bool(force and overwrote)})


def _walk_repo_files(root: Path, exclude_dirs: set[str]) -> Any:
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d.lower() not in exclude_dirs]
        for name in filenames:
            yield Path(dirpath) / name


def _repo_has_any_suffix(root: Path, exclude_dirs: set[str], suffixes: set[str]) -> bool:
    for p in _walk_repo_files(root, exclude_dirs):
        if p.suffix.lower() in suffixes:
            return True
    return False


def _repo_has_any_named_file(root: Path, exclude_dirs: set[str], names: set[str]) -> bool:
    names_l = {n.lower() for n in names}
    for p in _walk_repo_files(root, exclude_dirs):
        if p.name.lower() in names_l:
            return True
    return False


def _read_text_best_effort(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return ""


def _detect_package_manager(root: Path, package_json: dict[str, Any]) -> tuple[str, str]:
    raw_pm = package_json.get("packageManager")
    if isinstance(raw_pm, str) and raw_pm.strip():
        name = raw_pm.strip().split("@", 1)[0].strip().lower()
        if name in {"npm", "pnpm", "yarn", "bun"}:
            return name, "packageManager field"

    # Fall back to lockfiles.
    if (root / "bun.lock").exists() or (root / "bun.lockb").exists():
        return "bun", "lockfile"
    if (root / "pnpm-lock.yaml").exists():
        return "pnpm", "lockfile"
    if (root / "yarn.lock").exists():
        return "yarn", "lockfile"
    if (root / "package-lock.json").exists():
        return "npm", "lockfile"

    return "npm", "default"


def _pick_typecheck_recommendation(
    *,
    root: Path,
    exclude_dirs: set[str],
    package_json: dict[str, Any] | None,
    pm: str | None,
) -> tuple[list[str] | None, list[str] | None, dict[str, Any]]:
    meta: dict[str, Any] = {}

    dotnet_present = False
    for p in _walk_repo_files(root, exclude_dirs):
        suf = p.suffix.lower()
        if suf in {".sln", ".csproj", ".fsproj", ".vbproj"}:
            dotnet_present = True
            break
    meta["dotnet_present"] = dotnet_present
    if dotnet_present:
        return None, None, meta

    if package_json is not None and pm:
        scripts = package_json.get("scripts")
        has_typecheck_script = isinstance(scripts, dict) and "typecheck" in scripts
        meta["node_present"] = True
        meta["node_pm"] = pm
        meta["node_has_typecheck_script"] = bool(has_typecheck_script)
        if has_typecheck_script:
            ts_present = (root / "tsconfig.json").exists() or _repo_has_any_suffix(root, exclude_dirs, {".ts", ".tsx"})
            meta["ts_present"] = bool(ts_present)
            when = ["**/*.ts", "**/*.tsx"] if ts_present else ["**/*.js", "**/*.jsx"]
            return [pm, "run", "typecheck"], when, meta

    mypy_present = (root / "mypy.ini").exists()
    if not mypy_present and (root / "pyproject.toml").exists():
        pyproj = _read_text_best_effort(root / "pyproject.toml")
        mypy_present = "[tool.mypy]" in pyproj
    meta["mypy_present"] = bool(mypy_present)
    if mypy_present:
        return ["mypy", "."], ["**/*.py"], meta

    go_present = (root / "go.mod").exists() or _repo_has_any_named_file(root, exclude_dirs, {"go.mod"})
    meta["go_present"] = bool(go_present)
    if go_present:
        return ["go", "test", "./...", "-run=^$"], ["**/*.go", "go.mod", "go.sum"], meta

    rust_present = (root / "Cargo.toml").exists() or _repo_has_any_named_file(root, exclude_dirs, {"Cargo.toml"})
    meta["rust_present"] = bool(rust_present)
    if rust_present:
        return ["cargo", "check"], ["**/*.rs", "Cargo.toml", "Cargo.lock"], meta

    return None, None, meta


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Auto-detect repo stack and configure vibe-kit settings.")
    ap.add_argument("--apply", action="store_true", help="Write changes to `.vibe/config.json`.")
    ap.add_argument("--force", action="store_true", help="Overwrite existing configured values.")
    args = ap.parse_args(argv)

    root = _repo_root()
    cfg_path = root / ".vibe" / "config.json"
    if not cfg_path.exists():
        print("[configure] missing .vibe/config.json (run: python3 scripts/vibe.py bootstrap)")
        return 2

    cfg = _load_json(cfg_path)
    exclude_dirs_list = cfg.get("exclude_dirs")
    exclude_dirs = {str(x).lower() for x in exclude_dirs_list} if isinstance(exclude_dirs_list, list) else set()
    exclude_dirs |= {".git", ".vibe"}

    changes: list[dict[str, Any]] = []
    skipped: list[dict[str, Any]] = []
    detected: dict[str, Any] = {}

    _ensure_dict(cfg, "checks")
    checks = cfg["checks"]
    if isinstance(checks, dict):
        if checks.get("doctor") is None:
            _set_dotted(cfg, "checks.doctor", [], force=args.force, changes=changes, skipped=skipped)
        if checks.get("precommit") is None:
            _set_dotted(cfg, "checks.precommit", [], force=args.force, changes=changes, skipped=skipped)
    else:
        skipped.append({"path": "checks", "reason": "checks is not a dict"})

    quality_gates = _ensure_dict(cfg, "quality_gates")

    package_json_path = root / "package.json"
    package_json: dict[str, Any] | None = None
    pm: str | None = None
    pm_source: str | None = None
    if package_json_path.exists():
        try:
            package_json = _load_json(package_json_path)
            pm, pm_source = _detect_package_manager(root, package_json)
        except json.JSONDecodeError:
            skipped.append({"path": "package.json", "reason": "invalid JSON"})

    if pm:
        detected["node_package_manager"] = pm
        detected["node_pm_source"] = pm_source

    cmd, when_globs, detect_meta = _pick_typecheck_recommendation(
        root=root,
        exclude_dirs=exclude_dirs,
        package_json=package_json,
        pm=pm,
    )
    detected.update(detect_meta)

    existing_cmd = quality_gates.get("typecheck_cmd") if isinstance(quality_gates, dict) else None
    if cmd and (existing_cmd is None or args.force):
        _set_dotted(cfg, "quality_gates.typecheck_cmd", cmd, force=args.force, changes=changes, skipped=skipped)
    elif cmd:
        skipped.append({"path": "quality_gates.typecheck_cmd", "reason": "already set"})

    if when_globs:
        existing_when = quality_gates.get("typecheck_when_any_glob") if isinstance(quality_gates, dict) else None
        if existing_when is None or args.force:
            _set_dotted(
                cfg,
                "quality_gates.typecheck_when_any_glob",
                when_globs,
                force=args.force,
                changes=changes,
                skipped=skipped,
            )
        else:
            skipped.append({"path": "quality_gates.typecheck_when_any_glob", "reason": "already set"})

    report = {
        "timestamp": time.time(),
        "apply": bool(args.apply),
        "force": bool(args.force),
        "detected": detected,
        "changes": changes,
        "skipped": skipped,
    }
    report_path = root / ".vibe" / "reports" / "configure_report.json"
    _write_text(report_path, _dump_json(report), apply=True)

    if args.apply:
        _write_text(cfg_path, _dump_json(cfg), apply=True)
        print(f"[configure] wrote: {cfg_path}")
    else:
        print("[configure] dry-run (use --apply to write .vibe/config.json)")

    if changes:
        print("[configure] proposed changes:")
        for ch in changes:
            path = ch.get("path")
            after = ch.get("after")
            print(f"  - {path}: {after!r}")
    else:
        print("[configure] no config changes proposed.")

    print(f"[configure] wrote report: {report_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(__import__('sys').argv[1:]))

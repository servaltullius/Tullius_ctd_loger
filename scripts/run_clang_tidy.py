#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

PRODUCTION_DIRS = ("dump_tool/src", "helper/src", "plugin/src")
REPLACED_SOURCE = "plugin/src/PluginInfo.cpp"
GENERATED_PLUGIN_SOURCE = "SkyrimDiagPluginInfo.cpp"
PRODUCT_TARGET_MARKERS = (
    "CMakeFiles/SkyrimDiag.dir",
    "CMakeFiles/SkyrimDiagHelper.dir",
    "CMakeFiles/SkyrimDiagDumpToolCore.dir",
    "CMakeFiles/SkyrimDiagDumpToolNative.dir",
    "CMakeFiles/SkyrimDiagDumpToolCli.dir",
    "CMakeFiles/SkyrimDiagDumpToolWinUILauncher.dir",
)


def _entry_path(entry: dict[str, object]) -> Path:
    raw_file = entry.get("file")
    raw_directory = entry.get("directory")
    if not isinstance(raw_file, str) or not isinstance(raw_directory, str):
        raise ValueError("compile database entry requires string file and directory")
    path = Path(raw_file)
    if not path.is_absolute():
        path = Path(raw_directory) / path
    return path.resolve()


def _command_text(entry: dict[str, object]) -> str:
    command = entry.get("command")
    if isinstance(command, str):
        return command.replace("\\", "/")
    arguments = entry.get("arguments")
    if isinstance(arguments, list):
        return " ".join(str(item) for item in arguments).replace("\\", "/")
    return ""


def _pick_product_entry(
    entries: list[dict[str, object]], source: Path
) -> dict[str, object]:
    def score(entry: dict[str, object]) -> tuple[int, str] | None:
        command = _command_text(entry)
        for index, marker in enumerate(PRODUCT_TARGET_MARKERS):
            if marker.casefold() in command.casefold():
                return index, command
        return None

    product_entries = [
        (entry_score, entry)
        for entry in entries
        if (entry_score := score(entry)) is not None
    ]
    if not product_entries:
        raise ValueError(
            f"production source has no product target compile entry: {source}"
        )
    return min(product_entries, key=lambda item: item[0])[1]


def prepare_full_production_database(
    repo_root: Path, build_dir: Path
) -> tuple[Path, list[Path], list[str]]:
    database_path = build_dir / "compile_commands.json"
    if not database_path.is_file():
        raise FileNotFoundError(f"compile database not found: {database_path}")
    raw_database = json.loads(database_path.read_text(encoding="utf-8-sig"))
    if not isinstance(raw_database, list):
        raise ValueError(f"compile database must contain a JSON array: {database_path}")

    entries_by_path: dict[Path, list[dict[str, object]]] = {}
    for raw_entry in raw_database:
        if not isinstance(raw_entry, dict):
            raise ValueError("compile database contains a non-object entry")
        path = _entry_path(raw_entry)
        entries_by_path.setdefault(path, []).append(raw_entry)

    expected: dict[Path, str] = {}
    for rel_dir in PRODUCTION_DIRS:
        for path in (repo_root / rel_dir).glob("*.cpp"):
            rel = path.resolve().relative_to(repo_root).as_posix()
            if rel != REPLACED_SOURCE:
                expected[path.resolve()] = rel

    missing = [
        rel for path, rel in sorted(expected.items(), key=lambda item: item[1])
        if path not in entries_by_path
    ]
    generated = sorted(
        path for path in entries_by_path if path.name == GENERATED_PLUGIN_SOURCE
    )
    if not generated:
        missing.append(f"<generated>/{GENERATED_PLUGIN_SOURCE}")
    elif len(generated) > 1:
        raise ValueError(
            f"multiple generated plugin metadata sources found: {generated}"
        )
    if missing:
        raise ValueError(
            "production sources missing from compile database: " + ", ".join(missing)
        )

    selected_paths = sorted(expected, key=lambda path: expected[path])
    if generated:
        selected_paths.append(generated[0])
    selected_entries = [
        _pick_product_entry(entries_by_path[path], path) for path in selected_paths
    ]

    filtered_dir = build_dir / "skydiag-clang-tidy-db"
    filtered_dir.mkdir(parents=True, exist_ok=True)
    filtered_database = filtered_dir / "compile_commands.json"
    filtered_database.write_text(
        json.dumps(selected_entries, indent=2) + "\n", encoding="utf-8"
    )

    labels = [expected[path] for path in selected_paths if path in expected]
    if generated:
        labels.append(f"<generated>/{GENERATED_PLUGIN_SOURCE}")
    return filtered_dir, selected_paths, labels


def _lint_one(
    tidy_binary: str,
    database_dir: Path,
    source: Path,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            tidy_binary,
            "-p",
            str(database_dir),
            "--quiet",
            "--extra-arg=-D_CRT_USE_BUILTIN_OFFSETOF=1",
            "--extra-arg=-fdelayed-template-parsing",
            str(source),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Require and lint the complete Windows production source set."
    )
    parser.add_argument("--repo-root", default=str(Path(__file__).resolve().parents[1]))
    parser.add_argument("--build-dir", default="build-win")
    parser.add_argument("--clang-tidy", default="clang-tidy")
    parser.add_argument("--jobs", type=int, default=max(1, min(4, os.cpu_count() or 1)))
    parser.add_argument("--coverage-only", action="store_true")
    args = parser.parse_args(argv)

    root = Path(args.repo_root).resolve()
    build_dir = Path(args.build_dir)
    if not build_dir.is_absolute():
        build_dir = (root / build_dir).resolve()

    try:
        database_dir, sources, labels = prepare_full_production_database(root, build_dir)
    except (FileNotFoundError, json.JSONDecodeError, OSError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    print(f"==> full Windows production clang-tidy coverage: {len(sources)} file(s)")
    for label in labels:
        print(f"  - {label}")
    if args.coverage_only:
        return 0

    if args.jobs < 1:
        print("ERROR: --jobs must be at least 1", file=sys.stderr)
        return 2

    with ThreadPoolExecutor(max_workers=args.jobs) as executor:
        results = list(
            executor.map(
                lambda source: _lint_one(args.clang_tidy, database_dir, source),
                sources,
            )
        )

    failed = False
    for label, result in zip(labels, results, strict=True):
        if result.stdout:
            print(f"--- clang-tidy: {label} ---")
            print(result.stdout.rstrip())
        if result.returncode != 0:
            failed = True
            print(
                f"ERROR: clang-tidy failed for {label} (exit={result.returncode})",
                file=sys.stderr,
            )

    if failed:
        return 1
    print("==> full Windows production clang-tidy clean")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import sys
import tempfile
from pathlib import Path

from release_contract import (
    BUILD_PROVENANCE_SCHEMA,
    project_version,
    sha256_path,
    source_state,
)


def _parse_artifact(spec: str) -> tuple[str, Path]:
    name, separator, raw_path = spec.partition("=")
    if not separator or not name.strip() or not raw_path.strip():
        raise ValueError(f"artifact must use name=path syntax: {spec}")
    return name.strip().replace("\\", "/"), Path(raw_path).resolve()


def build_manifest(
    root: Path,
    kind: str,
    configuration: str,
    artifacts: list[tuple[str, Path]],
) -> dict[str, object]:
    artifact_hashes: dict[str, str] = {}
    for name, path in sorted(artifacts):
        if name in artifact_hashes:
            raise ValueError(f"duplicate provenance artifact name: {name}")
        if not path.is_file():
            raise FileNotFoundError(f"provenance artifact not found: {path}")
        artifact_hashes[name] = sha256_path(path)
    if not artifact_hashes:
        raise ValueError("build provenance requires at least one artifact")

    return {
        "schema": BUILD_PROVENANCE_SCHEMA,
        "version": project_version(root),
        **source_state(root),
        "kind": kind,
        "configuration": configuration,
        "artifacts": artifact_hashes,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Write commit-bound build provenance.")
    parser.add_argument("--repo-root", default=str(Path(__file__).resolve().parents[1]))
    parser.add_argument("--output", required=True)
    parser.add_argument("--kind", required=True, choices=("native", "winui"))
    parser.add_argument("--configuration", required=True)
    parser.add_argument("--artifact", action="append", default=[])
    parser.add_argument("--artifact-root", default="")
    args = parser.parse_args(argv)

    root = Path(args.repo_root).resolve()
    output = Path(args.output).resolve()
    artifacts = [_parse_artifact(spec) for spec in args.artifact]

    if args.artifact_root:
        artifact_root = Path(args.artifact_root).resolve()
        if not artifact_root.is_dir():
            print(f"ERROR: artifact root not found: {artifact_root}", file=sys.stderr)
            return 2
        for path in sorted(p for p in artifact_root.rglob("*") if p.is_file()):
            if path.resolve() == output:
                continue
            artifacts.append((path.relative_to(artifact_root).as_posix(), path))

    try:
        manifest = build_manifest(root, args.kind, args.configuration, artifacts)
    except (OSError, RuntimeError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1

    output.parent.mkdir(parents=True, exist_ok=True)
    fd, temp_name = tempfile.mkstemp(
        prefix=f".{output.name}.", suffix=".tmp", dir=output.parent
    )
    os.close(fd)
    temp_path = Path(temp_name)
    try:
        temp_path.write_text(
            json.dumps(manifest, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        os.replace(temp_path, output)
    finally:
        temp_path.unlink(missing_ok=True)
    print(f"Wrote build provenance: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

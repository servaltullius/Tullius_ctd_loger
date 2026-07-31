#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import re
import tempfile
from pathlib import Path

RC_TAG_PATTERN = re.compile(
    r"^(v\d+\.\d+\.\d+)-rc[0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*$",
    re.IGNORECASE,
)


def _notes_version(tag: str) -> str:
    """Use the base version only for the project's v<semver>-rc... tags."""
    match = RC_TAG_PATTERN.fullmatch(tag)
    return match.group(1) if match is not None else tag


def resolve_release_notes(repo_root: Path, tag: str) -> str:
    tag = tag.strip()
    if not tag:
        raise ValueError("release tag must not be empty")

    drafts = repo_root / "docs" / "release" / "drafts"
    candidates = [drafts / f"{tag}.md"]
    notes_version = _notes_version(tag)
    if notes_version != tag:
        candidates.append(drafts / f"{notes_version}.md")
    for candidate in candidates:
        if candidate.is_file():
            return candidate.read_text(encoding="utf-8")

    changelog = repo_root / "CHANGELOG.md"
    if changelog.is_file():
        header = re.compile(rf"^##\s+{re.escape(notes_version)}(?:\s|$)")
        next_release = re.compile(r"^##\s+v")
        captured: list[str] = []
        active = False
        for line in changelog.read_text(encoding="utf-8").splitlines():
            if not active and header.match(line):
                active = True
                captured.append(line)
                continue
            if active and next_release.match(line):
                break
            if active:
                captured.append(line)
        if captured:
            return "\n".join(captured).rstrip() + "\n"

    return f"Release {tag}\n"


def _atomic_write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temp_name = tempfile.mkstemp(
        prefix=f".{path.name}.", suffix=".tmp", dir=path.parent
    )
    os.close(fd)
    temp_path = Path(temp_name)
    try:
        temp_path.write_text(text, encoding="utf-8")
        os.replace(temp_path, path)
    finally:
        temp_path.unlink(missing_ok=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Resolve exact or RC-base release notes without broad prerelease fallback."
    )
    parser.add_argument("--repo-root", default=str(Path(__file__).resolve().parents[1]))
    parser.add_argument("--tag", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args(argv)

    root = Path(args.repo_root).resolve()
    output = Path(args.output)
    if not output.is_absolute():
        output = (root / output).resolve()
    _atomic_write(output, resolve_release_notes(root, args.tag))
    print(f"Wrote release notes: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

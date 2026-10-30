#!/usr/bin/env python3
from __future__ import annotations

import argparse
import bisect
import os
import re
import sys
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


# GitHub pairs the `~` of two ranges such as `0.5~0.9GB ... 58~60MB` in one
# paragraph into strikethrough, so release notes write a range as `0.5\~0.9GB`.
_FENCE = re.compile(r"^ {0,3}(`{3,}|~{3,})")
_NEW_BLOCK = re.compile(r"^\s*(?:#{1,6}(?:\s|$)|[-*+]\s|\d{1,9}[.)]\s|>|\|)")
_CODE_SPAN = re.compile(r"(?<!`)(`+)(?!`).*?(?<!`)\1(?!`)", re.S)
_TILDE_RUN = re.compile(r"(?<![\\~])~+(?!~)")


def find_strikethrough_tildes(text: str) -> list[int]:
    """1-based lines holding an unescaped `~` that GitHub may pair into strikethrough.

    Over-approximates GFM: two unescaped runs of one or two tildes in the same
    paragraph, list item, heading or table row count as a pair unless a run has
    whitespace on both sides. Tildes in code spans and fenced blocks are ignored.
    """
    flagged: set[int] = set()
    block: list[tuple[int, str]] = []

    def close_block() -> None:
        if not block:
            return
        joined = "\n".join(line for _, line in block)
        joined = _CODE_SPAN.sub(lambda m: re.sub(r"[^\n]", " ", m.group(0)), joined)
        starts: list[int] = []
        offset = 0
        for _, line in block:
            starts.append(offset)
            offset += len(line) + 1
        runs: dict[int, list[int]] = {}
        for match in _TILDE_RUN.finditer(joined):
            if len(match.group(0)) > 2:
                continue
            before = joined[match.start() - 1] if match.start() > 0 else " "
            after = joined[match.end()] if match.end() < len(joined) else " "
            if before.isspace() and after.isspace():
                continue
            line_number = block[bisect.bisect_right(starts, match.start()) - 1][0]
            runs.setdefault(len(match.group(0)), []).append(line_number)
        for lines in runs.values():
            if len(lines) >= 2:
                flagged.update(lines)
        block.clear()

    fence: str | None = None
    for number, line in enumerate(text.splitlines(), start=1):
        fence_match = _FENCE.match(line)
        if fence is not None:
            marker = fence_match.group(1) if fence_match else ""
            if marker[:1] == fence[0] and len(marker) >= len(fence):
                fence = None
            continue
        if fence_match:
            close_block()
            fence = fence_match.group(1)
            continue
        if not line.strip() or _NEW_BLOCK.match(line):
            close_block()
        if line.strip():
            block.append((number, line))
    close_block()
    return sorted(flagged)


def find_release_notes(repo_root: Path, tag: str) -> str | None:
    """The draft or CHANGELOG section for a tag, or None when neither exists."""
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

    return None


def resolve_release_notes(repo_root: Path, tag: str) -> str:
    notes = find_release_notes(repo_root, tag)
    return notes if notes is not None else f"Release {tag.strip()}\n"


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
    parser.add_argument(
        "--strict",
        action="store_true",
        help="fail instead of publishing a one-line placeholder when no draft or CHANGELOG section exists",
    )
    args = parser.parse_args(argv)

    root = Path(args.repo_root).resolve()
    output = Path(args.output)
    if not output.is_absolute():
        output = (root / output).resolve()
    notes = find_release_notes(root, args.tag)
    if notes is None:
        if args.strict:
            print(
                f"No release notes for {args.tag}: add docs/release/drafts/{args.tag}.md "
                "or a matching CHANGELOG.md section",
                file=sys.stderr,
            )
            return 2
        notes = f"Release {args.tag.strip()}\n"
    _atomic_write(output, notes)
    print(f"Wrote release notes: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

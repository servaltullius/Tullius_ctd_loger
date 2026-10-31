#!/usr/bin/env python3
from __future__ import annotations

import argparse
import bisect
import os
import re
import string
import sys
import tempfile
import unicodedata
from collections.abc import Iterator
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
# paragraph into strikethrough, so docs write a range as `0.5\~0.9GB`. The
# check follows cmark-gfm closely enough to find those pairs and no others:
# text is split where GitHub splits it (paragraphs, list items, headings,
# quotes, table cells; code left out), and a run of one or two `~` opens or
# closes only as the characters beside it allow.
_LINE_END = re.compile(r"\r\n|\r|\n")
_QUOTE = re.compile(r"^ {0,3}> ?")
_FENCE_OPEN = re.compile(r"^\s*(`{3,}|~{3,})(.*)$")
_FENCE_CLOSE = re.compile(r"^\s*(`{3,}|~{3,})[ \t]*$")
_HEADING = re.compile(r"^ {0,3}#{1,6}(?:[ \t]|$)")
_BREAK = re.compile(r"^ {0,3}(?:(?:-[ \t]*){3,}|(?:\*[ \t]*){3,}|(?:_[ \t]*){3,}|=+[ \t]*)$")
_BULLET = re.compile(r"^\s*[-*+][ \t]+\S")
_ORDERED = re.compile(r"^\s*(\d{1,9})[.)][ \t]+\S")
_TABLE_DELIMITER = re.compile(r"^\s*\|?\s*:?-+:?\s*(?:\|\s*:?-+:?\s*)*\|?\s*$")
_CELL_SPLIT = re.compile(r"(?<!\\)\|")
_CODE_SPAN = re.compile(r"(?<!`)(`+)(?!`).*?(?<!`)\1(?!`)", re.S)


def _is_punctuation(ch: str) -> bool:
    return ch in string.punctuation or unicodedata.category(ch).startswith("P")


def _inline_blocks(text: str) -> Iterator[list[tuple[int, str]]]:
    """The runs of (line number, text) that GitHub parses as one piece of inline text."""
    block: list[tuple[int, str]] = []
    in_list = False
    in_table = False
    fence = ""
    for number, line in enumerate(_LINE_END.split(text), start=1):
        while quote := _QUOTE.match(line):
            line = line[quote.end():]
        if fence:
            close = _FENCE_CLOSE.match(line)
            if close and close.group(1)[0] == fence[0] and len(close.group(1)) >= len(fence):
                fence = ""
            continue
        opening = _FENCE_OPEN.match(line)
        if opening and not (opening.group(1)[0] == "`" and "`" in opening.group(2)):
            if block:
                yield block
            block, in_table, fence = [], False, opening.group(1)
            continue
        if not line.strip() or _BREAK.match(line):
            if block:
                yield block
            block, in_table = [], False
            continue
        if in_table:
            yield from ([(number, cell)] for cell in _CELL_SPLIT.split(line))
            continue
        if _TABLE_DELIMITER.match(line) and len(block) == 1 and "|" in block[0][1]:
            header_number, header = block[0]
            yield from ([(header_number, cell)] for cell in _CELL_SPLIT.split(header))
            block, in_table = [], True
            continue
        if _HEADING.match(line):
            if block:
                yield block
            yield [(number, line)]
            block = []
            continue
        ordered = _ORDERED.match(line)
        # An ordered item interrupts a paragraph only as "1." unless a list is open.
        if _BULLET.match(line) or (ordered and (not block or in_list or ordered.group(1) == "1")):
            if block:
                yield block
            block, in_list = [], True
        elif not block and not line[:1].isspace():
            in_list = False
        block.append((number, line))
    if block:
        yield block


def _strikethrough_pairs(block: list[tuple[int, str]]) -> list[tuple[int, int]]:
    """(opening line, closing line) of each `~` pair GitHub strikes through in a block."""
    joined = "\n".join(line for _, line in block)
    # Code spans keep their length but lose their tildes; like the backtick
    # that ends one, they count as punctuation beside a run.
    joined = _CODE_SPAN.sub(lambda m: re.sub(r"[^\n]", "`", m.group(0)), joined)
    starts: list[int] = []
    offset = 0
    for _, line in block:
        starts.append(offset)
        offset += len(line) + 1

    def line_at(index: int) -> int:
        return block[bisect.bisect_right(starts, index) - 1][0]

    pairs: list[tuple[int, int]] = []
    openers: list[tuple[int, int]] = []
    index = 0
    while index < len(joined):
        ch = joined[index]
        if ch == "\\" and index + 1 < len(joined) and joined[index + 1] in string.punctuation:
            index += 2
            continue
        if ch != "~":
            index += 1
            continue
        end = index
        while end < len(joined) and joined[end] == "~":
            end += 1
        length = end - index
        if length <= 2:
            before = joined[index - 1] if index else "\n"
            after = joined[end] if end < len(joined) else "\n"
            can_open = not after.isspace() and (
                not _is_punctuation(after) or before.isspace() or _is_punctuation(before))
            can_close = not before.isspace() and (
                not _is_punctuation(before) or after.isspace() or _is_punctuation(after))
            closed = False
            if can_close:
                for k in range(len(openers) - 1, -1, -1):
                    if openers[k][0] == length:
                        pairs.append((line_at(openers[k][1]), line_at(index)))
                        del openers[k:]
                        closed = True
                        break
            if can_open and not closed:
                openers.append((length, index))
        index = end
    return pairs


def find_strikethrough_tildes(text: str) -> list[int]:
    """1-based lines holding an unescaped `~` that GitHub pairs into strikethrough."""
    flagged: set[int] = set()
    for block in _inline_blocks(text):
        for opening, closing in _strikethrough_pairs(block):
            flagged.update((opening, closing))
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


_HTML_COMMENT = re.compile(r"<!--.*?-->", re.S)


def has_release_text(notes: str) -> bool:
    """Whether notes say anything beyond headings and HTML comments: an empty
    draft, a bare CHANGELOG heading or a template's comments would publish a
    release page with no notes."""
    visible = _HTML_COMMENT.sub("", notes)
    return any(line.strip() and not _HEADING.match(line) for line in _LINE_END.split(visible))


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
        help="fail instead of publishing a one-line placeholder when no draft or CHANGELOG section exists, "
        "or when the one found has no text beyond headings and comments",
    )
    args = parser.parse_args(argv)

    root = Path(args.repo_root).resolve()
    output = Path(args.output)
    if not output.is_absolute():
        output = (root / output).resolve()
    notes = find_release_notes(root, args.tag)
    if args.strict and notes is not None and not has_release_text(notes):
        print(
            f"The release notes for {args.tag} have no text beyond headings and comments: fill in the draft "
            "or the CHANGELOG.md section",
            file=sys.stderr,
        )
        return 2
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

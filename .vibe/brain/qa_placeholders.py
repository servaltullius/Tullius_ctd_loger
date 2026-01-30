#!/usr/bin/env python3
from __future__ import annotations

import argparse
import re
import textwrap
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path

from context_db import load_config


PH_RE = re.compile(r"<[^>]+>")
XT_TOKEN_RE = re.compile(r"__XT_[A-Z0-9_]+__")


@dataclass
class QaFinding:
    edid: str | None
    rec: str | None
    kind: str
    source: str
    dest: str
    note: str


def _multiset(xs: list[str]) -> dict[str, int]:
    out: dict[str, int] = {}
    for x in xs:
        out[x] = out.get(x, 0) + 1
    return out


def _extract_placeholders(text: str) -> list[str]:
    return PH_RE.findall(text)


def _has_bad_unit_after_mag(dest: str, bad_units: list[str]) -> list[str]:
    hits: list[str] = []
    for u in bad_units:
        if re.search(rf"<mag>\s*{re.escape(u)}", dest, re.IGNORECASE):
            hits.append(u)
    return hits


def _has_bad_unit_after_dur(dest: str, bad_units: list[str]) -> list[str]:
    hits: list[str] = []
    for u in bad_units:
        if re.search(rf"<dur>\s*{re.escape(u)}", dest, re.IGNORECASE):
            hits.append(u)
    return hits


def _parse_xtranslator(path: Path, cfg) -> tuple[int, list[QaFinding]]:
    findings: list[QaFinding] = []
    total = 0

    bad_units = list(cfg.placeholders.get("bad_unit_words_ko") or [])
    mag_forbid = [u for u in bad_units if u in ["초", "분", "시간", "동안", "초당"]]
    dur_forbid = [u for u in bad_units if u in ["포인트", "%"]]

    # xTranslator format uses <String> entries under <Content>.
    for event, elem in ET.iterparse(str(path), events=("end",)):
        if elem.tag != "String":
            continue
        total += 1

        edid = (elem.findtext("EDID") or "").strip() or None
        rec = (elem.findtext("REC") or "").strip() or None
        src = (elem.findtext("Source") or "")
        dst = (elem.findtext("Dest") or "")

        src_ph = _extract_placeholders(src)
        dst_ph = _extract_placeholders(dst)
        if _multiset(src_ph) != _multiset(dst_ph):
            findings.append(
                QaFinding(
                    edid=edid,
                    rec=rec,
                    kind="placeholder_mismatch",
                    source=src,
                    dest=dst,
                    note=f"placeholders differ: src={src_ph} dest={dst_ph}",
                )
            )

        mag_hits = _has_bad_unit_after_mag(dst, mag_forbid)
        if mag_hits:
            findings.append(
                QaFinding(
                    edid=edid,
                    rec=rec,
                    kind="mag_bad_unit",
                    source=src,
                    dest=dst,
                    note=f"'<mag>' followed by: {', '.join(mag_hits)}",
                )
            )

        dur_hits = _has_bad_unit_after_dur(dst, dur_forbid)
        if dur_hits:
            findings.append(
                QaFinding(
                    edid=edid,
                    rec=rec,
                    kind="dur_bad_unit",
                    source=src,
                    dest=dst,
                    note=f"'<dur>' followed by: {', '.join(dur_hits)}",
                )
            )

        elem.clear()

    return total, findings


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Placeholder QA for xTranslator XML.")
    parser.add_argument("xml_path")
    parser.add_argument("--out", default=".vibe/reports/placeholder_qa.md")
    parser.add_argument("--limit", type=int, default=80)
    args = parser.parse_args(argv)

    cfg = load_config()
    path = Path(args.xml_path)
    if not path.is_absolute():
        path = cfg.root / path
    if not path.exists():
        print(f"[qa] not found: {args.xml_path}")
        return 2

    total, findings = _parse_xtranslator(path, cfg)
    findings.sort(key=lambda f: (f.kind, f.rec or "", f.edid or ""))

    out_path = cfg.root / args.out
    out_path.parent.mkdir(parents=True, exist_ok=True)

    lines: list[str] = []
    lines.append(f"# Placeholder QA\n")
    lines.append(f"- File: `{path}`")
    lines.append(f"- Strings scanned: {total}")
    lines.append(f"- Findings: {len(findings)}\n")
    for f in findings[: args.limit]:
        head = f"- [{f.kind}] {f.rec or ''} {f.edid or ''}".strip()
        lines.append(head)
        lines.append(textwrap.indent(f.note, "  - "))
        lines.append(textwrap.indent(f"EN: {f.source}", "  - "))
        lines.append(textwrap.indent(f"KO: {f.dest}", "  - "))
        lines.append("")
    if len(findings) > args.limit:
        lines.append(f"... truncated: {len(findings) - args.limit} more\n")

    out_path.write_text("\n".join(lines), encoding="utf-8")
    print(f"[qa] wrote: {out_path} (findings={len(findings)})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(__import__("sys").argv[1:]))

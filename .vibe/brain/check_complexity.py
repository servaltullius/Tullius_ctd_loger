#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import re
from dataclasses import dataclass
from pathlib import Path

from context_db import VibeConfig, is_excluded, load_config, normalize_rel


METHOD_START_RE = re.compile(
    r"^\s*(public|internal|protected|private)\s+"
    r"(?:static\s+|async\s+|virtual\s+|override\s+|sealed\s+|new\s+|unsafe\s+|extern\s+|partial\s+)*"
    r"(?!class\b)(?!struct\b)(?!record\b)(?!interface\b)(?!enum\b)"
    r"[A-Za-z_][A-Za-z0-9_<>,\[\]\?\.\s]*\s+"
    r"[A-Za-z_][A-Za-z0-9_]*\s*\(",
)


@dataclass
class ComplexityFinding:
    file: str
    line: int
    name: str
    lines: int
    nesting: int
    params: int
    score: int


def _iter_cs_files(cfg: VibeConfig) -> list[Path]:
    root = cfg.root
    out: list[Path] = []
    for pattern in cfg.include_globs:
        if not pattern.endswith(".cs"):
            continue
        for p in root.glob(pattern):
            if not p.is_file():
                continue
            rel = p.relative_to(root)
            if is_excluded(rel, cfg.exclude_dirs):
                continue
            out.append(p)
    uniq = {p.resolve(): p for p in out}
    return sorted(uniq.values(), key=lambda x: x.as_posix())


def _count_params(signature: str) -> int:
    inner = signature.split("(", 1)[1]
    inner = inner.rsplit(")", 1)[0]
    if not inner.strip():
        return 0
    # Split only on top-level commas. C# signatures can include commas inside:
    # - generics: Foo<Bar, Baz>
    # - tuples: (int A, string B)
    # - attributes/defaults: [Attr("a,b")] = "x,y"
    params: list[str] = []
    cur: list[str] = []
    angle = 0
    paren = 0
    bracket = 0
    brace = 0
    in_string: str | None = None
    escape = False

    for ch in inner:
        if in_string is not None:
            cur.append(ch)
            if escape:
                escape = False
                continue
            if ch == "\\":
                escape = True
                continue
            if ch == in_string:
                in_string = None
            continue

        if ch in ('"', "'"):
            in_string = ch
            cur.append(ch)
            continue

        if ch == "<":
            angle += 1
        elif ch == ">":
            angle = max(0, angle - 1)
        elif ch == "(":
            paren += 1
        elif ch == ")":
            paren = max(0, paren - 1)
        elif ch == "[":
            bracket += 1
        elif ch == "]":
            bracket = max(0, bracket - 1)
        elif ch == "{":
            brace += 1
        elif ch == "}":
            brace = max(0, brace - 1)

        if ch == "," and angle == 0 and paren == 0 and bracket == 0 and brace == 0:
            part = "".join(cur).strip()
            if part:
                params.append(part)
            cur = []
            continue

        cur.append(ch)

    part = "".join(cur).strip()
    if part:
        params.append(part)

    return len(params)


def _analyze_file(path: Path, cfg: VibeConfig) -> list[ComplexityFinding]:
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        text = path.read_text(encoding="utf-8", errors="ignore")

    lines = text.splitlines()
    findings: list[ComplexityFinding] = []
    i = 0
    while i < len(lines):
        line = lines[i]
        if not METHOD_START_RE.match(line):
            i += 1
            continue
        # Avoid false positives for record primary constructors, e.g.:
        #   private sealed record Foo(int A, int B);
        if re.search(
            r"^\s*(public|internal|protected|private)\s+.*\brecord\b\s+(?:struct\s+)?[A-Za-z_][A-Za-z0-9_]*\s*\(",
            line,
        ):
            i += 1
            continue

        start_line = i + 1
        sig_lines = [line.strip()]
        j = i
        # gather multi-line signature until '{' or '=>'
        while j + 1 < len(lines) and ("{" not in lines[j]) and ("=>" not in lines[j]):
            if "(" in "".join(sig_lines) and ")" in lines[j]:
                # likely end of params on this line; keep going if no brace yet
                pass
            j += 1
            sig_lines.append(lines[j].strip())
            if "{" in lines[j] or "=>" in lines[j]:
                break

        signature = " ".join(sig_lines)
        params = _count_params(signature) if "(" in signature and ")" in signature else 0

        # name heuristic: token before '('
        name = signature.split("(", 1)[0].strip().split()[-1]

        if "=>" in signature and "{" not in signature:
            # expression-bodied member
            length = 1
            nesting = 0
            i = j + 1
        else:
            # find block end by brace balance starting at first '{'
            k = j
            brace = 0
            max_brace = 0
            started = False
            while k < len(lines):
                brace += lines[k].count("{")
                if lines[k].count("{"):
                    started = True
                brace -= lines[k].count("}")
                max_brace = max(max_brace, brace)
                k += 1
                if started and brace <= 0:
                    break
            end_line = k
            length = max(1, end_line - i)
            nesting = max(0, max_brace - 1)
            i = k

        max_method_lines = int(cfg.quality_gates.get("max_method_lines_warn", 50))
        max_nesting = int(cfg.quality_gates.get("max_nesting_warn", 4))
        max_params = int(cfg.quality_gates.get("max_params_warn", 5))

        score = int(length / 10) + (nesting * 3) + max(0, params - 2)
        if length > max_method_lines or nesting > max_nesting or params > max_params:
            rel = normalize_rel(path.relative_to(cfg.root))
            findings.append(
                ComplexityFinding(
                    file=rel,
                    line=start_line,
                    name=name,
                    lines=length,
                    nesting=nesting,
                    params=params,
                    score=score,
                )
            )

    return findings


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Heuristic complexity warnings (C#).")
    parser.add_argument("--out", default=".vibe/reports/complexity.json")
    parser.add_argument(
        "--files",
        action="append",
        default=None,
        help="Analyze only specific repo-relative files (repeatable).",
    )
    args = parser.parse_args(argv)

    cfg = load_config()
    results: list[ComplexityFinding] = []
    if args.files:
        for f in args.files:
            p = Path(f)
            if not p.is_absolute():
                p = cfg.root / p
            if p.exists() and p.suffix.lower() == ".cs":
                results.extend(_analyze_file(p, cfg))
    else:
        for p in _iter_cs_files(cfg):
            results.extend(_analyze_file(p, cfg))

    results.sort(key=lambda r: (-r.score, -r.lines))

    out_path = cfg.root / args.out
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(
        json.dumps([r.__dict__ for r in results], ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )

    top = results[:10]
    if top:
        print("Complexity warnings (top 10):")
        for r in top:
            print(f"- {r.file}:{r.line} {r.name} (lines={r.lines}, nesting={r.nesting}, params={r.params}, score={r.score})")
    else:
        print("No complexity warnings.")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(__import__("sys").argv[1:]))

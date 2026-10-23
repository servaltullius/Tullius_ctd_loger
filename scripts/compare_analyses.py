#!/usr/bin/env python3
"""Compare two analyzer builds on the same dumps: what a change did to the output.

Runs a baseline and a new SkyrimDiagDumpToolCli.exe on every dump (Korean and
English by default) and lists, per dump and language, the fields that changed:
the conclusion, freeze state and confidence, bucket, known signature, quick
answer, suspects, actionable candidates, recommendations and their groups,
the faulting instruction, and summary keys that appeared or disappeared.
Everything else in a summary (evidence details, events, paths, the analysis
time) is left out on purpose: those move with every run.

  python scripts/compare_analyses.py --dumps <dir or .dmp> [...] \
      --base-cli <old build-win/bin/SkyrimDiagDumpToolCli.exe> \
      --new-cli build-win/bin/SkyrimDiagDumpToolCli.exe [--lang ko en] [--jobs 2]

Dumps and the summaries written for them carry mod lists and local paths:
point --dumps at them where they are and keep --out outside the repo (the
default is a new folder under the system temp directory, kept so the full
summaries and reports can be opened; remove it yourself afterwards).
Exit code: 0, or 1 with --fail-on-diff when anything changed.
"""
from __future__ import annotations

import argparse
import json
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
from accuracy_benchmark import run_analyzer  # noqa: E402

QUICK_ANSWER_FIELDS = (
    "case", "suspect_mod", "suspect_file", "likelihood", "headline", "action", "why_not_certain", "how_it_crashed",
)


def _get(obj: Any, *path: str) -> Any:
    for key in path:
        if not isinstance(obj, dict):
            return None
        obj = obj.get(key)
    return obj


def key_fields(summary: dict[str, Any]) -> dict[str, Any]:
    """The fields a reader acts on, in a comparable form."""
    fields: dict[str, Any] = {
        "summary_sentence": summary.get("summary_sentence"),
        "crash_bucket_key": summary.get("crash_bucket_key"),
        "signature": _get(summary, "signature_match", "id"),
        "freeze_state": _get(summary, "freeze_analysis", "state_id"),
        "freeze_confidence": _get(summary, "freeze_analysis", "confidence"),
        "fault_location": _get(summary, "exception", "module_plus_offset"),
        "fault_instruction": _get(summary, "fault_instruction", "verdict"),
    }
    for name in QUICK_ANSWER_FIELDS:
        fields[f"quick_answer.{name}"] = _get(summary, "quick_answer", name)
    fields["suspects"] = [
        f"{s.get('module_filename', '')} ({s.get('confidence', '')})"
        for s in summary.get("suspects") or [] if isinstance(s, dict)
    ][:5]
    fields["actionable_candidates"] = [
        f"{c.get('display_name', '')} [{c.get('status_id', '')}] ({c.get('confidence', '')})"
        for c in summary.get("actionable_candidates") or [] if isinstance(c, dict)
    ][:5]
    fields["recommendations"] = list(summary.get("recommendations") or [])
    fields["recommendation_groups"] = [
        c.get("group", "") for c in summary.get("recommendation_classes") or [] if isinstance(c, dict)
    ]
    return fields


def key_set(obj: Any) -> set[str]:
    return set(obj) if isinstance(obj, dict) else set()


def compare(base: dict[str, Any], new: dict[str, Any]) -> list[str]:
    """Changed fields as readable lines; empty when the two read the same."""
    lines: list[str] = []
    a, b = key_fields(base), key_fields(new)
    for name in a:
        if a[name] == b[name]:
            continue
        if isinstance(a[name], list) and isinstance(b[name], list) and name != "recommendation_groups":
            gone = [x for x in a[name] if x not in b[name]]
            added = [x for x in b[name] if x not in a[name]]
            if not gone and not added:
                lines.append(f"{name}: same entries, new order")
            for x in gone:
                lines.append(f"{name} - {x}")
            for x in added:
                lines.append(f"{name} + {x}")
        else:
            lines.append(f"{name}: {a[name]!r} -> {b[name]!r}")
    for label, old_keys, new_keys in (
        ("summary keys", key_set(base), key_set(new)),
        ("freeze_analysis keys", key_set(base.get("freeze_analysis")), key_set(new.get("freeze_analysis"))),
    ):
        for key in sorted(new_keys - old_keys):
            lines.append(f"{label} + {key}")
        for key in sorted(old_keys - new_keys):
            lines.append(f"{label} - {key}")
    return lines


def only_keys(lines: list[str]) -> bool:
    """Only keys appeared or went (an additive field), nothing a reader sees changed."""
    return bool(lines) and all(line.startswith(("summary keys ", "freeze_analysis keys ")) for line in lines)


def find_dumps(paths: list[Path]) -> list[Path]:
    dumps: list[Path] = []
    for path in paths:
        if path.is_dir():
            dumps.extend(sorted(path.glob("*.dmp")))
        elif path.suffix.lower() == ".dmp" and path.is_file():
            dumps.append(path)
        else:
            raise SystemExit(f"not a dump or a folder of dumps: {path}")
    return dumps


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dumps", type=Path, nargs="+", required=True, help="dump files or folders of dumps")
    parser.add_argument("--base-cli", type=Path, required=True, help="baseline SkyrimDiagDumpToolCli.exe")
    parser.add_argument("--new-cli", type=Path, required=True, help="changed SkyrimDiagDumpToolCli.exe")
    parser.add_argument("--lang", nargs="+", default=["ko", "en"], choices=("ko", "en"))
    parser.add_argument("--jobs", type=int, default=2, help="analyses run at once (each maps a whole dump)")
    parser.add_argument("--out", type=Path, help="where the summaries go (default: a new temp folder)")
    parser.add_argument("--fail-on-diff", action="store_true")
    args = parser.parse_args(argv)

    dumps = find_dumps(args.dumps)
    if not dumps:
        raise SystemExit("no dumps found")
    for cli in (args.base_cli, args.new_cli):
        if not cli.is_file():
            raise SystemExit(f"analyzer not found: {cli}")
    out = args.out or Path(tempfile.mkdtemp(prefix="sdcmp-"))

    jobs = [(side, cli, lang, dump)
            for dump in dumps for lang in args.lang
            for side, cli in (("base", args.base_cli), ("new", args.new_cli))]

    def run(job: tuple[str, Path, str, Path]) -> tuple[tuple[str, str, str], dict[str, Any]]:
        side, cli, lang, dump = job
        summary = run_analyzer([str(cli)], dump, out / side / lang / dump.stem, with_crash_logger=True, lang=lang)
        return (side, lang, dump.stem), summary

    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        results = dict(pool.map(run, jobs))

    changed = keys_only = 0
    for dump in dumps:
        for lang in args.lang:
            lines = compare(results[("base", lang, dump.stem)], results[("new", lang, dump.stem)])
            state = "same" if not lines else ("keys only" if only_keys(lines) else f"{len(lines)} change(s)")
            print(f"{lang} {dump.stem}: {state}")
            for line in lines:
                print(f"    {line}")
            keys_only += only_keys(lines)
            changed += bool(lines) and not only_keys(lines)
    total = len(dumps) * len(args.lang)
    print(f"\n{changed} of {total} analyses changed, {keys_only} only gained or lost keys; summaries in {out}")
    return 1 if args.fail_on_diff and (changed or keys_only) else 0


if __name__ == "__main__":
    raise SystemExit(main())

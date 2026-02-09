#!/usr/bin/env python3
"""Aggregate SkyrimDiag summary files to track crash-bucket quality over time."""

from __future__ import annotations

import argparse
import json
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Any


def _normalize(s: str) -> str:
    return " ".join(s.strip().lower().split())


def _is_reviewed(triage: dict[str, Any]) -> bool:
    status = _normalize(str(triage.get("review_status", "")))
    if status in {"reviewed", "confirmed", "triaged", "done"}:
        return True
    return bool(_normalize(str(triage.get("ground_truth_mod", ""))))


def _is_unknown_module(summary: dict[str, Any]) -> bool:
    exc = summary.get("exception", {})
    if not isinstance(exc, dict):
        return True
    module_plus_offset = _normalize(str(exc.get("module_plus_offset", "")))
    return module_plus_offset in {"", "unknown", "<unknown>", "n/a", "none"}


@dataclass
class BucketStats:
    total: int = 0
    reviewed: int = 0
    unknown_fault_module: int = 0
    gt_with_mod: int = 0
    gt_top1_match_by_mod_name: int = 0
    gt_top1_match_by_module_filename: int = 0
    gt_top1_match: int = 0


def _top1_suspect(suspects: list[Any]) -> dict[str, Any] | None:
    if not suspects:
        return None
    top = suspects[0]
    if isinstance(top, dict):
        return top
    return None


def _top1_match_mode(top: dict[str, Any] | None, ground_truth_mod: str) -> str | None:
    if top is None:
        return None
    gt = _normalize(ground_truth_mod)
    if not gt:
        return None

    inferred_mod_name = _normalize(str(top.get("inferred_mod_name", "")))
    if inferred_mod_name and inferred_mod_name == gt:
        return "mod_name"

    module_filename = _normalize(str(top.get("module_filename", "")))
    if module_filename and module_filename == gt:
        return "module_filename"
    return None


def _read_json(path: Path) -> dict[str, Any] | None:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return None


def _iter_summary_paths(root: Path, recursive: bool, pattern: str) -> list[Path]:
    globber = root.rglob if recursive else root.glob
    return sorted(p for p in globber(pattern) if p.is_file())


def aggregate(root: Path, recursive: bool, pattern: str) -> dict[str, Any]:
    files = _iter_summary_paths(root, recursive=recursive, pattern=pattern)
    buckets: dict[str, BucketStats] = {}
    parsed_files = 0

    for path in files:
        summary = _read_json(path)
        if not isinstance(summary, dict):
            continue
        parsed_files += 1

        crash_bucket_key = str(summary.get("crash_bucket_key", "")).strip() or "__missing_bucket__"
        triage = summary.get("triage", {})
        if not isinstance(triage, dict):
            triage = {}
        suspects = summary.get("suspects", [])
        if not isinstance(suspects, list):
            suspects = []

        row = buckets.setdefault(crash_bucket_key, BucketStats())
        row.total += 1
        if _is_unknown_module(summary):
            row.unknown_fault_module += 1

        if _is_reviewed(triage):
            row.reviewed += 1
            ground_truth_mod = _normalize(str(triage.get("ground_truth_mod", "")))
            if ground_truth_mod:
                row.gt_with_mod += 1
                mode = _top1_match_mode(_top1_suspect(suspects), ground_truth_mod)
                if mode == "mod_name":
                    row.gt_top1_match_by_mod_name += 1
                    row.gt_top1_match += 1
                elif mode == "module_filename":
                    row.gt_top1_match_by_module_filename += 1
                    row.gt_top1_match += 1

    bucket_rows: list[dict[str, Any]] = []
    for bucket, stats in buckets.items():
        top1_precision = (stats.gt_top1_match / stats.gt_with_mod) if stats.gt_with_mod else None
        unknown_rate = (stats.unknown_fault_module / stats.total) if stats.total else 0.0
        bucket_rows.append(
            {
                "crash_bucket_key": bucket,
                **asdict(stats),
                "unknown_rate": unknown_rate,
                "top1_precision_vs_ground_truth_mod": top1_precision,
            }
        )

    bucket_rows.sort(key=lambda x: (-int(x["total"]), str(x["crash_bucket_key"])))
    reviewed_total = sum(int(r["reviewed"]) for r in bucket_rows)
    gt_with_mod_total = sum(int(r["gt_with_mod"]) for r in bucket_rows)
    gt_top1_match_total = sum(int(r["gt_top1_match"]) for r in bucket_rows)

    return {
        "input_root": str(root),
        "pattern": pattern,
        "recursive": recursive,
        "files_found": len(files),
        "files_parsed": parsed_files,
        "bucket_count": len(bucket_rows),
        "reviewed_total": reviewed_total,
        "gt_with_mod_total": gt_with_mod_total,
        "gt_top1_match_total": gt_top1_match_total,
        "overall_top1_precision_vs_ground_truth_mod": (
            (gt_top1_match_total / gt_with_mod_total) if gt_with_mod_total else None
        ),
        "buckets": bucket_rows,
    }


def _print_table(report: dict[str, Any], top: int) -> None:
    print("SkyrimDiag bucket quality report")
    print(f"root={report['input_root']} parsed={report['files_parsed']}/{report['files_found']} buckets={report['bucket_count']}")
    print(
        "overall_top1_precision_vs_ground_truth_mod="
        f"{report['overall_top1_precision_vs_ground_truth_mod']}"
    )
    print("")
    print("Top buckets:")
    print("count reviewed unknown_rate top1_precision bucket")
    for row in report["buckets"][:top]:
        count = row["total"]
        reviewed = row["reviewed"]
        unknown_rate = f"{row['unknown_rate']:.2f}"
        precision = row["top1_precision_vs_ground_truth_mod"]
        precision_text = "-" if precision is None else f"{precision:.2f}"
        bucket = row["crash_bucket_key"]
        print(f"{count:5d} {reviewed:8d} {unknown_rate:11s} {precision_text:13s} {bucket}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Aggregate SkyrimDiag crash bucket quality from summary JSON files.")
    parser.add_argument(
        "--root",
        type=Path,
        default=Path("."),
        help="Directory containing *_SkyrimDiagSummary.json files (default: current directory)",
    )
    parser.add_argument(
        "--pattern",
        default="*_SkyrimDiagSummary.json",
        help="Glob pattern for summary files (default: *_SkyrimDiagSummary.json)",
    )
    parser.add_argument(
        "--non-recursive",
        action="store_true",
        help="Search only the root directory (default: recursive)",
    )
    parser.add_argument(
        "--top",
        type=int,
        default=20,
        help="How many top buckets to print (default: 20)",
    )
    parser.add_argument(
        "--out-json",
        type=Path,
        help="Optional output path to save the aggregated report JSON",
    )
    args = parser.parse_args()

    report = aggregate(
        root=args.root,
        recursive=not args.non_recursive,
        pattern=args.pattern,
    )
    _print_table(report, top=max(1, args.top))

    if args.out_json:
        args.out_json.parent.mkdir(parents=True, exist_ok=True)
        args.out_json.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        print(f"\nWrote: {args.out_json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Aggregate SkyrimDiag summary files to track crash-bucket quality over time."""

from __future__ import annotations

import argparse
import json
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Any


def _normalize(value: Any) -> str:
    # Triage/candidate fields are string-typed. Treat JSON null, numbers and
    # objects as missing instead of converting them to matchable text such as
    # "None", which could fabricate ground-truth hits.
    if not isinstance(value, str):
        return ""
    return " ".join(value.strip().lower().split())


def _is_reviewed(triage: dict[str, Any]) -> bool:
    status = _normalize(triage.get("review_status", ""))
    if status in {"reviewed", "confirmed", "triaged", "done"}:
        return True
    if _normalize(triage.get("ground_truth_mod", "")):
        return True
    if _normalize(triage.get("actual_cause", "")):
        return True
    if _normalize(triage.get("verdict", "")):
        return True
    return bool(_normalize(triage.get("notes", "")))


def _is_unknown_module(summary: dict[str, Any]) -> bool:
    exc = summary.get("exception", {})
    if not isinstance(exc, dict):
        return True
    module_plus_offset = _normalize(exc.get("module_plus_offset", ""))
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
    gt_top3_match: int = 0
    gt_top1_legacy_suspect_match: int = 0
    high_confidence_predictions: int = 0
    high_confidence_matches: int = 0
    abstained_with_ground_truth: int = 0


def _dict_rows(value: Any) -> list[dict[str, Any]]:
    if not isinstance(value, list):
        return []
    return [row for row in value if isinstance(row, dict)]


def _user_visible_candidates(summary: dict[str, Any]) -> list[dict[str, Any]]:
    """Return candidates in the same priority order used by summary/WinUI.

    The product shows up to five actionable candidates first. When that list is
    empty, WinUI shows up to three Crash Logger object references and fills the
    remaining seven visible slots with raw suspects. Quality metrics must grade
    that same user-visible decision rather than an internal intermediate list.
    """
    actionable = _dict_rows(summary.get("actionable_candidates", []))
    if actionable:
        return actionable[:5]

    crash_logger = summary.get("crash_logger", {})
    object_refs = _dict_rows(
        crash_logger.get("object_refs", []) if isinstance(crash_logger, dict) else []
    )[:3]
    visible: list[dict[str, Any]] = []
    for ref in object_refs:
        esp_name = ref.get("esp_name", "")
        try:
            relevance_score = int(ref.get("relevance_score", 0))
        except (TypeError, ValueError):
            relevance_score = 0
        visible.append(
            {
                **ref,
                "plugin_name": esp_name,
                "display_name": esp_name,
                # WinUI labels score >= 16 as "ESP ref (high)".
                "confidence": "High" if relevance_score >= 16 else "",
            }
        )

    suspects = _dict_rows(summary.get("suspects", []))
    visible.extend(suspects[: max(0, 7 - len(visible))])
    return visible


def _candidate_field_values(candidate: dict[str, Any]) -> list[tuple[str, str]]:
    fields = (
        "mod_name",
        "inferred_mod_name",
        "module_filename",
        "plugin_name",
        "primary_identifier",
        "display_name",
    )
    return [(field, _normalize(candidate.get(field, ""))) for field in fields]


def _candidate_match_mode(candidate: dict[str, Any] | None, ground_truth_mod: str) -> str | None:
    if candidate is None:
        return None
    gt = _normalize(ground_truth_mod)
    if not gt:
        return None

    for field, value in _candidate_field_values(candidate):
        if value and value == gt:
            if field in {"mod_name", "inferred_mod_name"}:
                return "mod_name"
            return "module_filename"
    return None


def _first_match_mode(candidates: list[dict[str, Any]], ground_truth_mod: str, limit: int) -> str | None:
    for candidate in candidates[:limit]:
        mode = _candidate_match_mode(candidate, ground_truth_mod)
        if mode is not None:
            return mode
    return None


def _is_high_confidence(candidate: dict[str, Any] | None) -> bool:
    if candidate is None:
        return False
    confidence = _normalize(candidate.get("confidence", ""))
    # Summary confidence text is localized by the analyzer language.
    return confidence in {"high", "높음"}


def _read_json(path: Path) -> dict[str, Any] | None:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except Exception:
        return None


def _iter_summary_paths(root: Path, recursive: bool, pattern: str) -> list[Path]:
    globber = root.rglob if recursive else root.glob
    return sorted(p for p in globber(pattern) if p.is_file())


def _incident_identity(summary: dict[str, Any]) -> str | None:
    incident = summary.get("incident", {})
    if isinstance(incident, dict):
        incident_id = _normalize(incident.get("incident_id", ""))
        if incident_id:
            return f"incident:{incident_id}"

    dump_path = summary.get("dump_path", "")
    if isinstance(dump_path, str) and dump_path.strip():
        normalized_path = "/".join(part for part in dump_path.replace("\\", "/").casefold().split("/") if part)
        if normalized_path:
            return f"dump:{normalized_path}"
    return None


def _triage_dict(summary: dict[str, Any]) -> dict[str, Any]:
    triage = summary.get("triage", {})
    return triage if isinstance(triage, dict) else {}


def _summary_preference(path: Path, summary: dict[str, Any]) -> tuple[int, int, str, str]:
    triage = _triage_dict(summary)
    return (
        int(bool(_normalize(triage.get("ground_truth_mod", "")))),
        int(_is_reviewed(triage)),
        _normalize(triage.get("reviewed_at_utc", "")),
        str(path).casefold(),
    )


def _prediction_fingerprint(summary: dict[str, Any]) -> tuple[tuple[str, ...], ...]:
    rows: list[tuple[str, ...]] = []
    for candidate in _user_visible_candidates(summary)[:3]:
        values = tuple(value for _, value in _candidate_field_values(candidate))
        rows.append((*values, _normalize(candidate.get("confidence", ""))))
    return tuple(rows)


def _select_unique_incidents(
    records: list[tuple[Path, dict[str, Any]]],
) -> tuple[list[tuple[Path, dict[str, Any]]], int, int, int, int]:
    without_identity: list[tuple[Path, dict[str, Any]]] = []
    groups: dict[str, list[tuple[Path, dict[str, Any]]]] = {}
    for record in records:
        identity = _incident_identity(record[1])
        if identity is None:
            without_identity.append(record)
        else:
            groups.setdefault(identity, []).append(record)

    selected = list(without_identity)
    duplicate_files_skipped = 0
    duplicate_identity_count = 0
    conflicting_ground_truth_count = 0
    conflicting_prediction_count = 0
    for group in groups.values():
        selected.append(max(group, key=lambda record: _summary_preference(record[0], record[1])))
        if len(group) <= 1:
            continue
        duplicate_identity_count += 1
        duplicate_files_skipped += len(group) - 1
        truths = {
            _normalize(_triage_dict(summary).get("ground_truth_mod", ""))
            for _, summary in group
        }
        truths.discard("")
        if len(truths) > 1:
            conflicting_ground_truth_count += 1
        predictions = {_prediction_fingerprint(summary) for _, summary in group}
        if len(predictions) > 1:
            conflicting_prediction_count += 1

    selected.sort(key=lambda record: str(record[0]).casefold())
    return (
        selected,
        duplicate_files_skipped,
        duplicate_identity_count,
        conflicting_ground_truth_count,
        conflicting_prediction_count,
    )


def aggregate(root: Path, recursive: bool, pattern: str) -> dict[str, Any]:
    files = _iter_summary_paths(root, recursive=recursive, pattern=pattern)
    buckets: dict[str, BucketStats] = {}
    parsed_files = 0
    parsed_records: list[tuple[Path, dict[str, Any]]] = []

    for path in files:
        summary = _read_json(path)
        if not isinstance(summary, dict):
            continue
        parsed_files += 1
        parsed_records.append((path, summary))

    (
        unique_records,
        duplicate_files_skipped,
        duplicate_identity_count,
        conflicting_ground_truth_count,
        conflicting_prediction_count,
    ) = _select_unique_incidents(parsed_records)

    for path, summary in unique_records:
        raw_bucket_key = summary.get("crash_bucket_key", "")
        crash_bucket_key = (
            raw_bucket_key.strip()
            if isinstance(raw_bucket_key, str) and raw_bucket_key.strip()
            else "__missing_bucket__"
        )
        triage = _triage_dict(summary)
        suspects = _dict_rows(summary.get("suspects", []))
        user_candidates = _user_visible_candidates(summary)

        row = buckets.setdefault(crash_bucket_key, BucketStats())
        row.total += 1
        if _is_unknown_module(summary):
            row.unknown_fault_module += 1

        if _is_reviewed(triage):
            row.reviewed += 1
            ground_truth_mod = _normalize(triage.get("ground_truth_mod", ""))
            if ground_truth_mod:
                row.gt_with_mod += 1
                top = user_candidates[0] if user_candidates else None
                mode = _candidate_match_mode(top, ground_truth_mod)
                if mode == "mod_name":
                    row.gt_top1_match_by_mod_name += 1
                    row.gt_top1_match += 1
                elif mode == "module_filename":
                    row.gt_top1_match_by_module_filename += 1
                    row.gt_top1_match += 1
                if _first_match_mode(user_candidates, ground_truth_mod, limit=3) is not None:
                    row.gt_top3_match += 1
                legacy_top = suspects[0] if suspects else None
                if _candidate_match_mode(legacy_top, ground_truth_mod) is not None:
                    row.gt_top1_legacy_suspect_match += 1
                if top is None:
                    row.abstained_with_ground_truth += 1
                elif _is_high_confidence(top):
                    row.high_confidence_predictions += 1
                    if mode is not None:
                        row.high_confidence_matches += 1

    bucket_rows: list[dict[str, Any]] = []
    for bucket, stats in buckets.items():
        top1_precision = (stats.gt_top1_match / stats.gt_with_mod) if stats.gt_with_mod else None
        top3_recall = (stats.gt_top3_match / stats.gt_with_mod) if stats.gt_with_mod else None
        high_precision = (
            stats.high_confidence_matches / stats.high_confidence_predictions
            if stats.high_confidence_predictions
            else None
        )
        abstention_rate = (
            stats.abstained_with_ground_truth / stats.gt_with_mod if stats.gt_with_mod else None
        )
        unknown_rate = (stats.unknown_fault_module / stats.total) if stats.total else 0.0
        bucket_rows.append(
            {
                "crash_bucket_key": bucket,
                **asdict(stats),
                "unknown_rate": unknown_rate,
                "top1_precision_vs_ground_truth_mod": top1_precision,
                "top3_recall_vs_ground_truth_mod": top3_recall,
                "high_confidence_precision_vs_ground_truth_mod": high_precision,
                "abstention_rate_with_ground_truth_mod": abstention_rate,
            }
        )

    bucket_rows.sort(key=lambda x: (-int(x["total"]), str(x["crash_bucket_key"])))
    reviewed_total = sum(int(r["reviewed"]) for r in bucket_rows)
    gt_with_mod_total = sum(int(r["gt_with_mod"]) for r in bucket_rows)
    gt_top1_match_total = sum(int(r["gt_top1_match"]) for r in bucket_rows)
    gt_top3_match_total = sum(int(r["gt_top3_match"]) for r in bucket_rows)
    high_confidence_predictions_total = sum(int(r["high_confidence_predictions"]) for r in bucket_rows)
    high_confidence_matches_total = sum(int(r["high_confidence_matches"]) for r in bucket_rows)
    abstained_with_ground_truth_total = sum(int(r["abstained_with_ground_truth"]) for r in bucket_rows)
    predicted_with_ground_truth_total = gt_with_mod_total - abstained_with_ground_truth_total
    top1_accuracy = (gt_top1_match_total / gt_with_mod_total) if gt_with_mod_total else None

    return {
        "input_root": str(root),
        "pattern": pattern,
        "recursive": recursive,
        "files_found": len(files),
        "files_parsed": parsed_files,
        "unique_incidents_analyzed": len(unique_records),
        "duplicate_files_skipped": duplicate_files_skipped,
        "duplicate_identity_count": duplicate_identity_count,
        "duplicate_conflicting_ground_truth_count": conflicting_ground_truth_count,
        "duplicate_conflicting_prediction_count": conflicting_prediction_count,
        "bucket_count": len(bucket_rows),
        "reviewed_total": reviewed_total,
        "gt_with_mod_total": gt_with_mod_total,
        "gt_top1_match_total": gt_top1_match_total,
        "gt_top3_match_total": gt_top3_match_total,
        "high_confidence_predictions_total": high_confidence_predictions_total,
        "high_confidence_matches_total": high_confidence_matches_total,
        "abstained_with_ground_truth_total": abstained_with_ground_truth_total,
        "predicted_with_ground_truth_total": predicted_with_ground_truth_total,
        # Kept for report compatibility. Since abstentions are included in its
        # denominator, this historical field is an accuracy/success rate rather
        # than precision conditional on making a prediction.
        "overall_top1_precision_vs_ground_truth_mod": top1_accuracy,
        "overall_top1_accuracy_vs_ground_truth_mod": top1_accuracy,
        "overall_top1_precision_when_predicted": (
            (gt_top1_match_total / predicted_with_ground_truth_total)
            if predicted_with_ground_truth_total
            else None
        ),
        "overall_top3_recall_vs_ground_truth_mod": (
            (gt_top3_match_total / gt_with_mod_total) if gt_with_mod_total else None
        ),
        "overall_high_confidence_precision_vs_ground_truth_mod": (
            (high_confidence_matches_total / high_confidence_predictions_total)
            if high_confidence_predictions_total
            else None
        ),
        "overall_abstention_rate_with_ground_truth_mod": (
            (abstained_with_ground_truth_total / gt_with_mod_total) if gt_with_mod_total else None
        ),
        "buckets": bucket_rows,
    }


def _rate_arg(value: str) -> float:
    parsed = float(value)
    if not 0.0 <= parsed <= 1.0:
        raise argparse.ArgumentTypeError("rate thresholds must be between 0 and 1")
    return parsed


def evaluate_quality_gate(
    report: dict[str, Any],
    *,
    min_ground_truth: int | None,
    min_high_confidence_predictions: int | None,
    min_top1_accuracy: float | None,
    min_top3_recall: float | None,
    min_high_confidence_precision: float | None,
    max_abstention_rate: float | None,
) -> dict[str, Any]:
    """Evaluate optional release thresholds without inventing missing evidence.

    A requested metric with no eligible samples fails closed. This prevents an
    empty or tiny corpus from being reported as an accuracy pass.
    """
    thresholds = {
        "min_ground_truth": min_ground_truth,
        "min_high_confidence_predictions": min_high_confidence_predictions,
        "min_top1_accuracy": min_top1_accuracy,
        "min_top3_recall": min_top3_recall,
        "min_high_confidence_precision": min_high_confidence_precision,
        "max_abstention_rate": max_abstention_rate,
    }
    configured = any(value is not None for value in thresholds.values())
    failures: list[str] = []

    conflicting_duplicates = int(report.get("duplicate_conflicting_ground_truth_count", 0))
    if configured and conflicting_duplicates:
        failures.append(
            f"duplicate incidents have conflicting ground truth: {conflicting_duplicates}"
        )
    conflicting_predictions = int(report.get("duplicate_conflicting_prediction_count", 0))
    if configured and conflicting_predictions:
        failures.append(
            f"duplicate incidents have conflicting analyzer predictions: {conflicting_predictions}"
        )

    def require_count(field: str, minimum: int | None, label: str) -> None:
        if minimum is None:
            return
        actual = int(report.get(field, 0))
        if actual < minimum:
            failures.append(f"{label}={actual} is below required {minimum}")

    def require_min(field: str, minimum: float | None, label: str) -> None:
        if minimum is None:
            return
        actual = report.get(field)
        if actual is None:
            failures.append(f"{label} is unavailable (no eligible samples)")
        elif float(actual) < minimum:
            failures.append(f"{label}={float(actual):.4f} is below required {minimum:.4f}")

    def require_max(field: str, maximum: float | None, label: str) -> None:
        if maximum is None:
            return
        actual = report.get(field)
        if actual is None:
            failures.append(f"{label} is unavailable (no eligible samples)")
        elif float(actual) > maximum:
            failures.append(f"{label}={float(actual):.4f} exceeds allowed {maximum:.4f}")

    require_count("gt_with_mod_total", min_ground_truth, "ground_truth_samples")
    require_count(
        "high_confidence_predictions_total",
        min_high_confidence_predictions,
        "high_confidence_predictions",
    )
    require_min(
        "overall_top1_accuracy_vs_ground_truth_mod",
        min_top1_accuracy,
        "top1_accuracy",
    )
    require_min("overall_top3_recall_vs_ground_truth_mod", min_top3_recall, "top3_recall")
    require_min(
        "overall_high_confidence_precision_vs_ground_truth_mod",
        min_high_confidence_precision,
        "high_confidence_precision",
    )
    require_max(
        "overall_abstention_rate_with_ground_truth_mod",
        max_abstention_rate,
        "abstention_rate",
    )

    return {
        "configured": configured,
        "passed": not failures,
        "thresholds": thresholds,
        "failures": failures,
    }


def _print_table(report: dict[str, Any], top: int) -> None:
    print("SkyrimDiag bucket quality report")
    print(
        f"root={report['input_root']} parsed={report['files_parsed']}/{report['files_found']} "
        f"unique={report['unique_incidents_analyzed']} buckets={report['bucket_count']}"
    )
    print(
        f"duplicate_files_skipped={report['duplicate_files_skipped']} "
        f"duplicate_conflicting_ground_truth={report['duplicate_conflicting_ground_truth_count']} "
        f"duplicate_conflicting_predictions={report['duplicate_conflicting_prediction_count']}"
    )
    print(
        "overall_top1_precision_vs_ground_truth_mod="
        f"{report['overall_top1_precision_vs_ground_truth_mod']}"
    )
    print(
        "overall_top1_accuracy_vs_ground_truth_mod="
        f"{report['overall_top1_accuracy_vs_ground_truth_mod']}"
    )
    print(
        "overall_top1_precision_when_predicted="
        f"{report['overall_top1_precision_when_predicted']}"
    )
    print(
        "overall_top3_recall_vs_ground_truth_mod="
        f"{report['overall_top3_recall_vs_ground_truth_mod']}"
    )
    print(
        "overall_high_confidence_precision_vs_ground_truth_mod="
        f"{report['overall_high_confidence_precision_vs_ground_truth_mod']}"
    )
    print(
        "overall_abstention_rate_with_ground_truth_mod="
        f"{report['overall_abstention_rate_with_ground_truth_mod']}"
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
    parser.add_argument(
        "--min-ground-truth",
        type=int,
        help="Fail unless at least this many reviewed summaries contain ground_truth_mod",
    )
    parser.add_argument(
        "--min-high-confidence-predictions",
        type=int,
        help="Fail unless at least this many ground-truth cases produced a High prediction",
    )
    parser.add_argument(
        "--min-top1-accuracy",
        type=_rate_arg,
        help="Fail when user-visible top-1 accuracy is below this 0..1 threshold",
    )
    parser.add_argument(
        "--min-top3-recall",
        type=_rate_arg,
        help="Fail when user-visible top-3 recall is below this 0..1 threshold",
    )
    parser.add_argument(
        "--min-high-confidence-precision",
        type=_rate_arg,
        help="Fail when High-confidence precision is below this 0..1 threshold",
    )
    parser.add_argument(
        "--max-abstention-rate",
        type=_rate_arg,
        help="Fail when the user-visible abstention rate exceeds this 0..1 threshold",
    )
    args = parser.parse_args()

    if args.min_ground_truth is not None and args.min_ground_truth < 0:
        parser.error("--min-ground-truth must be non-negative")
    if args.min_high_confidence_predictions is not None and args.min_high_confidence_predictions < 0:
        parser.error("--min-high-confidence-predictions must be non-negative")

    report = aggregate(
        root=args.root,
        recursive=not args.non_recursive,
        pattern=args.pattern,
    )
    report["quality_gate"] = evaluate_quality_gate(
        report,
        min_ground_truth=args.min_ground_truth,
        min_high_confidence_predictions=args.min_high_confidence_predictions,
        min_top1_accuracy=args.min_top1_accuracy,
        min_top3_recall=args.min_top3_recall,
        min_high_confidence_precision=args.min_high_confidence_precision,
        max_abstention_rate=args.max_abstention_rate,
    )
    _print_table(report, top=max(1, args.top))

    if args.out_json:
        args.out_json.parent.mkdir(parents=True, exist_ok=True)
        args.out_json.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        print(f"\nWrote: {args.out_json}")

    if not report["quality_gate"]["passed"]:
        print("\nQUALITY GATE FAILED")
        for failure in report["quality_gate"]["failures"]:
            print(f"  - {failure}")
        return 2
    if report["quality_gate"]["configured"]:
        print("\nQUALITY GATE PASSED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Behavioral tests for scripts/analyze_bucket_quality.py."""

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path


def _write_summary(path: Path, payload: dict) -> None:
    path.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def _run_script(root: Path, out_json: Path, *extra_args: str, expected_exit: int = 0) -> dict:
    repo_root = Path(__file__).resolve().parent.parent
    script = repo_root / "scripts" / "analyze_bucket_quality.py"
    python = sys.executable or "python3"
    result = subprocess.run(
        [
            python,
            str(script),
            "--root",
            str(root),
            "--non-recursive",
            "--out-json",
            str(out_json),
            "--top",
            "5",
            *extra_args,
        ],
        cwd=str(repo_root),
        check=False,
    )
    assert result.returncode == expected_exit
    return json.loads(out_json.read_text(encoding="utf-8"))


def test_ground_truth_mod_matches_inferred_mod_name() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        summary = {
            "crash_bucket_key": "bucket-a",
            "triage": {
                "review_status": "reviewed",
                "ground_truth_mod": "my texture overhaul",
            },
            "exception": {"module_plus_offset": "unknown"},
            "suspects": [
                {
                    "module_filename": "SomeDll.dll",
                    "inferred_mod_name": "My Texture Overhaul",
                }
            ],
        }
        _write_summary(root / "case1_SkyrimDiagSummary.json", summary)
        report = _run_script(root=root, out_json=root / "report.json")

        assert report["gt_with_mod_total"] == 1
        assert report["gt_top1_match_total"] == 1
        assert report["overall_top1_precision_vs_ground_truth_mod"] == 1.0


def test_user_visible_actionable_candidate_is_graded_before_raw_suspect() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        summary = {
            "crash_bucket_key": "bucket-user-visible",
            "triage": {
                "review_status": "reviewed",
                "ground_truth_mod": "Actual Cause",
            },
            "exception": {"module_plus_offset": "SkyrimSE.exe+0x1234"},
            "actionable_candidates": [
                {
                    "mod_name": "Wrong Candidate",
                    "module_filename": "Wrong.dll",
                    "confidence": "High",
                },
                {"mod_name": "Actual Cause", "module_filename": "Actual.dll", "confidence": "Medium"},
            ],
            "suspects": [
                {"inferred_mod_name": "Actual Cause", "module_filename": "Actual.dll"}
            ],
        }
        _write_summary(root / "case_visible_SkyrimDiagSummary.json", summary)
        report = _run_script(root=root, out_json=root / "report.json")

        assert report["gt_top1_match_total"] == 0
        assert report["gt_top3_match_total"] == 1
        assert report["overall_top1_precision_vs_ground_truth_mod"] == 0.0
        assert report["overall_top1_accuracy_vs_ground_truth_mod"] == 0.0
        assert report["overall_top1_precision_when_predicted"] == 0.0
        assert report["overall_top3_recall_vs_ground_truth_mod"] == 1.0
        assert report["high_confidence_predictions_total"] == 1
        assert report["high_confidence_matches_total"] == 0
        assert report["overall_high_confidence_precision_vs_ground_truth_mod"] == 0.0
        assert report["buckets"][0]["gt_top1_legacy_suspect_match"] == 1


def test_user_visible_object_ref_is_graded_before_raw_suspect_fallback() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        summary = {
            "crash_bucket_key": "bucket-object-ref-fallback",
            "triage": {
                "review_status": "reviewed",
                "ground_truth_mod": "Actual Cause.esp",
            },
            "exception": {"module_plus_offset": "unknown"},
            "actionable_candidates": [],
            "crash_logger": {
                "object_refs": [
                    {
                        "esp_name": "Actual Cause.esp",
                        "relevance_score": 16,
                    }
                ]
            },
            "suspects": [
                {
                    "inferred_mod_name": "Wrong DLL Mod",
                    "module_filename": "Wrong.dll",
                    "confidence": "High",
                }
            ],
        }
        _write_summary(root / "case_object_ref_SkyrimDiagSummary.json", summary)
        report = _run_script(root=root, out_json=root / "report.json")

        assert report["gt_top1_match_total"] == 1
        assert report["gt_top3_match_total"] == 1
        assert report["high_confidence_predictions_total"] == 1
        assert report["overall_high_confidence_precision_vs_ground_truth_mod"] == 1.0


def test_abstention_is_measured_when_no_candidate_is_shown() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        summary = {
            "crash_bucket_key": "bucket-abstain",
            "triage": {
                "review_status": "reviewed",
                "ground_truth_mod": "Unknown Until Repro",
            },
            "exception": {"module_plus_offset": "unknown"},
            "actionable_candidates": [],
            "suspects": [],
        }
        _write_summary(root / "case_abstain_SkyrimDiagSummary.json", summary)
        report = _run_script(root=root, out_json=root / "report.json")

        assert report["abstained_with_ground_truth_total"] == 1
        assert report["overall_abstention_rate_with_ground_truth_mod"] == 1.0


def test_quality_gate_fails_closed_when_samples_or_precision_are_missing() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        summary = {
            "crash_bucket_key": "bucket-gate-fail",
            "triage": {
                "review_status": "reviewed",
                "ground_truth_mod": "Actual Cause",
            },
            "exception": {"module_plus_offset": "SkyrimSE.exe+0x1234"},
            "actionable_candidates": [
                {"mod_name": "Wrong Candidate", "confidence": "High"}
            ],
        }
        _write_summary(root / "gate_fail_SkyrimDiagSummary.json", summary)
        report = _run_script(
            root,
            root / "report.json",
            "--min-ground-truth",
            "2",
            "--min-high-confidence-precision",
            "0.9",
            expected_exit=2,
        )

        assert report["quality_gate"]["configured"] is True
        assert report["quality_gate"]["passed"] is False
        assert len(report["quality_gate"]["failures"]) == 2


def test_quality_gate_passes_when_configured_thresholds_are_met() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        summary = {
            "crash_bucket_key": "bucket-gate-pass",
            "triage": {
                "review_status": "reviewed",
                "ground_truth_mod": "Actual Cause",
            },
            "exception": {"module_plus_offset": "Actual.dll+0x10"},
            "actionable_candidates": [
                {"mod_name": "Actual Cause", "module_filename": "Actual.dll", "confidence": "High"}
            ],
        }
        _write_summary(root / "gate_pass_SkyrimDiagSummary.json", summary)
        report = _run_script(
            root,
            root / "report.json",
            "--min-ground-truth",
            "1",
            "--min-high-confidence-predictions",
            "1",
            "--min-top1-accuracy",
            "1.0",
            "--min-top3-recall",
            "1.0",
            "--min-high-confidence-precision",
            "1.0",
            "--max-abstention-rate",
            "0.0",
        )

        assert report["quality_gate"]["configured"] is True
        assert report["quality_gate"]["passed"] is True
        assert report["quality_gate"]["failures"] == []


def test_korean_high_confidence_is_included_in_precision() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        summary = {
            "crash_bucket_key": "bucket-korean-confidence",
            "triage": {
                "review_status": "reviewed",
                "ground_truth_mod": "한글 모드",
            },
            "exception": {"module_plus_offset": "KoreanMod.dll+0x10"},
            "actionable_candidates": [
                {"mod_name": "한글 모드", "module_filename": "KoreanMod.dll", "confidence": "높음"}
            ],
        }
        _write_summary(root / "korean_SkyrimDiagSummary.json", summary)
        report = _run_script(root, root / "report.json")

        assert report["high_confidence_predictions_total"] == 1
        assert report["high_confidence_matches_total"] == 1
        assert report["overall_high_confidence_precision_vs_ground_truth_mod"] == 1.0


def test_json_null_cannot_fabricate_ground_truth_or_candidate_match() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        summary = {
            "crash_bucket_key": "bucket-null-fields",
            "triage": {
                "review_status": "reviewed",
                "ground_truth_mod": None,
            },
            "exception": {"module_plus_offset": None},
            "actionable_candidates": [
                {"mod_name": None, "module_filename": None, "confidence": "High"}
            ],
        }
        _write_summary(root / "null_SkyrimDiagSummary.json", summary)
        report = _run_script(
            root,
            root / "report.json",
            "--min-ground-truth",
            "1",
            expected_exit=2,
        )

        assert report["reviewed_total"] == 1
        assert report["gt_with_mod_total"] == 0
        assert report["gt_top1_match_total"] == 0
        assert report["high_confidence_predictions_total"] == 0
        assert report["quality_gate"]["passed"] is False


def test_duplicate_incident_cannot_inflate_minimum_sample_count() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        summary = {
            "dump_path": r"C:\Crashes\same.dmp",
            "incident": {"incident_id": "same-incident"},
            "crash_bucket_key": "bucket-duplicate",
            "triage": {
                "review_status": "reviewed",
                "ground_truth_mod": "Actual Cause",
            },
            "exception": {"module_plus_offset": "Actual.dll+0x10"},
            "actionable_candidates": [
                {"mod_name": "Actual Cause", "confidence": "High"}
            ],
        }
        _write_summary(root / "copy_a_SkyrimDiagSummary.json", summary)
        _write_summary(root / "copy_b_SkyrimDiagSummary.json", summary)
        report = _run_script(
            root,
            root / "report.json",
            "--min-ground-truth",
            "2",
            expected_exit=2,
        )

        assert report["files_parsed"] == 2
        assert report["unique_incidents_analyzed"] == 1
        assert report["duplicate_files_skipped"] == 1
        assert report["duplicate_identity_count"] == 1
        assert report["gt_with_mod_total"] == 1


def test_duplicate_incident_with_conflicting_labels_fails_configured_gate() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        base = {
            "dump_path": r"C:\Crashes\conflict.dmp",
            "incident": {"incident_id": "conflicting-incident"},
            "crash_bucket_key": "bucket-conflicting-label",
            "exception": {"module_plus_offset": "Example.dll+0x10"},
        }
        first = {
            **base,
            "triage": {"review_status": "reviewed", "ground_truth_mod": "Cause A"},
            "actionable_candidates": [{"mod_name": "Cause A", "confidence": "High"}],
        }
        second = {
            **base,
            "triage": {"review_status": "reviewed", "ground_truth_mod": "Cause B"},
            "actionable_candidates": [{"mod_name": "Cause B", "confidence": "High"}],
        }
        _write_summary(root / "a_SkyrimDiagSummary.json", first)
        _write_summary(root / "z_SkyrimDiagSummary.json", second)
        report = _run_script(
            root,
            root / "report.json",
            "--min-ground-truth",
            "1",
            expected_exit=2,
        )

        assert report["duplicate_conflicting_ground_truth_count"] == 1
        assert report["quality_gate"]["passed"] is False
        assert any(
            "conflicting ground truth" in failure
            for failure in report["quality_gate"]["failures"]
        )


def test_duplicate_incident_with_different_predictions_fails_configured_gate() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        base = {
            "dump_path": r"C:\Crashes\reanalyzed.dmp",
            "incident": {"incident_id": "reanalyzed-incident"},
            "crash_bucket_key": "bucket-reanalysis",
            "triage": {
                "review_status": "reviewed",
                "reviewed_at_utc": "2026-07-13T00:00:00Z",
                "ground_truth_mod": "Actual Cause",
            },
            "exception": {"module_plus_offset": "Example.dll+0x10"},
        }
        first = {
            **base,
            "actionable_candidates": [{"mod_name": "Actual Cause", "confidence": "High"}],
        }
        second = {
            **base,
            "actionable_candidates": [{"mod_name": "Wrong Cause", "confidence": "High"}],
        }
        _write_summary(root / "a_SkyrimDiagSummary.json", first)
        _write_summary(root / "z_SkyrimDiagSummary.json", second)
        report = _run_script(
            root,
            root / "report.json",
            "--min-ground-truth",
            "1",
            expected_exit=2,
        )

        assert report["duplicate_conflicting_prediction_count"] == 1
        assert report["quality_gate"]["passed"] is False
        assert any(
            "conflicting analyzer predictions" in failure
            for failure in report["quality_gate"]["failures"]
        )


def test_feedback_fields_mark_summary_as_reviewed() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        summary = {
            "crash_bucket_key": "bucket-b",
            "triage": {
                "review_status": "unreviewed",
                "actual_cause": "Bad mesh in custom armor",
                "verdict": "Confirmed mod-side CTD",
                "notes": "Top suspect matched manual repro",
            },
            "exception": {"module_plus_offset": "SkyrimSE.exe+0xD6DDDA"},
            "suspects": [],
        }
        _write_summary(root / "case2_SkyrimDiagSummary.json", summary)
        report = _run_script(root=root, out_json=root / "report.json")

        assert report["reviewed_total"] == 1


if __name__ == "__main__":
    test_ground_truth_mod_matches_inferred_mod_name()
    test_user_visible_actionable_candidate_is_graded_before_raw_suspect()
    test_user_visible_object_ref_is_graded_before_raw_suspect_fallback()
    test_abstention_is_measured_when_no_candidate_is_shown()
    test_quality_gate_fails_closed_when_samples_or_precision_are_missing()
    test_quality_gate_passes_when_configured_thresholds_are_met()
    test_korean_high_confidence_is_included_in_precision()
    test_json_null_cannot_fabricate_ground_truth_or_candidate_match()
    test_duplicate_incident_cannot_inflate_minimum_sample_count()
    test_duplicate_incident_with_conflicting_labels_fails_configured_gate()
    test_duplicate_incident_with_different_predictions_fails_configured_gate()
    test_feedback_fields_mark_summary_as_reviewed()
    print("bucket_quality_script_tests: OK")

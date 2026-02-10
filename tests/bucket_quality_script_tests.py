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


def _run_script(root: Path, out_json: Path) -> dict:
    repo_root = Path(__file__).resolve().parent.parent
    script = repo_root / "scripts" / "analyze_bucket_quality.py"
    python = sys.executable or "python3"
    subprocess.check_call(
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
        ],
        cwd=str(repo_root),
    )
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


if __name__ == "__main__":
    test_ground_truth_mod_matches_inferred_mod_name()
    print("bucket_quality_script_tests: OK")

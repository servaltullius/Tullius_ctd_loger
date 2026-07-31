#!/usr/bin/env python3
"""Candidate-consensus behavior gate over the in-repo regression corpus.

Runs on every push so a change that silently re-ranks candidates gets caught
immediately rather than at release time.

The source fixtures contain raw CandidateSignal inputs, not precomputed Summary
predictions. A small C++ runner calls the production BuildCandidateConsensus(),
checks each scenario's exact candidate contract, and writes synthetic summaries.
Only those generated summaries are passed to analyze_bucket_quality.py.

This is still a deterministic behavior test, NOT a measurement of real-world
accuracy. Release gate step 7 owns that separate claim and reports "not measured"
without a reviewed real-incident corpus. See tests/data/quality_corpus/README.md.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
CORPUS_DIR = REPO_ROOT / "tests" / "data" / "quality_corpus"
QUALITY_SCRIPT = REPO_ROOT / "scripts" / "analyze_bucket_quality.py"

THRESHOLDS = {
    "--min-ground-truth": "5",
    "--min-high-confidence-predictions": "1",
    "--min-top1-accuracy": "0.60",
    "--min-top3-recall": "0.80",
    # A High-confidence claim that is wrong is the most damaging output this tool
    # can produce, so it must stay perfect on the corpus.
    "--min-high-confidence-precision": "1.00",
    # Bounded so "abstain more often" cannot be used to inflate accuracy.
    "--max-abstention-rate": "0.20",
}

EXPECTED_COUNTS = {
    "files_parsed": 5,
    "gt_with_mod_total": 5,
    "gt_top1_match_total": 3,
    "gt_top3_match_total": 4,
    "high_confidence_predictions_total": 3,
    "high_confidence_matches_total": 3,
    "abstained_with_ground_truth_total": 1,
}


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Generate and score the synthetic CandidateConsensus corpus"
    )
    parser.add_argument(
        "--runner",
        type=Path,
        required=True,
        help="Path to the built skydiag_quality_corpus_runner executable",
    )
    return parser.parse_args()


def main() -> int:
    args = _parse_args()
    if not CORPUS_DIR.is_dir():
        print(f"missing quality corpus: {CORPUS_DIR}", file=sys.stderr)
        return 1

    fixtures = sorted(CORPUS_DIR.glob("*_signals.json"))
    if not fixtures:
        print(f"quality corpus contains no signal fixtures: {CORPUS_DIR}", file=sys.stderr)
        return 1
    stale_summaries = sorted(CORPUS_DIR.glob("*_SkyrimDiagSummary.json"))
    if stale_summaries:
        print(
            "quality corpus must not contain precomputed Summary predictions: "
            + ", ".join(path.name for path in stale_summaries),
            file=sys.stderr,
        )
        return 1
    if not args.runner.is_file():
        print(f"quality corpus runner not found: {args.runner}", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory() as tmp:
        generated_dir = Path(tmp) / "generated-summaries"
        generation = subprocess.run(
            [str(args.runner), str(CORPUS_DIR), str(generated_dir)],
            cwd=str(REPO_ROOT),
            check=False,
        )
        if generation.returncode != 0:
            print("CandidateConsensus summary generation failed.", file=sys.stderr)
            return generation.returncode

        generated = sorted(generated_dir.glob("*_SkyrimDiagSummary.json"))
        if len(generated) != len(fixtures):
            print(
                f"generated summary count mismatch: fixtures={len(fixtures)} "
                f"summaries={len(generated)}",
                file=sys.stderr,
            )
            return 1

        report = Path(tmp) / "analysis-quality-corpus.json"
        quality_args = [
            sys.executable or "python3",
            str(QUALITY_SCRIPT),
            "--root",
            str(generated_dir),
            "--out-json",
            str(report),
        ]
        for flag, value in THRESHOLDS.items():
            quality_args.extend([flag, value])

        result = subprocess.run(quality_args, cwd=str(REPO_ROOT), check=False)
        if result.returncode != 0:
            print(
                "\nAttribution behavior gate failed. Either production consensus logic "
                "regressed, or an intentional fixture contract changed.",
                file=sys.stderr,
            )
            return result.returncode

        report_data = json.loads(report.read_text(encoding="utf-8"))
        mismatches = [
            f"{field}: expected {expected}, got {report_data.get(field)!r}"
            for field, expected in EXPECTED_COUNTS.items()
            if report_data.get(field) != expected
        ]
        if mismatches:
            print("\nGenerated corpus metrics changed:", file=sys.stderr)
            for mismatch in mismatches:
                print(f"  - {mismatch}", file=sys.stderr)
            return 1

    print(
        f"\nCandidateConsensus behavior gate passed over {len(fixtures)} "
        "generated synthetic summaries."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

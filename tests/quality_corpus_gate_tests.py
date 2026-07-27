#!/usr/bin/env python3
"""Analyzer attribution behavior gate over the in-repo regression corpus.

Runs on every push so a change that silently re-ranks candidates gets caught
immediately rather than at release time.

Scope, deliberately narrow: the corpus is hand-authored fixtures pinning intended
analyzer decisions. Passing means the analyzer still decides what it was designed
to decide. It is NOT a measurement of real-world accuracy, and must never be used
to report that accuracy has been verified -- release gate step 7 owns that claim
and honestly reports "not measured" without a reviewed real-incident corpus.
See tests/data/quality_corpus/README.md.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
CORPUS_DIR = REPO_ROOT / "tests" / "data" / "quality_corpus"
QUALITY_SCRIPT = REPO_ROOT / "scripts" / "analyze_bucket_quality.py"

# The fixtures are deterministic, so these are the exact values the corpus
# currently produces rather than loosened bounds. Any drift is a real behavior
# change and should be reviewed -- update these together with the fixture that
# caused the change, and say why in the commit message.
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


def main() -> int:
    if not CORPUS_DIR.is_dir():
        print(f"missing quality corpus: {CORPUS_DIR}", file=sys.stderr)
        return 1

    fixtures = sorted(CORPUS_DIR.glob("*_SkyrimDiagSummary.json"))
    if not fixtures:
        print(f"quality corpus contains no summary fixtures: {CORPUS_DIR}", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory() as tmp:
        report = Path(tmp) / "analysis-quality-corpus.json"
        args = [
            sys.executable or "python3",
            str(QUALITY_SCRIPT),
            "--root",
            str(CORPUS_DIR),
            "--out-json",
            str(report),
        ]
        for flag, value in THRESHOLDS.items():
            args.extend([flag, value])

        result = subprocess.run(args, cwd=str(REPO_ROOT), check=False)

    if result.returncode != 0:
        print(
            "\nAttribution quality gate failed. Either a real regression, or the corpus "
            "changed and THRESHOLDS in this file need updating to match.",
            file=sys.stderr,
        )
        return result.returncode

    print(f"\nQuality corpus gate passed over {len(fixtures)} fixtures.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

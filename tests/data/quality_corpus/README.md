# Analysis quality regression corpus

Raw signal fixtures are executed by `skydiag_quality_corpus_runner`, which calls
the production `BuildCandidateConsensus()` implementation and writes temporary
`*_SkyrimDiagSummary.json` files. `skydiag_quality_corpus_gate_tests` then scores
only those generated summaries with `scripts/analyze_bucket_quality.py`.

## What this corpus is — and is not

These are **hand-authored signal scenarios**, not real incidents. Each one pins a
decision candidate consensus is supposed to make: combine independent evidence,
keep stack-only candidates cautious, retain a useful top-three candidate, or
abstain rather than manufacture a result from boost-only history.

Unlike precomputed Summary fixtures, candidate order, status, confidence, score,
and abstention are produced during the test by the same `CandidateConsensus.cpp`
used by the analyzer. Each scenario checks its exact candidate contract before
the generated summaries are aggregated. A broken consensus implementation must
therefore fail before the metric report can pass.

This remains a **behavior regression gate**, not a measurement of real-world
accuracy. Passing says the implementation still makes these five designed
decisions. It says nothing about how often it identifies a real root cause — see
the accuracy caveats in the top-level `README.md`.

The two claims are kept deliberately separate. Release gate step 7 measures
accuracy against reviewed real incidents, and it reports `SKIPPED (not measured)`
when no such corpus is configured. That is intentional and enforced by
`tests/packaging_includes_cli_tests.py`: an unmeasured property must never be
reported as a pass, and these fixtures must not be used to imply otherwise.

## Measuring real accuracy

Point release gate step 7 at a private corpus of reviewed incidents:

```bash
export SKYDIAG_QUALITY_CORPUS=/path/to/reviewed-incidents
export SKYDIAG_QUALITY_MIN_GROUND_TRUTH=50
export SKYDIAG_QUALITY_MIN_HIGH_CONFIDENCE_PREDICTIONS=10
export SKYDIAG_QUALITY_MIN_TOP1_ACCURACY=0.55
export SKYDIAG_QUALITY_MIN_TOP3_RECALL=0.75
export SKYDIAG_QUALITY_MIN_HIGH_CONFIDENCE_PRECISION=0.90
export SKYDIAG_QUALITY_MAX_ABSTENTION_RATE=0.35
bash scripts/verify_release_gate.sh
```

`SKYDIAG_QUALITY_CORPUS` accepts reviewed analyzer Summary files and is separate
from these raw signal scenarios. A real corpus should cover at least the same
important evidence shapes, but these synthetic fixtures must never be mixed into
its accuracy denominator.

Real dumps carry usernames and drive letters, so keep that corpus out of the
repository.

An incident joins the corpus once its cause is confirmed independently of this
tool — a source-level fix, a reproduction that disappears when one mod is
removed, or an author confirming the bug. A cause that came from reading this
tool's own report must not become its own ground truth.

## Fixture and execution contract

Source files are named `*_signals.json` and contain:

| Field | Role |
|---|---|
| `_fixture_kind` | Versioned raw-signal schema identifier |
| `scenario_id` | Stable synthetic incident and output filename key |
| `ground_truth_mod` | Expected answer for this designed scenario, not real-incident ground truth |
| `signals[]` | Inputs mapped directly to production `CandidateSignal` fields |
| `expected_candidates[]` | Exact ordered subset of generated candidate fields to pin |

The runner intentionally accepts ASCII fixture identifiers only. Production
Unicode canonicalization remains covered by `skydiag_candidate_consensus_tests`;
keeping this corpus ASCII makes the generated cross-platform Summary JSON
deterministic.

The source directory must not contain `*_SkyrimDiagSummary.json`. Summaries are
always created in a temporary directory during the test, which prevents a stale
checked-in prediction from bypassing the production consensus code.

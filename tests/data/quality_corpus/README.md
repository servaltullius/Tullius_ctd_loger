# Analysis quality regression corpus

Fixtures scored by `scripts/analyze_bucket_quality.py`, run as
`skydiag_quality_corpus_gate_tests` in ctest.

## What this corpus is — and is not

These are **hand-authored summary fixtures**, not real incidents. Each one pins a
decision the analyzer is supposed to make: prefer the real culprit over a hook
framework, do not blame a system DLL for being the observation point, abstain
rather than guess when the evidence is thin.

That makes them a **behavior regression gate**, not a measurement of real-world
accuracy. Passing says the analyzer still decides what it was designed to decide.
It says nothing about how often it identifies a real root cause — see the
accuracy caveats in the top-level `README.md`.

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

Setting `SKYDIAG_QUALITY_CORPUS` replaces this corpus rather than adding to it,
so a real corpus should be a superset of the behaviors pinned here.

Real dumps carry usernames and drive letters, so keep that corpus out of the
repository.

An incident joins the corpus once its cause is confirmed independently of this
tool — a source-level fix, a reproduction that disappears when one mod is
removed, or an author confirming the bug. A cause that came from reading this
tool's own report must not become its own ground truth.

## Fixture format

`*_SkyrimDiagSummary.json`, the analyzer's own output schema. The gate reads:

| Field | Role |
|---|---|
| `triage.ground_truth_mod` | Confirmed culprit; the answer being graded |
| `triage.review_status` | Marks the incident reviewed |
| `actionable_candidates[]` | Ranked predictions (`confidence`, `mod_name`, `module_filename`) |
| `suspects[]` | Raw stack suspects, used when no actionable candidate exists |
| `crash_bucket_key` | Groups incidents for per-bucket reporting |
| `incident.incident_id` | Deduplicates the same incident appearing twice |

A candidate matches when any of `mod_name`, `inferred_mod_name`,
`module_filename`, `plugin_name`, `primary_identifier` or `display_name` equals
`ground_truth_mod`, compared case-insensitively with collapsed whitespace.

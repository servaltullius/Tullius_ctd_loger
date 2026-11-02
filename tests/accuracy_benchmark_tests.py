"""Tests for scripts/accuracy_benchmark.py.

The benchmark compares three views of the same confirmed incident; each test
pins one part of how a view's answer is read and scored, using shapes taken
from the real summaries and Crash Logger logs of 2026-10-12/13.
"""

import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "scripts"))
import accuracy_benchmark as bench  # noqa: E402

CONTENT_CATALOG_LOG = """CRASH TIME: 2026-10-13 11:34:03
Unhandled exception "C++ Exception" at 0x7FFB541B483A KERNELBASE.dll+00C483A
CALL STACK ([P]robable / [S]tack scan):
\t[ 0][P] 0x7FFB541B483A    KERNELBASE.dll+00C483A
\t[ 1][P] 0x7FFB378755A9  VCRUNTIME140.dll+00055A9
\t[ 2][P] 0x7FFB377F4E92      MSVCP140.dll+0044E92
\t[ 3][P] 0x7FF6ABA95AE9      SkyrimSE.exe+1235AE9 -> 443464+0x119\tint3
\t[13][P] 0x7FF9BE614153 NPCWaterAIFix.dll+0024153
\t[14][P] 0x7FF99793FA07        Trails.dll+004FA07\tmov rcx, rbx
\t[16][P] 0x7FFB55EACDF7      KERNEL32.DLL+002CDF7
\t[18][S] 0x7FFAD3BD05D6   EngineFixes.dll+00F05D6\tmov r8d, [rdi+0x68]

REGISTERS:
\tRAX 0x0
"""


def test_crash_logger_view_reads_probable_frames_only():
    modules = bench.crash_logger_probable_modules(CONTENT_CATALOG_LOG)
    assert modules == ["KERNELBASE.dll", "VCRUNTIME140.dll", "MSVCP140.dll", "SkyrimSE.exe",
                       "NPCWaterAIFix.dll", "Trails.dll", "KERNEL32.DLL"], modules
    view = bench.crash_logger_view(CONTENT_CATALOG_LOG)
    # A reader skips Windows, the runtime and the game EXE; [S] rows are not frames.
    assert view == {"ranked": ["NPCWaterAIFix.dll", "Trails.dll"], "lead": "NPCWaterAIFix.dll"}, view
    assert bench.crash_logger_view(None) == {"ranked": [], "lead": ""}


def test_tullius_view_leads_with_the_actionable_candidate():
    summary = {
        "actionable_candidates": [{"module_filename": "CrashTester.dll", "mod_name": "CrashTester (dev test)"}],
        "suspects": [
            {"module_filename": "CrashTester.dll", "inferred_mod_name": "CrashTester (dev test)"},
            {"module_filename": "Other.dll"},
        ],
        "signature_match": {"id": "CONTENT_CATALOG_FORMAT_MISMATCH"},
    }
    view = bench.tullius_view(summary)
    assert view["lead"][0] == "crashtester.dll"
    assert len(view["ranked"]) == 2, view["ranked"]  # the suspect repeating the candidate is not a second entry
    assert view["known_cause"] == "CONTENT_CATALOG_FORMAT_MISMATCH"
    empty = bench.tullius_view({"actionable_candidates": [], "suspects": [], "signature_match": None})
    assert empty["lead"] == [] and empty["known_cause"] == ""


def test_scoring_outcomes():
    truth = {"modules": ["CrashTester.dll"]}
    lead_right = {"lead": ["crashtester.dll"], "ranked": [["crashtester.dll"]]}
    lead_wrong = {"lead": ["other.dll"], "ranked": [["other.dll"], ["crashtester.dll"]]}
    assert bench.score_view(lead_right, truth)["outcome"] == "correct"
    wrong = bench.score_view(lead_wrong, truth)
    assert wrong["outcome"] == "wrong" and wrong["top3"] is True
    assert bench.score_view({"lead": [], "ranked": []}, truth)["outcome"] == "abstain"
    # Crash Logger's view carries a string lead.
    assert bench.score_view({"lead": "CrashTester.dll", "ranked": ["CrashTester.dll"]}, truth)["outcome"] == "correct"

    # "No plugin caused it": naming nothing is right, naming a plugin is wrong.
    none_truth = {"none": True, "known_cause": "CONTENT_CATALOG_FORMAT_MISMATCH"}
    quiet = bench.score_view({"lead": [], "ranked": [], "known_cause": "CONTENT_CATALOG_FORMAT_MISMATCH"}, none_truth)
    assert quiet == {"outcome": "correct", "top3": None, "lead": "", "known_cause_hit": True}, quiet
    blamed = bench.score_view({"lead": "NPCWaterAIFix.dll", "ranked": ["NPCWaterAIFix.dll"]}, none_truth)
    assert blamed["outcome"] == "wrong" and "known_cause_hit" not in blamed


def test_quick_answer_is_scored_apart_from_the_ranking():
    # A forced close: the analysis below still ranks the plugin that called
    # freed code, but the quick answer names no mod. With "no plugin caused it"
    # the ranking leads wrong and the quick answer is right.
    forced = {
        "actionable_candidates": [{"module_filename": "PrismaUI.dll"}],
        "suspects": [{"module_filename": "PrismaUI.dll"}],
        "signature_match": None,
        "quick_answer": {"kind": "forced_close", "case": "forced_close", "suspect_mod": "", "suspect_file": ""},
    }
    none_truth = {"none": True}
    assert bench.score_view(bench.tullius_view(forced), none_truth)["outcome"] == "wrong"
    qa = bench.score_view(bench.quick_answer_view(forced), none_truth)
    assert qa == {"outcome": "correct", "top3": None, "lead": "", "case": "forced_close"}, qa

    # A named lead is matched by its file or its mod name; naming none when a
    # mod was at fault is an abstention, not a hit.
    named = {"quick_answer": {"case": "candidate", "suspect_mod": "Real Mod", "suspect_file": "Real.dll"}}
    assert bench.score_view(bench.quick_answer_view(named), {"modules": ["Real Mod"]})["outcome"] == "correct"
    assert bench.score_view(bench.quick_answer_view(named), {"modules": ["Other.dll"]})["outcome"] == "wrong"
    quiet = bench.score_view(bench.quick_answer_view(forced), {"modules": ["PrismaUI.dll"]})
    assert quiet["outcome"] == "abstain" and quiet["top3"] is None, quiet

    # A known cause the quick answer leads with is checked against the
    # expected signature; a signature it does not lead with is not counted.
    known = {"signature_match": {"id": "CONTENT_CATALOG_FORMAT_MISMATCH"},
             "quick_answer": {"case": "known_issue", "suspect_mod": "", "suspect_file": ""}}
    cause_truth = {"none": True, "known_cause": "CONTENT_CATALOG_FORMAT_MISMATCH"}
    assert bench.score_view(bench.quick_answer_view(known), cause_truth)["known_cause_hit"] is True
    other_lead = dict(known, quick_answer={"case": "damaged_save", "suspect_mod": "", "suspect_file": ""})
    assert bench.score_view(bench.quick_answer_view(other_lead), cause_truth)["known_cause_hit"] is False

    # A summary from before the quick answer existed names nothing.
    assert bench.quick_answer_view({"actionable_candidates": []})["lead"] == []


def _write(path: Path, payload) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload), encoding="utf-8")


def test_reviewed_quick_answer_guidance():
    truth = {"none": True, "quick_answer": {"case": "forced_close", "action": "Nothing to do."}}
    for qa, case_score, action_score in (
        ({"case": "forced_close", "action": "Nothing to do."}, "correct", "correct"),
        ({"case": "forced_close", "action": "Nothing  to do. "}, "correct", "correct"),
        ({"case": "forced_close", "action": "Load another save."}, "correct", "wrong"),
        ({"case": "damaged_save", "action": "Load another save."}, "wrong", "wrong"),
        ({"case": "forced_close"}, "correct", "missing"),
        ({}, "missing", "missing"),
    ):
        result = bench.score_view(bench.quick_answer_view({"quick_answer": qa}), truth)
        assert result["guidance"] == {"case": case_score, "action": action_score}, result
        assert result["outcome"] == "correct"  # the separate attribution score
    partial = {"none": True, "quick_answer": {"case": "forced_close"}}
    assert bench.score_view(bench.quick_answer_view({}), partial)["guidance"] == {"case": "missing"}
    assert "guidance" not in bench.score_view(bench.tullius_view({}), truth)


FAKE_CLI = """import json, sys
from pathlib import Path
dump = Path(sys.argv[1]); out = Path(sys.argv[sys.argv.index("--out-dir") + 1])
with_cl = "--no-crash-logger" not in sys.argv
summary = {"actionable_candidates": [{"module_filename": "Real.dll"}] if with_cl else [],
           "suspects": [{"module_filename": "Victim.dll"}],
           "signature_match": None,
           "quick_answer": {"case": "candidate", "suspect_mod": "", "suspect_file": "Real.dll" if with_cl else ""}}
if with_cl:  # the analyzer finds the log next to the dump on its own
    summary["crash_logger"] = {"log_path": "<redacted>\\\\crash.log"}
(out / (dump.stem + "_SkyrimDiagSummary.json")).write_text(json.dumps(summary), encoding="utf-8")
"""


def test_add_case_and_score_end_to_end():
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        fake_cli = root / "fake_cli.py"
        fake_cli.write_text(FAKE_CLI, encoding="utf-8")
        dump = root / "SkyrimDiag_Crash_1.dmp"
        dump.write_bytes(b"MDMP")
        log = root / "crash.log"
        log.write_text(CONTENT_CATALOG_LOG, encoding="utf-8")
        corpus = root / "corpus"
        rc = bench.main(["add-case", "--corpus", str(corpus), "--id", "case-1", "--kind", "ctd",
                         "--dump", str(dump), "--crash-logger-log", str(log), "--expect-module", "Real.dll",
                         "--expect-quick-case", "candidate", "--expect-quick-action", "Check Real.dll.",
                         "--cli", sys.executable, str(fake_cli)])
        assert rc == 0
        manifest = json.loads((corpus / "manifest.json").read_text(encoding="utf-8"))
        assert manifest["cases"][0]["ground_truth"] == {
            "modules": ["Real.dll"], "quick_answer": {"case": "candidate", "action": "Check Real.dll."}}
        report = bench.score_corpus(corpus)
        views = report["cases"][0]["views"]
        assert views["tullius_only"]["outcome"] == "wrong"  # without the log only the victim shows
        assert views["tullius_with_crash_logger"]["outcome"] == "correct"
        assert views["crash_logger"]["outcome"] == "wrong"
        assert report["overall"]["tullius_with_crash_logger"]["top1_accuracy"] == 1.0
        # The quick answer is scored apart, in its own group.
        assert report["schema_version"] == 2
        assert report["view_groups"] == {
            "candidate_ranking": ["tullius_only", "tullius_with_crash_logger", "crash_logger"],
            "quick_answer": ["tullius_only_quick_answer", "tullius_with_crash_logger_quick_answer"],
        }
        assert views["tullius_only_quick_answer"]["outcome"] == "abstain"  # names nothing without the log
        assert views["tullius_with_crash_logger_quick_answer"]["outcome"] == "correct"
        assert report["overall"]["tullius_with_crash_logger_quick_answer"]["top3_recall"] is None
        # An older/unlabelled case changes the attribution denominator only.
        unlabelled = dict(manifest["cases"][0], id="older", ground_truth={"modules": ["Real.dll"]})
        manifest["cases"].append(unlabelled)
        shutil.copytree(corpus / "cases" / "case-1", corpus / "cases" / "older")
        _write(corpus / "manifest.json", manifest)
        overall = bench.score_corpus(corpus)["overall"]["tullius_only_quick_answer"]
        assert overall["cases"] == 2
        assert overall["guidance"]["case"] == {"correct": 1, "wrong": 0, "missing": 0, "accuracy": 1.0}
        assert overall["guidance"]["action"] == {"correct": 0, "wrong": 0, "missing": 1, "accuracy": 0.0}
        printed = subprocess.run([sys.executable, str(REPO_ROOT / "scripts" / "accuracy_benchmark.py"), "score",
                                  "--corpus", str(corpus)], capture_output=True, text=True)
        assert printed.returncode == 0
        assert "Candidate ranking" in printed.stdout and "Quick answer" in printed.stdout, printed.stdout
        assert "missing=1" in printed.stdout, printed.stdout
        assert "guidance: case=correct, action=missing" in printed.stdout, printed.stdout
        # A case id is not silently overwritten.
        result = subprocess.run([sys.executable, str(REPO_ROOT / "scripts" / "accuracy_benchmark.py"), "add-case",
                                 "--corpus", str(corpus), "--id", "case-1", "--kind", "ctd", "--dump", str(dump),
                                 "--expect-none", "--cli", sys.executable, str(fake_cli)],
                                capture_output=True, text=True)
        assert result.returncode != 0 and "exists" in (result.stderr + result.stdout)
        # The crash_logger view must read the log the analyzer paired.
        other = root / "other.log"
        other.write_text(CONTENT_CATALOG_LOG, encoding="utf-8")
        for log_args in (["--crash-logger-log", str(other)], []):
            result = subprocess.run([sys.executable, str(REPO_ROOT / "scripts" / "accuracy_benchmark.py"), "add-case",
                                     "--corpus", str(corpus), "--id", "case-2", "--kind", "ctd", "--dump", str(dump),
                                     *log_args, "--expect-none", "--cli", sys.executable, str(fake_cli)],
                                    capture_output=True, text=True)
            assert result.returncode != 0 and "crash.log" in (result.stderr + result.stdout), result


def main() -> int:
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_") and callable(v)]
    for test in tests:
        test()
    print(f"{len(tests)} tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

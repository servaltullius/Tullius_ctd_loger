"""Tests for scripts/accuracy_benchmark.py.

The benchmark compares three views of the same confirmed incident; each test
pins one part of how a view's answer is read and scored, using shapes taken
from the real summaries and Crash Logger logs of 2026-10-12/13.
"""

import json
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


def _write(path: Path, payload) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload), encoding="utf-8")


FAKE_CLI = """import json, sys
from pathlib import Path
dump = Path(sys.argv[1]); out = Path(sys.argv[sys.argv.index("--out-dir") + 1])
with_cl = "--no-crash-logger" not in sys.argv
summary = {"actionable_candidates": [{"module_filename": "Real.dll"}] if with_cl else [],
           "suspects": [{"module_filename": "Victim.dll"}],
           "signature_match": None}
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
                         "--cli", sys.executable, str(fake_cli)])
        assert rc == 0
        manifest = json.loads((corpus / "manifest.json").read_text(encoding="utf-8"))
        assert manifest["cases"][0]["ground_truth"] == {"modules": ["Real.dll"]}
        report = bench.score_corpus(corpus)
        views = report["cases"][0]["views"]
        assert views["tullius_only"]["outcome"] == "wrong"  # without the log only the victim shows
        assert views["tullius_with_crash_logger"]["outcome"] == "correct"
        assert views["crash_logger"]["outcome"] == "wrong"
        assert report["overall"]["tullius_with_crash_logger"]["top1_accuracy"] == 1.0
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

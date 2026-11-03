"""Tests for scripts/compare_analyses.py.

The tool runs two analyzer builds on the same dumps; these tests pin how two
summaries are compared: which fields count, how list changes read, and that
fields which move with every run are left out.
"""

import copy
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "scripts"))
import compare_analyses as cmp  # noqa: E402

BASE = {
    "schema": {"name": "SkyrimDiagSummary", "version": 2},
    "dump_path": "<redacted>\\SkyrimDiag_Crash_20261007_123742_440.dmp",
    "summary_sentence": "Known crash mechanism [ACCESS_VIOLATION_NULL]: ...",
    "crash_bucket_key": "CTD-skyrimse-3ff101",
    "signature_match": {"id": "ACCESS_VIOLATION_NULL", "scope": "mechanism"},
    "exception": {"module_plus_offset": "SkyrimSE.exe+0x3ff101"},
    "quick_answer": {"case": "fault_module", "likelihood": "low", "headline": "The game crashed to desktop (CTD).",
                     "action": "Share the report.", "why_not_certain": "", "suspect_mod": "", "suspect_file": ""},
    "suspects": [{"module_filename": "CrashLogger.dll", "confidence": "Low", "score": 75}],
    "actionable_candidates": [{"display_name": "AE_StellarBlade_Doro.esp", "status_id": "reference_clue",
                               "confidence": "Low"}],
    "recommendations": ["[Basics] ExceptionCode=0xC0000005 (Access Violation).", "[Check] Share the report."],
    "recommendation_classes": [{"group": "verify", "kind": ""}, {"group": "verify", "kind": ""}],
    "freeze_analysis": {"state_id": "", "confidence": ""},
    "evidence": [{"title": "Callstack", "details": "tid=35448"}],
    "crash_history_stats": {"same_pattern": 3, "first": "2026-10-07T12:37:42Z"},
}


def test_same_summary_reads_same() -> None:
    assert cmp.compare(BASE, copy.deepcopy(BASE)) == []


def test_volatile_fields_are_left_out() -> None:
    new = copy.deepcopy(BASE)
    new["dump_path"] = "<redacted>\\elsewhere.dmp"
    new["evidence"][0]["details"] = "tid=1"
    new["crash_history_stats"]["same_pattern"] = 4
    assert cmp.compare(BASE, new) == []


def test_scalar_and_quick_answer_changes() -> None:
    new = copy.deepcopy(BASE)
    new["signature_match"]["id"] = "TRAP_CONTACT_REMOVED_1_6_1170"
    new["quick_answer"]["how_it_crashed"] = "It crashed while using an object that was already gone."
    lines = cmp.compare(BASE, new)
    assert "signature: 'ACCESS_VIOLATION_NULL' -> 'TRAP_CONTACT_REMOVED_1_6_1170'" in lines, lines
    assert any(line.startswith("quick_answer.how_it_crashed: None -> ") for line in lines), lines


def test_list_changes_name_the_entries() -> None:
    new = copy.deepcopy(BASE)
    new["recommendations"][1] = "[Known crash site] Check mods that create or remove traps."
    new["recommendation_classes"][1]["group"] = "immediate"
    lines = cmp.compare(BASE, new)
    assert "recommendations - [Check] Share the report." in lines, lines
    assert "recommendations + [Known crash site] Check mods that create or remove traps." in lines, lines
    # Groups follow the recommendations' order, so they read as one value.
    assert "recommendation_groups: ['verify', 'verify'] -> ['verify', 'immediate']" in lines, lines

    reordered = copy.deepcopy(BASE)
    reordered["recommendations"].reverse()
    assert cmp.compare(BASE, reordered) == ["recommendations: same entries, new order"]


def test_new_and_removed_keys() -> None:
    new = copy.deepcopy(BASE)
    new["analyzer_version"] = "0.2.70"
    new["fault_instruction"] = {"verdict": "null_pointer"}
    new["freeze_analysis"]["game_lock_waits"] = []
    del new["crash_history_stats"]
    lines = cmp.compare(BASE, new)
    assert not cmp.only_keys(lines), "a new field with a value is a change"
    keys = copy.deepcopy(BASE)
    keys["analyzer_version"] = "0.2.70"
    assert cmp.only_keys(cmp.compare(BASE, keys)) and not cmp.only_keys([]), "an added key alone"
    for expected in ("summary keys + analyzer_version", "summary keys + fault_instruction",
                     "summary keys - crash_history_stats", "freeze_analysis keys + game_lock_waits",
                     "fault_instruction: None -> 'null_pointer'"):
        assert expected in lines, (expected, lines)


def test_selected_next_action() -> None:
    base = copy.deepcopy(BASE)
    base["next_action_index"] = 0
    base["recommendation_classes"][0]["kind"] = "basics"
    new = dict(base, next_action_index=1)
    lines = cmp.compare(base, new)
    assert any(line.startswith("next_action.text:") and "Share the report." in line for line in lines), lines
    assert any(line.startswith("next_action.kind:") for line in lines), lines

    reordered = copy.deepcopy(base)
    reordered["recommendations"].reverse()
    reordered["recommendation_classes"].reverse()
    reordered["next_action_index"] = 1
    assert cmp.compare(base, reordered) == ["recommendations: same entries, new order"]

    reclassified = copy.deepcopy(base)
    reclassified["recommendation_classes"][0]["kind"] = "check"
    assert cmp.compare(base, reclassified) == ["next_action.kind: 'basics' -> 'check'"]

    for index in (-1, 2, True, "0", None):
        fields = cmp.key_fields(dict(base, next_action_index=index))
        assert fields["next_action.text"] is None and fields["next_action.kind"] is None
    legacy = dict(new, recommendation_classes=[])
    assert cmp.key_fields(legacy)["next_action.text"] == base["recommendations"][1]
    assert cmp.key_fields(legacy)["next_action.kind"] is None


def test_find_dumps(tmp: Path) -> None:
    (tmp / "a.dmp").write_bytes(b"")
    (tmp / "b.dmp").write_bytes(b"")
    (tmp / "notes.txt").write_text("x")
    assert [p.name for p in cmp.find_dumps([tmp])] == ["a.dmp", "b.dmp"]
    try:
        cmp.find_dumps([tmp / "notes.txt"])
    except SystemExit:
        pass
    else:
        raise AssertionError("a file that is not a dump is refused")

    # A folder and a dump in it name that dump once.
    assert [p.name for p in cmp.find_dumps([tmp, tmp / "a.dmp"])] == ["a.dmp", "b.dmp"]


def test_dump_labels() -> None:
    labels = cmp.dump_labels([Path("p1/crash.dmp"), Path("p2/crash.dmp"), Path("p3/other.dmp")])
    assert labels == ["crash-1", "crash-2", "other"], labels
    # A number another dump already has as its name is skipped.
    labels = cmp.dump_labels([Path("p1/crash.dmp"), Path("p2/crash.dmp"), Path("p3/crash-1.dmp")])
    assert labels == ["crash-2", "crash-3", "crash-1"], labels


def test_same_named_dumps_from_different_folders(tmp: Path) -> None:
    # Dumps from two players, both named same.dmp. Only the first changes.
    for name in ("a", "b"):
        (tmp / name).mkdir()
        (tmp / name / "same.dmp").write_bytes(name.encode())
    base_cli = tmp / "base.exe"
    new_cli = tmp / "new.exe"
    base_cli.write_bytes(b"")
    new_cli.write_bytes(b"")
    changed = copy.deepcopy(BASE)
    changed["quick_answer"]["case"] = "candidate"
    out_dirs = []

    def fake_run(cli, dump, out_dir, with_crash_logger, lang="en"):
        out_dirs.append(out_dir)
        return changed if cli[0] == str(new_cli) and dump.parent.name == "a" else copy.deepcopy(BASE)

    real_run = cmp.run_analyzer
    cmp.run_analyzer = fake_run
    try:
        import contextlib
        import io
        printed = io.StringIO()
        with contextlib.redirect_stdout(printed):
            code = cmp.main(["--dumps", str(tmp / "a" / "same.dmp"), str(tmp / "b" / "same.dmp"),
                             "--base-cli", str(base_cli), "--new-cli", str(new_cli), "--lang", "en",
                             "--out", str(tmp / "out"), "--fail-on-diff", "--jobs", "1"])
    finally:
        cmp.run_analyzer = real_run
    text = printed.getvalue()
    assert code == 1, text
    assert "1 of 2 analyses changed" in text, text
    assert "en same-1: 1 change(s)" in text and "en same-2: same" in text, text
    assert f"dump: {tmp / 'a' / 'same.dmp'}" in text, text
    assert len(set(out_dirs)) == 4, out_dirs  # base and new for each dump, none shared


if __name__ == "__main__":
    import tempfile

    test_same_summary_reads_same()
    test_volatile_fields_are_left_out()
    test_scalar_and_quick_answer_changes()
    test_selected_next_action()
    test_list_changes_name_the_entries()
    test_new_and_removed_keys()
    with tempfile.TemporaryDirectory() as tmp:
        test_find_dumps(Path(tmp))
    test_dump_labels()
    with tempfile.TemporaryDirectory() as tmp:
        test_same_named_dumps_from_different_folders(Path(tmp))
    print("compare_analyses_tests: OK")

#!/usr/bin/env python3
from __future__ import annotations

import json
import os
import subprocess
import tempfile
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
HARNESS_PROJECT = REPO_ROOT / "tests" / "share_text_fixture_harness" / "ShareTextFixtureHarness.csproj"
DATA_DIR = REPO_ROOT / "tests" / "data" / "share_text_cases"


def _dotnet_arg_path(path: Path) -> str:
    # Use native paths on normal Linux/macOS/Windows runners.
    # Only convert to UNC Windows paths when running inside WSL, where
    # `dotnet` may resolve to the Windows host SDK path handling model.
    if os.environ.get("WSL_DISTRO_NAME"):
        completed = subprocess.run(
            ["wslpath", "-w", str(path)],
            cwd=str(REPO_ROOT),
            text=True,
            capture_output=True,
            check=True,
        )
        return completed.stdout.strip()
    return str(path)


def _run_harness(summary_name: str, mode: str) -> str:
    return _run_harness_on(DATA_DIR / summary_name, mode)


def _run_harness_on(summary_path: Path, mode: str) -> str:
    summary_name = summary_path.name
    completed = subprocess.run(
        [
            "dotnet",
            "run",
            "--project",
            _dotnet_arg_path(HARNESS_PROJECT),
            "--framework",
            "net8.0",
            "--",
            _dotnet_arg_path(summary_path),
            mode,
        ],
        cwd=str(REPO_ROOT),
        text=True,
        encoding="utf-8",
        capture_output=True,
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"dotnet harness failed for {summary_name} ({mode})\n"
            f"stdout:\n{completed.stdout}\n"
            f"stderr:\n{completed.stderr}"
        )
    return completed.stdout.replace("\r\n", "\n")


def _read_expected(expected_name: str) -> str:
    return (DATA_DIR / expected_name).read_text(encoding="utf-8").replace("\r\n", "\n").rstrip("\n")


def test_frame_first_community_share_fixture() -> None:
    actual = _run_harness("frame_first_summary.json", "community")
    expected = _read_expected("frame_first_community_share.expected.txt")
    assert actual == expected
    assert "Corroborated fault location" in actual
    assert "Cross-validated candidate" not in actual
    assert "Confidence: High" not in actual


def test_frame_first_summary_clipboard_fixture() -> None:
    actual = _run_harness("frame_first_summary.json", "clipboard")
    expected = _read_expected("frame_first_summary_clipboard.expected.txt")
    assert actual == expected
    assert "/tmp/" not in actual
    assert "Corroborated fault location" in actual
    assert "Confidence: High" not in actual


def test_conflicting_community_share_fixture() -> None:
    actual = _run_harness("conflicting_summary.json", "community")
    expected = _read_expected("conflicting_community_share.expected.txt")
    assert actual == expected


def test_conflicting_summary_clipboard_fixture() -> None:
    actual = _run_harness("conflicting_summary.json", "clipboard")
    expected = _read_expected("conflicting_summary_clipboard.expected.txt")
    assert actual == expected


def test_reference_clue_community_share_fixture() -> None:
    actual = _run_harness("reference_clue_summary.json", "community")
    expected = _read_expected("reference_clue_community_share.expected.txt")
    assert actual == expected


def test_reference_clue_summary_clipboard_fixture() -> None:
    actual = _run_harness("reference_clue_summary.json", "clipboard")
    expected = _read_expected("reference_clue_summary_clipboard.expected.txt")
    assert actual == expected


def test_strong_frame_related_community_share_fixture() -> None:
    actual = _run_harness("strong_frame_related_summary.json", "community")
    expected = _read_expected("strong_frame_related_community_share.expected.txt")
    assert actual == expected


def test_strong_frame_related_summary_clipboard_fixture() -> None:
    actual = _run_harness("strong_frame_related_summary.json", "clipboard")
    expected = _read_expected("strong_frame_related_summary_clipboard.expected.txt")
    assert actual == expected


def test_frame_firstchance_related_community_share_fixture() -> None:
    actual = _run_harness("frame_firstchance_related_summary.json", "community")
    expected = _read_expected("frame_firstchance_related_community_share.expected.txt")
    assert actual == expected


def test_frame_firstchance_related_summary_clipboard_fixture() -> None:
    actual = _run_harness("frame_firstchance_related_summary.json", "clipboard")
    expected = _read_expected("frame_firstchance_related_summary_clipboard.expected.txt")
    assert actual == expected


def test_frame_history_related_community_share_fixture() -> None:
    actual = _run_harness("frame_history_related_summary.json", "community")
    expected = _read_expected("frame_history_related_community_share.expected.txt")
    assert actual == expected


def test_frame_history_related_summary_clipboard_fixture() -> None:
    actual = _run_harness("frame_history_related_summary.json", "clipboard")
    expected = _read_expected("frame_history_related_summary_clipboard.expected.txt")
    assert actual == expected


def test_frame_resource_related_community_share_fixture() -> None:
    actual = _run_harness("frame_resource_related_summary.json", "community")
    expected = _read_expected("frame_resource_related_community_share.expected.txt")
    assert actual == expected


def test_frame_resource_related_summary_clipboard_fixture() -> None:
    actual = _run_harness("frame_resource_related_summary.json", "clipboard")
    expected = _read_expected("frame_resource_related_summary_clipboard.expected.txt")
    assert actual == expected


def test_standalone_stackwalk_related_community_share_fixture() -> None:
    actual = _run_harness("standalone_stackwalk_related_summary.json", "community")
    expected = _read_expected("standalone_stackwalk_related_community_share.expected.txt")
    assert actual == expected


def test_standalone_stackwalk_related_summary_clipboard_fixture() -> None:
    actual = _run_harness("standalone_stackwalk_related_summary.json", "clipboard")
    expected = _read_expected("standalone_stackwalk_related_summary_clipboard.expected.txt")
    assert actual == expected


def test_gpu_wait_hang_follows_report_next_action() -> None:
    # The report's NextAction (next_action_index) is the [Main thread] line;
    # neither the candidate, the [Hang] line nor the generic [Conflict] line
    # may replace it in the viewer or the share text.
    for mode in ("community", "clipboard"):
        actual = _run_harness("gpu_wait_hang_summary.json", mode)
        next_lines = [line for line in actual.split("\n") if "Next action:" in line]
        assert len(next_lines) == 1, actual
        assert "The main thread was waiting for the GPU" in next_lines[0], actual
        assert "(detected: ENB)" in next_lines[0], actual


def test_quick_answer_leads_the_share_texts() -> None:
    # Players read the first lines: right under the title come the quick
    # answer's headline, what to try, why it is not certain and how it crashed;
    # the detailed fields follow unchanged.
    community = _run_harness("quick_answer_summary.json", "community")
    assert community == _read_expected("quick_answer_community_share.expected.txt")
    lines = community.split("\n")
    assert lines[1].startswith("💬 Quick answer: The game crashed to desktop (CTD)."), community
    assert lines[1].endswith(" — possible"), community
    assert "What to try: Disable or update Precision - Accurate Melee Collisions" in lines[2], community
    assert "Why not certain:" in lines[3], community
    assert "How it crashed: It crashed while using an object" in lines[4], community

    clipboard = _run_harness("quick_answer_summary.json", "clipboard")
    assert clipboard == _read_expected("quick_answer_summary_clipboard.expected.txt")
    lines = clipboard.split("\n")
    assert lines[1].startswith("Quick answer: The game crashed to desktop (CTD)."), clipboard
    assert lines[2].startswith("What to try: "), clipboard
    assert lines[3].startswith("Why not certain: "), clipboard
    assert lines[4].startswith("How it crashed: "), clipboard
    assert lines[6] == "Details", clipboard
    assert lines[7].startswith("Dump: "), clipboard


def test_summary_without_quick_answer_keeps_its_share_texts() -> None:
    # Summaries written before the quick answer existed read as before.
    for mode in ("community", "clipboard"):
        actual = _run_harness("frame_first_summary.json", mode)
        assert "Quick answer" not in actual, actual


def test_recommendation_classes_decide_the_sections() -> None:
    # ADR-0018: the analyzer's recommendation_classes place each line, whatever
    # its text tag; a group this viewer does not know falls back to the text,
    # and the next action treats an actionable_candidate kind as candidate-level.
    actual = _run_harness("recommendation_classes_summary.json", "groups")
    assert actual == (
        "immediate: Disable Foo.dll and retest.\n"
        "immediate: Look at the load order.\n"
        "verify: Read the linked article first.\n"
        "recapture: ExceptionCode=0xC0000005.\n"
        "next: Update or isolate Foo.dll first\n"
    ), actual

    # Without recommendation_classes (older analyzers) the text tags decide.
    summary = json.loads((DATA_DIR / "recommendation_classes_summary.json").read_text(encoding="utf-8"))
    del summary["recommendation_classes"]
    with tempfile.TemporaryDirectory() as tmp:
        older = Path(tmp) / "older_summary.json"
        older.write_text(json.dumps(summary), encoding="utf-8")
        actual = _run_harness_on(older, "groups")
    assert actual == (
        "immediate: Read the linked article first.\n"
        "immediate: Look at the load order.\n"
        "verify: Disable Foo.dll and retest.\n"
        "verify: ExceptionCode=0xC0000005.\n"
        "next: Disable Foo.dll and retest.\n"
    ), actual


def test_forced_close_is_not_shared_as_a_ctd() -> None:
    # ADR-0022: a crash right after Alt+F4 is shared as a forced-close error,
    # not under the CTD header, with nothing to do as its next action.
    community = _run_harness("forced_close_summary.json", "community")
    lines = community.split("\n")
    assert lines[0] == "⚪ Skyrim Forced-Close Error Report — SkyrimDiag", community
    assert lines[1].startswith("💬 Quick answer: This error happened while you force-quit the game (Alt+F4)."), community
    assert "👉 What to try: Nothing to do." in lines, community
    assert "🔍 Category: FORCED_CLOSE" in lines, community
    assert "CTD Report" not in community, community
    next_action = [line for line in lines if line.startswith("🛠️ Next action: ")]
    assert next_action and next_action[0].startswith("🛠️ Next action: This error happened while you force-quit"), community

    clipboard = _run_harness("forced_close_summary.json", "clipboard")
    assert clipboard.split("\n")[1].startswith("Quick answer: This error happened while you force-quit"), clipboard


def _run_harness_on_modified(base_name: str, modify, mode: str) -> str:
    summary = json.loads((DATA_DIR / base_name).read_text(encoding="utf-8"))
    modify(summary)
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / base_name
        path.write_text(json.dumps(summary, ensure_ascii=False), encoding="utf-8")
        return _run_harness_on(path, mode)


def test_ambiguous_crash_logger_log_names_no_mod() -> None:
    # A log that may be another crash's names no culprit (the analyzer's
    # FirstCulpritObjectRef); its refs were still shown as "Referenced mod".
    def with_ref(ambiguous: bool):
        def modify(summary: dict) -> None:
            summary["actionable_candidates"] = []
            summary["crash_logger"]["object_refs"] = [{
                "esp_name": "NpcMod.esp",
                "best_object_type": "TESNPC*",
                "best_location": "RCX",
                "object_name": "",
                "form_id": "",
                "ref_count": 1,
                "relevance_score": 18,
            }]
            summary["crash_logger"]["pairing"] = {"ambiguous": ambiguous, "confidence": "low" if ambiguous else "high"}
        return modify

    paired = _run_harness_on_modified("reference_clue_summary.json", with_ref(False), "community")
    assert "📌 Referenced mod (ESP): NpcMod.esp" in paired, paired
    ambiguous = _run_harness_on_modified("reference_clue_summary.json", with_ref(True), "community")
    assert "📌 Referenced mod (ESP): NpcMod.esp" not in ambiguous, ambiguous


def test_ctd_named_freeze_stays_a_ctd() -> None:
    # The header follows the analyzer's flags, not words in the summary
    # sentence: a CTD in "FreezeFix.dll" was shared as a freeze.
    def modify(summary: dict) -> None:
        summary["summary_sentence"] = "Crash in FreezeFix.dll (Freeze Fix): possible snapshot of a freeze."

    community = _run_harness_on_modified("frame_first_summary.json", modify, "community")
    assert community.split("\n")[0] == "🔴 Skyrim CTD Report — SkyrimDiag", community


def test_callstack_label_follows_the_analyzer() -> None:
    # The analyzer's standalone_callstack decides (its stack weight); the
    # viewer's own rule compared the total score, history included.
    def not_standalone(summary: dict) -> None:
        summary["actionable_candidates"][0]["standalone_callstack"] = False

    community = _run_harness_on_modified("standalone_stackwalk_related_summary.json", not_standalone, "community")
    assert "🧩 Tullius callstack:" not in community, community
    assert "Next action: Tullius callstack:" not in community, community
    expected = _read_expected("standalone_stackwalk_related_community_share.expected.txt")
    assert "🧩 Tullius callstack:" in expected, expected


def test_viewer_options_match_the_analyzer() -> None:
    # No symbols flag leaves SKYRIMDIAG_ALLOW_ONLINE_SYMBOLS to the analyzer
    # (null), as the CLI does; ko, kor and korean are Korean in both.
    actual = _run_harness("frame_first_summary.json", "options")
    assert actual == "null|True|False|True|True|True|False|False", actual


def test_viewer_survives_fields_of_another_type() -> None:
    # TryGetProperty, TryGetInt32 and GetString throw on a node or value of
    # another kind: an array entry that is a string, or the incident's
    # recapture_evaluation copied as is from another version's manifest.
    def modify(summary: dict) -> None:
        summary["actionable_candidates"].append("not an object")
        summary["crash_logger"].setdefault("object_refs", []).append("not an object")
        summary["incident"] = {"recapture_evaluation": {"escalation_level": "2", "triggered": "yes", "reasons": [1]}}
        summary["dump_identity"] = {"schema": 5, "sha256": 7, "size_bytes": "big", "last_write_time_utc_100ns": "x"}

    community = _run_harness_on_modified("frame_first_summary.json", modify, "community")
    assert community.split("\n")[0] == "🔴 Skyrim CTD Report — SkyrimDiag", community


def test_viewer_reads_helper_ini_like_the_helper() -> None:
    # IniText.h FindValue: one matching pair of " or ' removed, the first
    # section of that name only, a header up to its "]". The viewer stripped
    # double quotes only and searched every section of that name.
    cases = [
        ("[SkyrimDiagHelper]\nOutputDir='D:\\Logs'\n", "D:\\Logs"),
        ("[SkyrimDiagHelper]\nOutputDir=\"D:\\Logs\"\n", "D:\\Logs"),
        ("[SkyrimDiagHelper] ; settings\nOutputDir=D:\\Logs\n", "D:\\Logs"),
        ("[SkyrimDiagHelper]\nMaxCrashDumps=3\n[Other]\nOutputDir=E:\\No\n[SkyrimDiagHelper]\nOutputDir=F:\\Later\n", "<none>"),
        ("[SkyrimDiagHelper]\nOutputDir=\n", ""),
    ]
    with tempfile.TemporaryDirectory() as tmp:
        for i, (text, expected) in enumerate(cases):
            path = Path(tmp) / f"helper_{i}.ini"
            path.write_text(text, encoding="utf-8")
            actual = _run_harness_on(path, "ini-outputdir")
            assert actual == expected, (text, actual)


def test_cleared_summary_produces_no_share_text() -> None:
    actual = _run_harness("frame_first_summary.json", "cleared")
    assert actual == "", actual


if __name__ == "__main__":
    test_frame_first_community_share_fixture()
    test_frame_first_summary_clipboard_fixture()
    test_conflicting_community_share_fixture()
    test_conflicting_summary_clipboard_fixture()
    test_reference_clue_community_share_fixture()
    test_reference_clue_summary_clipboard_fixture()
    test_strong_frame_related_community_share_fixture()
    test_strong_frame_related_summary_clipboard_fixture()
    test_frame_firstchance_related_community_share_fixture()
    test_frame_firstchance_related_summary_clipboard_fixture()
    test_frame_history_related_community_share_fixture()
    test_frame_history_related_summary_clipboard_fixture()
    test_frame_resource_related_community_share_fixture()
    test_frame_resource_related_summary_clipboard_fixture()
    test_standalone_stackwalk_related_community_share_fixture()
    test_standalone_stackwalk_related_summary_clipboard_fixture()
    test_gpu_wait_hang_follows_report_next_action()
    test_quick_answer_leads_the_share_texts()
    test_summary_without_quick_answer_keeps_its_share_texts()
    test_recommendation_classes_decide_the_sections()
    test_forced_close_is_not_shared_as_a_ctd()
    test_ambiguous_crash_logger_log_names_no_mod()
    test_ctd_named_freeze_stays_a_ctd()
    test_callstack_label_follows_the_analyzer()
    test_viewer_options_match_the_analyzer()
    test_viewer_survives_fields_of_another_type()
    test_viewer_reads_helper_ini_like_the_helper()
    test_cleared_summary_produces_no_share_text()
    print("share_text_fixture_tests: OK")

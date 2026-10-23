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
    # answer's headline, what to try and why it is not certain; the detailed
    # fields follow unchanged.
    community = _run_harness("quick_answer_summary.json", "community")
    assert community == _read_expected("quick_answer_community_share.expected.txt")
    lines = community.split("\n")
    assert lines[1].startswith("💬 Quick answer: The game crashed to desktop (CTD)."), community
    assert lines[1].endswith(" — possible"), community
    assert "What to try: Disable or update Precision - Accurate Melee Collisions" in lines[2], community
    assert "Why not certain:" in lines[3], community

    clipboard = _run_harness("quick_answer_summary.json", "clipboard")
    assert clipboard == _read_expected("quick_answer_summary_clipboard.expected.txt")
    lines = clipboard.split("\n")
    assert lines[1].startswith("Quick answer: The game crashed to desktop (CTD)."), clipboard
    assert lines[2].startswith("What to try: "), clipboard
    assert lines[3].startswith("Why not certain: "), clipboard
    assert lines[5] == "Details", clipboard
    assert lines[6].startswith("Dump: "), clipboard


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
    test_cleared_summary_produces_no_share_text()
    print("share_text_fixture_tests: OK")

#!/usr/bin/env python3
from __future__ import annotations

import subprocess
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
HARNESS_PROJECT = REPO_ROOT / "tests" / "share_text_fixture_harness" / "ShareTextFixtureHarness.csproj"
DATA_DIR = REPO_ROOT / "tests" / "data" / "share_text_cases"


def _windows_path(path: Path) -> str:
    completed = subprocess.run(
        ["wslpath", "-w", str(path)],
        cwd=str(REPO_ROOT),
        text=True,
        capture_output=True,
        check=True,
    )
    return completed.stdout.strip()


def _run_harness(summary_name: str, mode: str) -> str:
    summary_path = DATA_DIR / summary_name
    completed = subprocess.run(
        [
            "dotnet",
            "run",
            "--project",
            _windows_path(HARNESS_PROJECT),
            "--framework",
            "net8.0",
            "--",
            _windows_path(summary_path),
            mode,
        ],
        cwd=str(REPO_ROOT),
        text=True,
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


def test_frame_first_summary_clipboard_fixture() -> None:
    actual = _run_harness("frame_first_summary.json", "clipboard")
    expected = _read_expected("frame_first_summary_clipboard.expected.txt")
    assert actual == expected


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


if __name__ == "__main__":
    test_frame_first_community_share_fixture()
    test_frame_first_summary_clipboard_fixture()
    test_conflicting_community_share_fixture()
    test_conflicting_summary_clipboard_fixture()
    test_reference_clue_community_share_fixture()
    test_reference_clue_summary_clipboard_fixture()
    print("share_text_fixture_tests: OK")

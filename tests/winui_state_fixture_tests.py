#!/usr/bin/env python3
from __future__ import annotations

from pathlib import Path

from dotnet_harness import Harness


REPO_ROOT = Path(__file__).resolve().parents[1]
PROJECT = REPO_ROOT / "tests" / "winui_state_fixture_harness" / "WinUIStateFixtureHarness.csproj"


def main() -> None:
    completed = Harness(PROJECT).run()
    if completed.returncode != 0:
        raise RuntimeError(
            "WinUI state harness failed\n"
            f"stdout:\n{completed.stdout}\n"
            f"stderr:\n{completed.stderr}"
        )
    assert "winui_state_fixture_harness: OK" in completed.stdout
    print("winui_state_fixture_tests: OK")


if __name__ == "__main__":
    main()

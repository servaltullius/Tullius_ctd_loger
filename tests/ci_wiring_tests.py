"""Guard that the static-analysis and test gates are actually invoked by CI.

Every gate covered here already existed as configuration before it ran anywhere:
`.clang-tidy` was committed and referenced in the changelog while nothing
executed it, the fuzz harnesses built only on a developer's machine, and the
Windows-only ctest targets were invisible to the Linux job. Configuration that
no workflow calls looks like coverage in review and provides none, so these
tests assert the call sites rather than the config files.

Both the normal CI caller and the tag-triggered release caller must enable the
full Linux gate set. Version tags are excluded from ci.yml, so relying on a
previous branch run would let a directly tagged commit bypass those checks.
"""

import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
WORKFLOWS = REPO_ROOT / ".github" / "workflows"


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def check_clang_tidy_is_wired(failures: list[str]) -> None:
    tidy_config = _read(REPO_ROOT / ".clang-tidy")
    if "WarningsAsErrors: '*'" not in tidy_config:
        failures.append(
            ".clang-tidy must keep WarningsAsErrors: '*' so a new finding fails "
            "CI instead of scrolling past in the log"
        )

    runner = REPO_ROOT / "scripts" / "run_clang_tidy.sh"
    if not runner.is_file():
        failures.append("scripts/run_clang_tidy.sh is missing")
        return

    runner_text = _read(runner)
    # The covered set is derived from the compile database, so a newly added
    # analyzer source is linted without anyone remembering to list it.
    if "compile_commands.json" not in runner_text:
        failures.append(
            "run_clang_tidy.sh must derive its file list from the compile "
            "database, not from a hardcoded list that new sources bypass"
        )

    linux = _read(WORKFLOWS / "linux-tests.yml")
    if "run_clang_tidy:" not in linux:
        failures.append("linux-tests.yml is missing the run_clang_tidy input")
    if "scripts/run_clang_tidy.sh" not in linux:
        failures.append("linux-tests.yml never calls scripts/run_clang_tidy.sh")

    for name in ("ci.yml", "release.yml"):
        caller = _read(WORKFLOWS / name)
        if "run_clang_tidy: true" not in caller:
            failures.append(f"{name} does not enable the clang-tidy job")


def check_fuzzers_are_wired(failures: list[str]) -> None:
    linux = _read(WORKFLOWS / "linux-tests.yml")
    for target in ("fuzz_crashlogger_parser", "fuzz_wct_parser"):
        if target not in linux:
            failures.append(f"linux-tests.yml never runs {target}")

    if "-max_total_time=" not in linux:
        failures.append(
            "the fuzz job must bound its run time; an unbounded fuzzer in CI "
            "hangs the workflow instead of reporting a regression"
        )

    for name in ("ci.yml", "release.yml"):
        caller = _read(WORKFLOWS / name)
        if "run_fuzz: true" not in caller:
            failures.append(f"{name} does not enable the fuzz job")


def check_sanitizers_are_wired(failures: list[str]) -> None:
    for name in ("ci.yml", "release.yml"):
        caller = _read(WORKFLOWS / name)
        if "run_asan: true" not in caller:
            failures.append(f"{name} does not enable the ASan + UBSan job")


def check_windows_tests_are_wired(failures: list[str]) -> None:
    # if(WIN32) test targets cannot run in the Linux job, so if these two
    # workflows skip ctest the targets never execute anywhere.
    for name in ("ci.yml", "release.yml"):
        text = _read(WORKFLOWS / name)
        if "ctest --test-dir build-win" not in text:
            failures.append(f"{name} builds Windows targets but never runs ctest on them")


def main() -> int:
    failures: list[str] = []
    check_clang_tidy_is_wired(failures)
    check_fuzzers_are_wired(failures)
    check_sanitizers_are_wired(failures)
    check_windows_tests_are_wired(failures)

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1

    print("ci wiring tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

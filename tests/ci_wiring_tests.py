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

import importlib.util
import json
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
WORKFLOWS = REPO_ROOT / ".github" / "workflows"


def _read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def _load_script(name: str):
    path = REPO_ROOT / "scripts" / name
    spec = importlib.util.spec_from_file_location(f"skydiag_test_{path.stem}", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"failed to load script: {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def check_clang_tidy_is_wired(failures: list[str]) -> None:
    tidy_config = _read(REPO_ROOT / ".clang-tidy")
    if "WarningsAsErrors: '*'" not in tidy_config:
        failures.append(
            ".clang-tidy must keep WarningsAsErrors: '*' so a new finding fails "
            "CI instead of scrolling past in the log"
        )

    linux_runner = REPO_ROOT / "scripts" / "run_clang_tidy.sh"
    windows_runner = REPO_ROOT / "scripts" / "run_clang_tidy.py"
    if not linux_runner.is_file():
        failures.append("scripts/run_clang_tidy.sh is missing")
        return
    if not windows_runner.is_file():
        failures.append("scripts/run_clang_tidy.py is missing")
        return

    linux_runner_text = _read(linux_runner)
    if "compile_commands.json" not in linux_runner_text:
        failures.append(
            "run_clang_tidy.sh must derive its file list from the compile "
            "database, not from a hardcoded list that new sources bypass"
        )

    windows_runner_text = _read(windows_runner)
    for contract in (
        "PRODUCTION_DIRS",
        "dump_tool/src",
        "helper/src",
        "plugin/src",
        "production sources missing from compile database",
        "production source has no product target compile entry",
        "SkyrimDiagPluginInfo.cpp",
    ):
        if contract not in windows_runner_text:
            failures.append(
                f"full Windows clang-tidy runner missing coverage contract: {contract}"
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
        if "scripts/run_clang_tidy.py" not in caller:
            failures.append(
                f"{name} does not lint the complete Windows production source set"
            )
        if "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON" not in caller:
            failures.append(
                f"{name} does not export the Windows compile database"
            )


def check_clang_tidy_rejects_test_only_entries(failures: list[str]) -> None:
    runner = _load_script("run_clang_tidy.py")
    with tempfile.TemporaryDirectory(prefix="skydiag_tidy_product_entry_") as temp:
        root = Path(temp)
        alias_anchor = root / "path_alias"
        alias_anchor.mkdir()
        repo_alias = alias_anchor / ".."
        source = root / "dump_tool" / "src" / "OnlyInTest.cpp"
        source.parent.mkdir(parents=True)
        source.write_text("int only_in_test = 0;\n", encoding="utf-8")
        (root / "helper" / "src").mkdir(parents=True)
        (root / "plugin" / "src").mkdir(parents=True)

        build = root / "build"
        generated = build / "plugin" / "generated" / "SkyrimDiagPluginInfo.cpp"
        generated.parent.mkdir(parents=True)
        generated.write_text("int generated_plugin_info = 0;\n", encoding="utf-8")
        database = [
            {
                "directory": str(build),
                "file": str(source),
                "command": (
                    "clang++ -o CMakeFiles/skydiag_test_only.dir/OnlyInTest.cpp.obj "
                    f"-c {source}"
                ),
            },
            {
                "directory": str(build),
                "file": str(generated),
                "command": (
                    "clang++ -o CMakeFiles/SkyrimDiag.dir/"
                    f"SkyrimDiagPluginInfo.cpp.obj -c {generated}"
                ),
            },
        ]
        (build / "compile_commands.json").write_text(
            json.dumps(database), encoding="utf-8"
        )
        try:
            runner.prepare_full_production_database(repo_alias, build)
        except ValueError as exc:
            if "no product target compile entry" not in str(exc):
                failures.append(
                    "run_clang_tidy.py rejected a test-only entry for the wrong reason: "
                    f"{exc}"
                )
        else:
            failures.append(
                "run_clang_tidy.py treated a test-only compile entry as production coverage"
            )

        database[0]["command"] = (
            "clang++ -o CMakeFiles/SkyrimDiagDumpToolCore.dir/"
            f"OnlyInTest.cpp.obj -c {source}"
        )
        (build / "compile_commands.json").write_text(
            json.dumps(database), encoding="utf-8"
        )
        try:
            runner.prepare_full_production_database(repo_alias, build)
        except (OSError, ValueError) as exc:
            failures.append(
                "run_clang_tidy.py rejected the production dump-tool core target: "
                f"{exc}"
            )


def check_release_notes_rc_fallback(failures: list[str]) -> None:
    resolver = _load_script("resolve_release_notes.py")
    release_workflow = _read(WORKFLOWS / "release.yml")
    if "scripts/resolve_release_notes.py" not in release_workflow:
        failures.append("release.yml does not use the tested release-notes resolver")

    with tempfile.TemporaryDirectory(prefix="skydiag_release_notes_") as temp:
        root = Path(temp)
        drafts = root / "docs" / "release" / "drafts"
        drafts.mkdir(parents=True)
        (drafts / "v1.2.3.md").write_text("base draft\n", encoding="utf-8")
        (root / "CHANGELOG.md").write_text(
            "## v1.2.3\nbase changelog\n\n## v1.2.2\nold\n",
            encoding="utf-8",
        )

        rc_notes = resolver.resolve_release_notes(root, "v1.2.3-rc4")
        if rc_notes != "base draft\n":
            failures.append("RC tag did not resolve the base-version release draft")

        (drafts / "v1.2.3-rc4.md").write_text("exact RC draft\n", encoding="utf-8")
        exact_notes = resolver.resolve_release_notes(root, "v1.2.3-rc4")
        if exact_notes != "exact RC draft\n":
            failures.append("exact RC release draft did not take precedence")

        beta_notes = resolver.resolve_release_notes(root, "v1.2.3-beta1")
        if beta_notes != "Release v1.2.3-beta1\n":
            failures.append(
                "non-RC prerelease incorrectly fell back to base-version release notes"
            )


def check_release_prerelease_classification(failures: list[str]) -> None:
    contract = _load_script("release_contract.py")
    stable_cases = ("v0.2.58", "v1.0.0")
    prerelease_cases = (
        "v0.2.58-rc1",
        "v0.2.58-beta.2",
        "v0.2.58-preview-nightly",
    )
    for tag in stable_cases:
        if contract.release_tag_is_prerelease(tag):
            failures.append(f"stable tag was classified as a prerelease: {tag}")
    for tag in prerelease_cases:
        if not contract.release_tag_is_prerelease(tag):
            failures.append(f"suffixed tag was not classified as a prerelease: {tag}")
    for tag in ("v0.2", "v0.2.58-", "v01.2.58", "release-v0.2.58"):
        try:
            contract.release_tag_is_prerelease(tag)
        except ValueError:
            continue
        failures.append(f"invalid release tag was accepted: {tag}")

    release_workflow = _read(WORKFLOWS / "release.yml")
    for contract_text in (
        "release_tag_is_prerelease",
        "is_prerelease=$isPrerelease",
        '$actualPrerelease = [bool]$release.isPrerelease',
        "$actualPrerelease -ne $expectedPrerelease",
    ):
        if contract_text not in release_workflow:
            failures.append(
                "release.yml is missing bidirectional prerelease contract: "
                f"{contract_text}"
            )


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
    check_clang_tidy_rejects_test_only_entries(failures)
    check_release_notes_rc_fallback(failures)
    check_release_prerelease_classification(failures)
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

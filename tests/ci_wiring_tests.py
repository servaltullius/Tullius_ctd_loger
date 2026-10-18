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
import os
import re
import shutil
import subprocess
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


def check_clang_tidy_header_filter(tidy_config: str, failures: list[str]) -> None:
    """HeaderFilterRegex must reach project headers under either separator.

    The Windows compile database spells include directories with backslashes,
    so clang reports a header as G:\\...\\helper\\src\\CrashCapture.h, with a
    forward slash only where the #include spelling has one. A "helper/" filter
    matched no Windows header and every finding there was dropped as non-user
    code. Python re stands in for llvm::Regex here; the pattern sticks to
    syntax both read the same way ([\\\\/] is "backslash or slash" in each).
    """
    match = re.search(r"^HeaderFilterRegex:\s*'((?:[^']|'')*)'\s*$", tidy_config, re.MULTILINE)
    if match is None:
        failures.append(".clang-tidy must set HeaderFilterRegex as a single-quoted YAML string")
        return
    # Inside YAML single quotes a backslash is literal and '' is a quote.
    pattern = match.group(1).replace("''", "'")
    try:
        header_filter = re.compile(pattern)
    except re.error as exc:
        failures.append(f".clang-tidy HeaderFilterRegex does not compile: {exc}")
        return

    project_headers = (
        r"G:\r\helper\include\SkyrimDiagHelper/ManualCaptureHotkey.h",
        r"G:\r\helper\src\CrashCapture.h",
        r"G:\r\plugin\include\SkyrimDiag/CrashHandler.h",
        r"G:\r\dump_tool\src\WctTypes.h",
        r"G:\r\shared\SkyrimDiagWctWait.h",
        "/home/runner/work/r/dump_tool/src/WctTypes.h",
        "/home/runner/work/r/helper/src/CrashCapture.h",
        "/home/runner/work/r/shared/SkyrimDiagWctWait.h",
    )
    for path in project_headers:
        if header_filter.search(path) is None:
            failures.append(
                f".clang-tidy HeaderFilterRegex skips project header {path}; "
                "its findings would never fail the gate"
            )

    dependency_headers = (
        r"D:\a\r\build-win\vcpkg_installed\x64-windows-static\include\fmt\core.h",
        "/home/runner/work/r/build-tidy/vcpkg_installed/x64-linux/include/fmt/core.h",
    )
    for path in dependency_headers:
        if header_filter.search(path) is not None:
            failures.append(
                f".clang-tidy HeaderFilterRegex matches dependency header {path}"
            )


def check_clang_tidy_is_wired(failures: list[str]) -> None:
    tidy_config = _read(REPO_ROOT / ".clang-tidy")
    if "WarningsAsErrors: '*'" not in tidy_config:
        failures.append(
            ".clang-tidy must keep WarningsAsErrors: '*' so a new finding fails "
            "CI instead of scrolling past in the log"
        )
    check_clang_tidy_header_filter(tidy_config, failures)

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

        # The release workflow must not publish that one-line placeholder.
        if "--strict" not in release_workflow:
            failures.append("release.yml does not resolve release notes with --strict")
        out = root / "notes.md"
        if resolver.main(["--repo-root", str(root), "--tag", "v1.2.3-beta1", "--output", str(out), "--strict"]) == 0:
            failures.append("--strict accepted a tag with no draft or CHANGELOG section")
        if resolver.main(["--repo-root", str(root), "--tag", "v1.2.3-rc4", "--output", str(out), "--strict"]) != 0:
            failures.append("--strict rejected a tag that has a draft")


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
    for target in ("fuzz_crashlogger_parser", "fuzz_wct_parser", "fuzz_skse_log_parser"):
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


def check_windows_build_retries_once(failures: list[str]) -> None:
    # The runner sometimes fails to link a freshly built test ("Access is
    # denied" / "used by another process"); without a retry the whole job fails.
    for name in ("ci.yml", "release.yml"):
        text = _read(WORKFLOWS / name)
        if "Build failed once; retrying the remaining steps." not in text:
            failures.append(f"{name} does not retry the Windows build after a transient link failure")


def check_docs_only_skip_is_pr_only(failures: list[str]) -> None:
    # Docs-only PRs may skip the Windows job, but a push to main and the
    # release workflow must always build, test and package.
    ci = _read(WORKFLOWS / "ci.yml")
    if "needs.changes.outputs.code == 'true'" not in ci:
        failures.append("ci.yml Windows job is not gated by the docs-only check")
    if 'if [ "$EVENT_NAME" = "pull_request" ]; then' not in ci:
        failures.append("ci.yml docs-only check must apply to pull requests only")
    if "          code=true" not in ci:
        failures.append("ci.yml docs-only check must default to running the Windows job")
    release = _read(WORKFLOWS / "release.yml")
    if "needs.changes" in release:
        failures.append("release.yml must never skip the Windows job")


def check_pull_request_tidy_scope(failures: list[str]) -> None:
    # Pull requests lint only the production sources they change, or everything
    # when a header or build setting changes; main pushes and releases lint all.
    runner = _load_script("run_clang_tidy.py")
    labels = ["dump_tool/src/A.cpp", "helper/src/B.cpp", "plugin/src/C.cpp"]
    cases = [
        (["dump_tool/src/A.cpp", "docs/x.md"], [0]),
        (["README.md"], []),
        (["dump_tool/src/A.cpp", "dump_tool/src/A.h"], None),
        (["shared/SkyrimDiagRtti.h"], None),
        ([".clang-tidy"], None),
        (["helper/CMakeLists.txt"], None),
        (["scripts/run_clang_tidy.py"], None),
        (["plugin/src/PluginInfo.cpp.in"], None),
    ]
    for changed, expected in cases:
        got = runner.select_changed_sources(changed, labels)
        if got != expected:
            failures.append(f"run_clang_tidy.py scope for {changed}: expected {expected}, got {got}")
    ci = _read(WORKFLOWS / "ci.yml")
    if '$env:EVENT_NAME -eq "pull_request"' not in ci or "--changed-since" not in ci:
        failures.append("ci.yml must narrow clang-tidy for pull requests only")
    if "--changed-since" in _read(WORKFLOWS / "release.yml"):
        failures.append("release.yml must always lint the full production set")


def check_compiler_cache_is_ci_only(failures: list[str]) -> None:
    # CI caches compiles with sccache, which needs /Z7 instead of /Zi; the
    # release workflow must keep building every object from scratch.
    ci = _read(WORKFLOWS / "ci.yml")
    if re.search(r"uses: mozilla-actions/sccache-action@[0-9a-f]{40}\b", ci) is None:
        failures.append("ci.yml must pin sccache-action to a commit SHA")
    for needed in (
        "-DCMAKE_C_COMPILER_LAUNCHER=sccache",
        "-DCMAKE_CXX_COMPILER_LAUNCHER=sccache",
        'SCCACHE_GHA_ENABLED: "true"',
        "-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded",
        "-DCMAKE_POLICY_DEFAULT_CMP0141=NEW",
        'SCCACHE_IGNORE_SERVER_IO_ERROR: "1"',
    ):
        if needed not in ci:
            failures.append(f"ci.yml compiler cache is missing {needed}")
    if "sccache" in _read(WORKFLOWS / "release.yml"):
        failures.append("release.yml must not use the compiler cache")


def _job_blocks(text: str) -> dict[str, str]:
    """Top-level jobs of a workflow, keyed by id, as their raw text."""
    lines = text.splitlines()
    start = next((i for i, line in enumerate(lines) if line.rstrip() == "jobs:"), None)
    if start is None:
        return {}
    jobs: dict[str, list[str]] = {}
    current = None
    for line in lines[start + 1:]:
        match = re.match(r"^  ([A-Za-z0-9_-]+):\s*$", line)
        if match:
            current = match.group(1)
            jobs[current] = []
        elif line and not line.startswith(" ") and not line.startswith("#"):
            break
        elif current is not None:
            jobs[current].append(line)
    return {name: "\n".join(body) for name, body in jobs.items()}


def check_workflows_are_pinned_and_bounded(failures: list[str]) -> None:
    # A third-party action moved by its owner, or vcpkg master moving on,
    # changes the build without a commit here; a hung step otherwise holds a
    # runner for the 6-hour default.
    for path in sorted(WORKFLOWS.glob("*.yml")):
        text = _read(path)
        for match in re.finditer(r"uses:\s*([^\s#]+)@([^\s#]+)", text):
            action, ref = match.groups()
            if action.startswith(("actions/", "./")):
                continue
            if re.fullmatch(r"[0-9a-f]{40}", ref) is None:
                failures.append(f"{path.name}: {action} must be pinned to a commit SHA, not {ref}")
        for job, body in _job_blocks(text).items():
            if re.search(r"^    runs-on:", body, re.M) and not re.search(r"^    timeout-minutes:\s*\d+", body, re.M):
                failures.append(f"{path.name}: job {job} needs a job-level timeout-minutes")
            # The plugin builds CommonLibSSE-NG from the extern/ submodule.
            if "bootstrap-vcpkg" in body and not re.search(r"^\s+submodules:\s*true\b", body, re.M):
                failures.append(f"{path.name}: job {job} builds the plugin but does not check out submodules")
        if "git clone https://github.com/microsoft/vcpkg" in text:
            if "'default-registry'.baseline" not in text or "checkout --quiet --detach $baseline" not in text:
                failures.append(f"{path.name}: the vcpkg clone must be checked out at the manifest baseline")


def check_dump_stress_repeats_only_after_a_build(failures: list[str]) -> None:
    # A failed build would otherwise show up as a second, misleading ctest
    # failure in every shard.
    text = _read(WORKFLOWS / "dump-stress.yml")
    if "steps.build.outcome == 'success'" not in text:
        failures.append("dump-stress.yml repeats tests even after a failed build")


def _docs_only_detector_script() -> str:
    lines = _read(WORKFLOWS / "ci.yml").splitlines()
    start = next(i for i, line in enumerate(lines) if line.strip() == "id: diff")
    run = next(i for i in range(start, len(lines)) if lines[i].strip() == "run: |")
    indent = len(lines[run]) - len(lines[run].lstrip()) + 2
    body: list[str] = []
    for line in lines[run + 1:]:
        if line.strip() and len(line) - len(line.lstrip()) < indent:
            break
        body.append(line[indent:])
    return "\n".join(body) + "\n"


def _find_bash() -> str | None:
    """A bash that runs the detector the way the CI runner's Git Bash does.

    On Windows, PATH usually lists C:\\Windows\\System32 first, so
    shutil.which("bash") returns the WSL launcher there. WSL reads the
    Windows GITHUB_OUTPUT path as a relative file name, so the detector's
    output never reached the test (2026-10-13: the test failed whenever
    ctest ran from PowerShell or cmd and passed from Git Bash).
    """
    if os.name != "nt":
        return shutil.which("bash")
    system_root = Path(os.environ.get("SystemRoot", r"C:\Windows")).resolve()
    candidates: list[Path] = []
    git = shutil.which("git")
    if git:
        # <Git>\cmd\git.exe or <Git>\mingw64\bin\git.exe -> <Git>\bin\bash.exe
        for parent in Path(git).resolve().parents:
            candidates += [parent / "bin" / "bash.exe", parent / "usr" / "bin" / "bash.exe"]
    for entry in os.environ.get("PATH", "").split(os.pathsep):
        if entry:
            candidates.append(Path(entry) / "bash.exe")
    for candidate in candidates:
        try:
            resolved = candidate.resolve()
        except OSError:
            continue
        if resolved.is_file() and system_root not in resolved.parents:
            return str(resolved)
    return None


def check_docs_only_detector_behaviour(failures: list[str]) -> None:
    # Run the detector itself on a scratch repository.
    bash = _find_bash()
    git = shutil.which("git")
    if not bash or not git:
        print("ci_wiring_tests: bash/git not found; docs-only detector behaviour not checked")
        return
    script = _docs_only_detector_script()
    cases = [
        ("docs only", {"README.md": "b", "docs/a.md": "b"}, [], "false"),
        ("non-ASCII doc path", {"doc/안내.md": "b"}, [], "false"),
        ("code change", {"src/a.cpp": "b"}, [], "true"),
        ("script moved into docs", {}, [("scripts/tool.py", "docs/tool.py")], "true"),
    ]
    with tempfile.TemporaryDirectory() as tmp:
        for name, writes, moves, expected in cases:
            repo = Path(tmp) / name.replace(" ", "_")
            repo.mkdir()

            def run(*args: str) -> str:
                return subprocess.run(
                    [git, "-c", "user.name=t", "-c", "user.email=t@t", *args],
                    cwd=repo, check=True, capture_output=True, text=True, encoding="utf-8",
                ).stdout.strip()

            run("init", "-q")
            for path, text in {"README.md": "a", "src/a.cpp": "a", "scripts/tool.py": "x = 1\n" * 20,
                               "doc/안내.md": "a"}.items():
                (repo / path).parent.mkdir(parents=True, exist_ok=True)
                (repo / path).write_text(text, encoding="utf-8")
            run("add", "-A")
            run("commit", "-q", "-m", "base")
            base = run("rev-parse", "HEAD")
            for path, text in writes.items():
                (repo / path).parent.mkdir(parents=True, exist_ok=True)
                (repo / path).write_text(text, encoding="utf-8")
            for src, dst in moves:
                (repo / dst).parent.mkdir(parents=True, exist_ok=True)
                run("mv", src, dst)
            run("add", "-A")
            run("commit", "-q", "-m", "change")
            head = run("rev-parse", "HEAD")

            output = repo / "github_output.txt"
            env = dict(os.environ, EVENT_NAME="pull_request", BASE_SHA=base, HEAD_SHA=head,
                       GITHUB_OUTPUT=str(output))
            subprocess.run([bash, "-c", script], cwd=repo, env=env, check=True, capture_output=True)
            if not output.is_file():
                failures.append(f"docs-only detector, {name}: {bash} wrote no GITHUB_OUTPUT file")
                continue
            got = output.read_text(encoding="utf-8").strip()
            if got != f"code={expected}":
                failures.append(f"docs-only detector, {name}: expected code={expected}, got {got!r}")


def main() -> int:
    failures: list[str] = []
    check_clang_tidy_is_wired(failures)
    check_clang_tidy_rejects_test_only_entries(failures)
    check_release_notes_rc_fallback(failures)
    check_release_prerelease_classification(failures)
    check_fuzzers_are_wired(failures)
    check_sanitizers_are_wired(failures)
    check_windows_tests_are_wired(failures)
    check_windows_build_retries_once(failures)
    check_docs_only_skip_is_pr_only(failures)
    check_docs_only_detector_behaviour(failures)
    check_dump_stress_repeats_only_after_a_build(failures)
    check_pull_request_tidy_scope(failures)
    check_compiler_cache_is_ci_only(failures)
    check_workflows_are_pinned_and_bounded(failures)

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1

    print("ci wiring tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

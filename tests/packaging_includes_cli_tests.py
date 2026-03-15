import importlib.util
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
SCRIPTS_DIR = REPO_ROOT / "scripts"


def _load_release_contract():
    contract_path = SCRIPTS_DIR / "release_contract.py"
    spec = importlib.util.spec_from_file_location("release_contract", contract_path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"Failed to load release contract module: {contract_path}")

    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


_RELEASE_CONTRACT = _load_release_contract()
REQUIRED_ZIP_ENTRIES = tuple(_RELEASE_CONTRACT.REQUIRED_ZIP_ENTRIES)
EXCLUDED_WINUI_TOP_LEVEL_DIRS = frozenset(
    _RELEASE_CONTRACT.EXCLUDED_WINUI_TOP_LEVEL_DIRS
)
REQUIRED_WINUI_BUILD_OUTPUTS = tuple(_RELEASE_CONTRACT.REQUIRED_WINUI_BUILD_OUTPUTS)
find_winui_build_root = _RELEASE_CONTRACT.find_winui_build_root


def _touch(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"x")


def main() -> int:
    agents_md = (REPO_ROOT / "AGENTS.md").read_text(encoding="utf-8")
    development_md = (REPO_ROOT / "docs" / "DEVELOPMENT.md").read_text(encoding="utf-8")
    prerelease_template_path = REPO_ROOT / "docs" / "release" / "PRERELEASE_NOTES_TEMPLATE.md"
    package_py = REPO_ROOT / "scripts" / "package.py"
    gate_script = (REPO_ROOT / "scripts" / "verify_release_gate.sh").read_text(encoding="utf-8")
    build_win_script = (REPO_ROOT / "scripts" / "build-win.cmd").read_text(encoding="utf-8")
    build_winui_script = (REPO_ROOT / "scripts" / "build-winui.cmd").read_text(encoding="utf-8")
    linux_workflow = (REPO_ROOT / ".github" / "workflows" / "linux-tests.yml").read_text(encoding="utf-8")
    ci_workflow = (REPO_ROOT / ".github" / "workflows" / "ci.yml").read_text(encoding="utf-8")
    smoke_workflow_path = REPO_ROOT / ".github" / "workflows" / "winui-headless-smoke.yml"
    smoke_workflow = smoke_workflow_path.read_text(encoding="utf-8")
    vibe_py = (REPO_ROOT / "scripts" / "vibe.py").read_text(encoding="utf-8")
    build_win_from_wsl = (REPO_ROOT / "scripts" / "build-win-from-wsl.sh").read_text(
        encoding="utf-8"
    )
    build_winui_from_wsl = (
        REPO_ROOT / "scripts" / "build-winui-from-wsl.sh"
    ).read_text(encoding="utf-8")

    assert "scripts/release_contract.py" in gate_script, (
        "release gate must sync release_contract.py to catch mirror drift"
    )
    assert "scripts/verify_release_gate.sh" in gate_script, (
        "release gate must sync verify_release_gate.sh to catch mirror drift"
    )
    assert "scripts/build-win.cmd" in gate_script, (
        "release gate must sync build-win.cmd alongside packaging scripts"
    )
    assert "find_winui_build_root" in gate_script, (
        "release gate must resolve the real WinUI publish root instead of assuming a flat top-level layout"
    )
    assert "cygpath -u" in gate_script, (
        "release gate must normalize Windows publish paths before bash file existence checks"
    )
    assert "%$'\\r'}" in gate_script, (
        "release gate must trim Windows CRLF output from Python helper snippets before path checks"
    )
    assert "nlohmann-json3-dev" in linux_workflow, (
        "Linux workflow must install nlohmann-json3-dev before configuring CMake tests"
    )
    assert "WinUI headless interop smoke" not in ci_workflow, (
        "main CI must not block merges on the flaky WinUI headless smoke step"
    )
    assert smoke_workflow_path.is_file(), (
        "manual WinUI smoke workflow must exist"
    )
    assert "workflow_dispatch" in smoke_workflow, (
        "manual WinUI smoke workflow must be runnable on demand"
    )
    assert 'Join-Path $env:GITHUB_WORKSPACE "build-winui\\SkyrimDiagDumpToolWinUI.dll"' in smoke_workflow, (
        "manual smoke workflow must target the published WinUI DLL"
    )
    assert "timeout-minutes: 2" in smoke_workflow, (
        "manual smoke workflow must cap the smoke step duration"
    )
    assert "& dotnet $dll --headless --no-online-symbols --out-dir $smokeOutDir $missingDump" in smoke_workflow, (
        "manual smoke workflow must execute the published DLL directly via dotnet"
    )
    assert '$code = $LASTEXITCODE' in smoke_workflow, (
        "manual smoke workflow must assert the direct dotnet invocation exit code"
    )
    assert "SkyrimDiagDumpToolWinUI_headless_bootstrap.log" in smoke_workflow, (
        "manual smoke workflow must print the headless bootstrap log when startup fails"
    )
    assert "_configure_fallback" in vibe_py, (
        "vibe.py must provide a configure fallback when repo-local brain scripts are absent"
    )
    assert "_doctor_fallback" in vibe_py, (
        "vibe.py must provide a doctor fallback when repo-local brain scripts are absent"
    )
    assert "_agents_doctor_fallback" in vibe_py, (
        "vibe.py must provide an agents-doctor fallback when repo-local brain scripts are absent"
    )
    assert 'pushd "%~dp0.."' in build_win_script, (
        "build-win.cmd must self-map the repo root so WSL/UNC launches do not break cmd/lib.exe"
    )
    assert "popd" in build_win_script, (
        "build-win.cmd must restore the working directory after UNC-safe pushd handling"
    )
    assert 'pushd "%~dp0.."' in build_winui_script, (
        "build-winui.cmd must self-map the repo root so WSL/UNC launches stay on a drive path"
    )
    assert "popd" in build_winui_script, (
        "build-winui.cmd must restore the working directory after UNC-safe pushd handling"
    )
    assert "wslpath -w" in build_win_from_wsl, (
        "build-win-from-wsl.sh must convert the batch path to a Windows absolute path"
    )
    assert "powershell.exe" in build_win_from_wsl, (
        "build-win-from-wsl.sh must invoke Windows PowerShell with the absolute batch path"
    )
    assert "wslpath -w" in build_winui_from_wsl, (
        "build-winui-from-wsl.sh must convert the batch path to a Windows absolute path"
    )
    assert "powershell.exe" in build_winui_from_wsl, (
        "build-winui-from-wsl.sh must invoke Windows PowerShell with the absolute batch path"
    )
    assert "bash scripts/build-win-from-wsl.sh" in agents_md, (
        "AGENTS build guide must document the WSL build wrapper entry point"
    )
    assert "bash scripts/build-winui-from-wsl.sh" in agents_md, (
        "AGENTS build guide must document the WSL WinUI build wrapper entry point"
    )
    assert "Release decisions use the local verification commands in this file as the source of truth." in agents_md, (
        "AGENTS build guide must state that local verification is the release source of truth"
    )
    assert ".github/workflows/winui-headless-smoke.yml" in agents_md, (
        "AGENTS build guide must point to the manual WinUI headless smoke workflow"
    )
    assert "bash scripts/build-win-from-wsl.sh" in development_md, (
        "DEVELOPMENT.md must document the WSL build wrapper entry point"
    )
    assert "bash scripts/build-winui-from-wsl.sh" in development_md, (
        "DEVELOPMENT.md must document the WSL WinUI build wrapper entry point"
    )
    assert "Local verification is the release source of truth for this repository." in development_md, (
        "DEVELOPMENT.md must state that release decisions are based on local verification"
    )
    assert "GitHub Actions is optional/reference only" in development_md, (
        "DEVELOPMENT.md must mark GitHub Actions as optional/reference"
    )
    assert ".github/workflows/winui-headless-smoke.yml" in development_md, (
        "DEVELOPMENT.md must document the manual WinUI smoke workflow path"
    )
    assert "cmd.exe /c scripts\\\\build-win.cmd" in development_md, (
        "DEVELOPMENT.md must explain that relative cmd.exe launches from WSL are not supported"
    )
    assert "SkyrimDiagDumpToolWinUI.runtimeconfig.json" in build_winui_script, (
        "build-winui.cmd must require WinUI runtimeconfig sidecar when selecting output"
    )
    assert "SkyrimDiagDumpToolWinUI.deps.json" in build_winui_script, (
        "build-winui.cmd must require WinUI deps sidecar when selecting output"
    )
    assert prerelease_template_path.is_file(), (
        "prerelease release-notes template must exist at docs/release/PRERELEASE_NOTES_TEMPLATE.md"
    )
    prerelease_template = prerelease_template_path.read_text(encoding="utf-8")
    for heading in (
        "## 핵심 변경",
        "## WinUI / 사용성",
        "## 엔진 변경",
        "## 빌드 / 운영",
        "## 주의사항",
        "## 검증",
    ):
        assert heading in prerelease_template, (
            f"prerelease template must include required section heading: {heading}"
        )
    assert "로컬 검증 결과를 우선 적습니다" in prerelease_template, (
        "prerelease template must prefer local verification results over CI status summaries"
    )
    assert "PRERELEASE_NOTES_TEMPLATE.md" in development_md, (
        "DEVELOPMENT.md must point to the prerelease release-notes template"
    )
    assert "--notes-file" in development_md, (
        "DEVELOPMENT.md must document the gh release --notes-file flow"
    )
    assert "Do not block prerelease solely on GitHub Actions." in development_md, (
        "DEVELOPMENT.md release checklist must keep prerelease decisions tied to local verification"
    )
    assert "Windows-only synthetic helper runtime trigger checks:" in development_md, (
        "DEVELOPMENT.md must document the game-off helper runtime trigger checks"
    )
    assert "This is not an analysis-quality regression suite." in development_md, (
        "DEVELOPMENT.md must distinguish runtime trigger checks from analysis-quality regression"
    )
    for runtime_test in (
        "skydiag_helper_runtime_smoke_tests.exe",
        "skydiag_helper_false_positive_runtime_tests.exe",
        "skydiag_helper_hang_runtime_tests.exe",
    ):
        assert runtime_test in development_md, (
            f"DEVELOPMENT.md must document the Windows helper runtime test executable: {runtime_test}"
        )
    assert "run them one at a time" in development_md, (
        "DEVELOPMENT.md must require sequential execution for the helper runtime tests"
    )

    with tempfile.TemporaryDirectory(prefix="skydiag_pkg_test_") as td:
        td_path = Path(td)
        build_dir = td_path / "build"
        winui_dir = td_path / "winui"
        out_zip = td_path / "out.zip"

        # Minimal fake build artifacts expected by scripts/package.py
        _touch(build_dir / "bin" / "SkyrimDiag.dll")
        _touch(build_dir / "bin" / "SkyrimDiagHelper.exe")
        _touch(build_dir / "bin" / "SkyrimDiagDumpToolNative.dll")
        _touch(build_dir / "bin" / "SkyrimDiagDumpToolCli.exe")

        # Minimal fake WinUI publish folder.
        _touch(winui_dir / "SkyrimDiagDumpToolWinUI.exe")
        _touch(winui_dir / "SkyrimDiagDumpToolWinUI.pri")
        _touch(winui_dir / "SkyrimDiagDumpToolWinUI.runtimeconfig.json")
        _touch(winui_dir / "SkyrimDiagDumpToolWinUI.deps.json")
        _touch(winui_dir / "App.xbf")
        _touch(winui_dir / "MainWindow.xbf")
        # Nested build/publish output should be ignored by the packager.
        _touch(winui_dir / "publish" / "SkyrimDiagDumpToolWinUI.exe")
        _touch(winui_dir / "win-x64" / "SkyrimDiagDumpToolWinUI.exe")

        nested_only_dir = td_path / "nested-winui"
        _touch(nested_only_dir / "publish" / "SkyrimDiagDumpToolWinUI.exe")
        _touch(nested_only_dir / "publish" / "SkyrimDiagDumpToolWinUI.pri")
        _touch(nested_only_dir / "publish" / "SkyrimDiagDumpToolWinUI.runtimeconfig.json")
        _touch(nested_only_dir / "publish" / "SkyrimDiagDumpToolWinUI.deps.json")
        _touch(nested_only_dir / "publish" / "App.xbf")
        _touch(nested_only_dir / "publish" / "MainWindow.xbf")
        assert find_winui_build_root(nested_only_dir) == nested_only_dir / "publish", (
            "release contract helper must locate nested WinUI publish roots for CI and release-gate parity"
        )

        proc = subprocess.run(
            [
                sys.executable,
                str(package_py),
                "--build-dir",
                str(build_dir),
                "--winui-dir",
                str(winui_dir),
                "--out",
                str(out_zip),
                "--no-pdb",
            ],
            cwd=str(REPO_ROOT),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if proc.returncode != 0:
            raise AssertionError(
                f"package.py failed ({proc.returncode})\\nstdout:\\n{proc.stdout}\\nstderr:\\n{proc.stderr}"
            )

        assert out_zip.is_file(), "package.py did not produce zip"

        for output in REQUIRED_WINUI_BUILD_OUTPUTS:
            assert (winui_dir / output).is_file(), (
                f"Expected fake WinUI build output for contract check: {output}"
            )

        with zipfile.ZipFile(out_zip, "r") as zf:
            names = set(zf.namelist())

        for entry in REQUIRED_ZIP_ENTRIES:
            assert entry in names, f"Missing required zip entry in zip: {entry}"

        assert "SKSE/Plugins/SkyrimDiag.ini" in names, (
            "Expected SkyrimDiag.ini to be packaged"
        )
        assert "SKSE/Plugins/SkyrimDiagHelper.ini" in names, (
            "Expected SkyrimDiagHelper.ini to be packaged"
        )

        for nested_dir in sorted(EXCLUDED_WINUI_TOP_LEVEL_DIRS):
            forbidden_prefix = f"SKSE/Plugins/SkyrimDiagWinUI/{nested_dir}/"
            assert not any(name.startswith(forbidden_prefix) for name in names), (
                f"Nested WinUI output must not be packaged: {forbidden_prefix}"
            )

        data_root = REPO_ROOT / "dump_tool" / "data"
        expected_data = [
            p.relative_to(data_root).as_posix()
            for p in data_root.rglob("*")
            if p.is_file()
        ]
        assert expected_data, "Expected at least one dump_tool/data file in repository"
        for rel in expected_data:
            plugin_data_path = f"SKSE/Plugins/data/{rel}"
            winui_data_path = f"SKSE/Plugins/SkyrimDiagWinUI/data/{rel}"
            assert plugin_data_path in names, (
                f"Missing plugin data file in zip: {plugin_data_path}"
            )
            assert winui_data_path in names, (
                f"Missing WinUI data file in zip: {winui_data_path}"
            )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

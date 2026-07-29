#!/usr/bin/env python3
from __future__ import annotations

import json
import hashlib
import re
import subprocess
from pathlib import Path, PurePosixPath

REQUIRED_WINUI_RUNTIME_ASSETS = (
    "Microsoft.WindowsAppRuntime.Bootstrap.dll",
    "Microsoft.WindowsAppRuntime.dll",
    "Microsoft.WindowsAppRuntime.pri",
    "Microsoft.ui.xaml.dll",
    "Microsoft.UI.pri",
    "CoreMessagingXP.dll",
)

REQUIRED_WINUI_ASSETS = (
    "SkyrimDiagDumpToolWinUI.dll",
    "SkyrimDiagDumpToolWinUI.pri",
    "SkyrimDiagDumpToolWinUI.runtimeconfig.json",
    "SkyrimDiagDumpToolWinUI.deps.json",
    "App.xbf",
    "MainWindow.xbf",
    *REQUIRED_WINUI_RUNTIME_ASSETS,
)

REQUIRED_WINUI_BUILD_OUTPUTS = (
    "SkyrimDiagDumpToolWinUI.exe",
    *REQUIRED_WINUI_ASSETS,
)

BUILD_PROVENANCE_FILENAME = "SkyrimDiagBuildProvenance.json"
PACKAGE_PROVENANCE_ENTRY = "SKSE/Plugins/SkyrimDiagPackageProvenance.json"
BUILD_PROVENANCE_SCHEMA = "skydiag.build_provenance.v1"
PACKAGE_PROVENANCE_SCHEMA = "skydiag.package_provenance.v1"

REQUIRED_ZIP_ENTRIES = (
    "SKSE/Plugins/SkyrimDiag.dll",
    "SKSE/Plugins/SkyrimDiagHelper.exe",
    "SKSE/Plugins/SkyrimDiagDumpToolCli.exe",
    "SKSE/Plugins/SkyrimDiag.ini",
    "SKSE/Plugins/SkyrimDiagHelper.ini",
    "SKSE/Plugins/SkyrimDiagWinUI/SkyrimDiagDumpToolWinUI.exe",
    "SKSE/Plugins/SkyrimDiagWinUI/app/SkyrimDiagDumpToolWinUI.exe",
    "SKSE/Plugins/SkyrimDiagWinUI/app/SkyrimDiagDumpToolNative.dll",
    PACKAGE_PROVENANCE_ENTRY,
    *(f"SKSE/Plugins/SkyrimDiagWinUI/app/{item}" for item in REQUIRED_WINUI_ASSETS),
)

NATIVE_BUILD_ZIP_MAPPINGS = (
    ("SkyrimDiag.dll", "SKSE/Plugins/SkyrimDiag.dll"),
    ("SkyrimDiagHelper.exe", "SKSE/Plugins/SkyrimDiagHelper.exe"),
    ("SkyrimDiagDumpToolCli.exe", "SKSE/Plugins/SkyrimDiagDumpToolCli.exe"),
    (
        "SkyrimDiagDumpToolWinUI.exe",
        "SKSE/Plugins/SkyrimDiagWinUI/SkyrimDiagDumpToolWinUI.exe",
    ),
    (
        "SkyrimDiagDumpToolNative.dll",
        "SKSE/Plugins/SkyrimDiagWinUI/app/SkyrimDiagDumpToolNative.dll",
    ),
)

WINUI_BUILD_ZIP_MAPPINGS = (
    (
        "SkyrimDiagDumpToolWinUI.exe",
        "SKSE/Plugins/SkyrimDiagWinUI/app/SkyrimDiagDumpToolWinUI.exe",
    ),
    (
        "SkyrimDiagDumpToolWinUI.dll",
        "SKSE/Plugins/SkyrimDiagWinUI/app/SkyrimDiagDumpToolWinUI.dll",
    ),
    *(
        (item, f"SKSE/Plugins/SkyrimDiagWinUI/app/{item}")
        for item in REQUIRED_WINUI_RUNTIME_ASSETS
    ),
)

REQUIRED_X64_PE_ZIP_ENTRIES = tuple(
    entry
    for _, entry in (*NATIVE_BUILD_ZIP_MAPPINGS, *WINUI_BUILD_ZIP_MAPPINGS)
    if Path(entry).suffix.lower() in {".dll", ".exe"}
)

PROJECT_VERSIONED_PE_ZIP_ENTRIES = tuple(
    dict.fromkeys(
        entry
        for _, entry in (*NATIVE_BUILD_ZIP_MAPPINGS, *WINUI_BUILD_ZIP_MAPPINGS[:1])
    )
)

EXCLUDED_WINUI_TOP_LEVEL_DIRS = frozenset({"publish", "win-x64", "x64"})
_SEMVER_COMPONENT = r"(?:0|[1-9]\d*)"
_STABLE_RELEASE_TAG = re.compile(
    rf"^v{_SEMVER_COMPONENT}\.{_SEMVER_COMPONENT}\.{_SEMVER_COMPONENT}$"
)
_SUFFIXED_RELEASE_TAG = re.compile(
    rf"^v{_SEMVER_COMPONENT}\.{_SEMVER_COMPONENT}\.{_SEMVER_COMPONENT}"
    r"-[0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*$"
)


def project_version(root: str | Path) -> str:
    cmake_lists = Path(root) / "CMakeLists.txt"
    text = cmake_lists.read_text(encoding="utf-8")
    project_match = re.search(r"\bproject\s*\((.*?)\)", text, re.IGNORECASE | re.DOTALL)
    version_match = (
        re.search(r"\bVERSION\s+(\d+\.\d+\.\d+)\b", project_match.group(1), re.IGNORECASE)
        if project_match
        else None
    )
    if version_match is None:
        raise ValueError(f"project VERSION not found in {cmake_lists}")
    return version_match.group(1)


def vcpkg_version(root: str | Path) -> str | None:
    """Version declared in vcpkg.json, or None when the manifest omits one.

    vcpkg accepts several version fields; a top-level manifest is also allowed to
    declare none at all, which is a valid way to keep CMakeLists.txt the single
    source of truth.
    """
    manifest = Path(root) / "vcpkg.json"
    if not manifest.is_file():
        return None
    data = json.loads(manifest.read_text(encoding="utf-8"))
    for field in ("version", "version-string", "version-semver", "version-date"):
        value = data.get(field)
        if isinstance(value, str) and value.strip():
            return value.strip()
    return None


def assert_version_sources_agree(root: str | Path) -> str:
    """Ensure every declared project version matches CMakeLists.txt.

    vcpkg.json drifted 14 releases behind CMakeLists.txt before this check
    existed, because the release path only ever reads project_version() and
    nothing compared the two.
    """
    version = project_version(root)
    declared = vcpkg_version(root)
    if declared is not None and declared != version:
        raise ValueError(
            f"version mismatch: CMakeLists.txt has {version} but vcpkg.json has {declared}. "
            "Update vcpkg.json, or drop its version field to keep CMakeLists.txt authoritative."
        )
    return version


def release_zip_name(tag_or_version: str) -> str:
    normalized = tag_or_version.strip()
    if not normalized:
        raise ValueError("tag_or_version must not be empty")
    return f"Tullius_ctd_loger_{normalized}.zip"


def release_tag_is_prerelease(tag: str) -> bool:
    """Classify only the stable and suffixed tag forms accepted by releases."""
    normalized = tag.strip()
    if _STABLE_RELEASE_TAG.fullmatch(normalized):
        return False
    if _SUFFIXED_RELEASE_TAG.fullmatch(normalized):
        return True
    raise ValueError(
        "release tag must be vX.Y.Z or "
        "vX.Y.Z-<suffix>[.<suffix>|-<suffix>...]"
    )


def release_zip_glob() -> str:
    return "Tullius_ctd_loger_v*.zip"


def nested_winui_path_regex() -> str:
    parts = "|".join(sorted(EXCLUDED_WINUI_TOP_LEVEL_DIRS))
    return rf"^SKSE/Plugins/SkyrimDiagWinUI/(app/)?({parts})/"


def sha256_path(path: str | Path) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _run_git(root: Path, *args: str, text: bool = False) -> bytes | str:
    result = subprocess.run(
        ["git", "-C", str(root), *args],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        text=text,
        encoding="utf-8" if text else None,
    )
    if result.returncode != 0:
        stderr = result.stderr if text else result.stderr.decode("utf-8", errors="replace")
        raise ValueError(f"git {' '.join(args)} failed: {stderr.strip()}")
    return result.stdout


def source_state(root: str | Path) -> dict[str, object]:
    root_path = Path(root).resolve()
    commit = str(_run_git(root_path, "rev-parse", "HEAD", text=True)).strip()
    tracked_diff = bytes(_run_git(root_path, "diff", "--binary", "HEAD", "--"))
    untracked_raw = bytes(
        _run_git(root_path, "ls-files", "--others", "--exclude-standard", "-z")
    )
    untracked = sorted(
        item.decode("utf-8", errors="surrogateescape")
        for item in untracked_raw.split(b"\0")
        if item
    )

    digest = hashlib.sha256()
    digest.update(commit.encode("ascii"))
    digest.update(b"\0tracked\0")
    digest.update(tracked_diff)
    digest.update(b"\0untracked\0")
    for rel in untracked:
        path = root_path / rel
        digest.update(rel.encode("utf-8", errors="surrogateescape"))
        digest.update(b"\0")
        if path.is_file():
            digest.update(path.read_bytes())
        digest.update(b"\0")

    return {
        "git_commit": commit,
        "git_dirty": bool(tracked_diff or untracked),
        "source_tree_sha256": digest.hexdigest(),
    }


def _cmake_cache_value(build_path: Path, key: str) -> str | None:
    cache = build_path / "CMakeCache.txt"
    if not cache.is_file():
        raise ValueError(f"CMake cache not found: {cache}")
    prefix = f"{key}:"
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line.startswith(prefix):
            continue
        _, separator, value = line.partition("=")
        if separator:
            return value.strip()
    return None


def native_artifact_dir(
    build_dir: str | Path,
    configuration: str,
    bin_dir: str | Path | None = None,
) -> Path:
    build_path = Path(build_dir).resolve()
    if bin_dir:
        return Path(bin_dir).resolve()

    configurations = _cmake_cache_value(build_path, "CMAKE_CONFIGURATION_TYPES")
    if configurations:
        available = [item for item in configurations.split(";") if item]
        if configuration not in available:
            raise ValueError(
                f"configuration {configuration} not present in CMAKE_CONFIGURATION_TYPES: "
                f"{available}"
            )
        return build_path / "bin" / configuration

    build_type = _cmake_cache_value(build_path, "CMAKE_BUILD_TYPE")
    if not build_type:
        raise ValueError(f"CMAKE_BUILD_TYPE is empty in {build_path / 'CMakeCache.txt'}")
    if build_type.casefold() != configuration.casefold():
        raise ValueError(
            f"build configuration mismatch: requested {configuration}, cache has {build_type}"
        )
    return build_path / "bin"


def native_artifact_path(
    build_dir: str | Path,
    configuration: str,
    filename: str,
    bin_dir: str | Path | None = None,
) -> Path:
    path = native_artifact_dir(build_dir, configuration, bin_dir) / filename
    if not path.is_file():
        raise FileNotFoundError(f"exact build artifact not found: {path}")
    return path


def find_winui_build_root(root: str | Path) -> Path | None:
    root_path = Path(root).resolve()
    if not root_path.is_dir():
        return None
    if all((root_path / item).is_file() for item in REQUIRED_WINUI_BUILD_OUTPUTS):
        return root_path
    return None


def winui_artifact_is_packaged(relative_path: str, *, include_pdb: bool) -> bool:
    """Return whether a flat WinUI build artifact belongs in the MO2 payload."""
    rel = PurePosixPath(relative_path.replace("\\", "/"))
    if rel.is_absolute() or not rel.parts or ".." in rel.parts:
        raise ValueError(f"invalid WinUI artifact path: {relative_path}")
    if rel.name == BUILD_PROVENANCE_FILENAME:
        return False
    if rel.parts[0].casefold() in EXCLUDED_WINUI_TOP_LEVEL_DIRS:
        return False
    if not include_pdb and rel.suffix.casefold() == ".pdb":
        return False
    return True


def validate_build_provenance(
    manifest_path: str | Path,
    repo_root: str | Path,
    *,
    kind: str,
    configuration: str,
    artifacts: dict[str, Path],
    require_clean: bool = False,
) -> dict[str, object]:
    path = Path(manifest_path)
    if not path.is_file():
        raise ValueError(f"build provenance not found: {path}")
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict) or data.get("schema") != BUILD_PROVENANCE_SCHEMA:
        raise ValueError(f"unsupported build provenance schema: {path}")
    if data.get("version") != project_version(repo_root):
        raise ValueError(f"build provenance version mismatch: {path}")
    if data.get("kind") != kind:
        raise ValueError(f"build provenance kind mismatch: {path}")
    if data.get("configuration") != configuration:
        raise ValueError(f"build provenance configuration mismatch: {path}")

    expected_state = source_state(repo_root)
    for field in ("git_commit", "git_dirty", "source_tree_sha256"):
        if data.get(field) != expected_state[field]:
            raise ValueError(
                f"build provenance source state mismatch for {field}: "
                f"manifest={data.get(field)!r} current={expected_state[field]!r}"
            )
    if require_clean and bool(data.get("git_dirty")):
        raise ValueError(f"release build provenance is dirty: {path}")

    recorded = data.get("artifacts")
    if not isinstance(recorded, dict):
        raise ValueError(f"build provenance artifacts must be an object: {path}")
    expected_names = set(artifacts)
    recorded_names = set(recorded)
    if recorded_names != expected_names:
        missing = sorted(expected_names - recorded_names)
        extra = sorted(recorded_names - expected_names)
        raise ValueError(
            "build provenance coverage mismatch: "
            f"missing={missing[:1]} extra={extra[:1]} ({path})"
        )
    for name, artifact_path in artifacts.items():
        expected_hash = recorded.get(name)
        if not isinstance(expected_hash, str):
            raise ValueError(f"build provenance missing artifact {name}: {path}")
        actual_hash = sha256_path(artifact_path)
        if actual_hash != expected_hash:
            raise ValueError(
                f"build provenance artifact mismatch: {name} "
                f"(manifest={expected_hash}, actual={actual_hash})"
            )
    return data

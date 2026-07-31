#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path, PurePosixPath

from release_contract import native_artifact_path

WINDOWS_EPOCH_FILETIME_100NS = 116_444_736_000_000_000


def _resolve(root: Path, value: str) -> Path:
    path = Path(value)
    return (root / path).resolve() if not path.is_absolute() else path.resolve()


def _safe_extract(archive: zipfile.ZipFile, destination: Path) -> None:
    destination_resolved = destination.resolve()
    for info in archive.infolist():
        entry = PurePosixPath(info.filename)
        if entry.is_absolute() or ".." in entry.parts:
            raise ValueError(f"unsafe zip entry: {info.filename}")
        target = (destination / Path(*entry.parts)).resolve()
        if destination_resolved not in target.parents and target != destination_resolved:
            raise ValueError(f"zip entry escapes extraction root: {info.filename}")
    archive.extractall(destination)


def _run(command: list[str], *, cwd: Path, timeout: int) -> subprocess.CompletedProcess[str]:
    creationflags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    return subprocess.run(
        command,
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=timeout,
        creationflags=creationflags,
        check=False,
    )


def _sha256_path(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _read_summary(path: Path, label: str) -> dict[str, object]:
    if not path.is_file() or path.stat().st_size == 0:
        raise RuntimeError(f"packaged launcher did not create {label}: {path}")
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise RuntimeError(f"packaged launcher produced a non-object {label}")
    schema = data.get("schema")
    if not isinstance(schema, dict) or schema.get("name") != "SkyrimDiagSummary":
        raise RuntimeError(f"packaged launcher produced an invalid {label} schema")
    return data


def _validate_dump_identity(
    identity: object, dump_path: Path
) -> tuple[str, int, int]:
    if not isinstance(identity, dict):
        raise RuntimeError("packaged launcher summary is missing dump_identity")
    sha256 = identity.get("sha256")
    size_bytes = identity.get("size_bytes")
    modified = identity.get("last_write_time_utc_100ns")
    if (
        identity.get("schema") != "skydiag.dump_identity.v1"
        or not isinstance(sha256, str)
        or len(sha256) != 64
        or any(char not in "0123456789abcdef" for char in sha256)
        or type(size_bytes) is not int
        or size_bytes <= 0
        or type(modified) is not int
        or modified <= 0
    ):
        raise RuntimeError("packaged launcher produced an invalid dump_identity")

    stat = dump_path.stat()
    expected_modified = stat.st_mtime_ns // 100 + WINDOWS_EPOCH_FILETIME_100NS
    if sha256 != _sha256_path(dump_path):
        raise RuntimeError("packaged launcher dump_identity SHA-256 does not match the dump")
    if size_bytes != stat.st_size:
        raise RuntimeError("packaged launcher dump_identity size does not match the dump")
    if modified != expected_modified:
        raise RuntimeError(
            "packaged launcher dump_identity last-write time does not match the dump"
        )
    return sha256, size_bytes, modified


def _validate_analysis_artifacts(
    output_dir: Path, dump_path: Path
) -> tuple[Path, Path, Path, Path, dict[str, object]]:
    stem = dump_path.stem
    legacy_report = output_dir / f"{stem}_SkyrimDiagReport.txt"
    legacy_summary = output_dir / f"{stem}_SkyrimDiagSummary.json"
    if not legacy_report.is_file() or legacy_report.stat().st_size == 0:
        raise RuntimeError(
            f"packaged launcher did not create legacy report: {legacy_report}"
        )
    legacy_data = _read_summary(legacy_summary, "legacy summary")
    sha256, size_bytes, modified = _validate_dump_identity(
        legacy_data.get("dump_identity"), dump_path
    )

    family_dir = (
        output_dir
        / ".skydiag-analysis"
        / sha256
        / f"{size_bytes:016x}.{modified:016x}"
    )
    family_report = family_dir / "Report.txt"
    family_summary = family_dir / "Summary.json"
    if not family_report.is_file() or family_report.stat().st_size == 0:
        raise RuntimeError(
            f"packaged launcher did not create identity-family report: {family_report}"
        )
    family_data = _read_summary(family_summary, "identity-family summary")
    if family_data.get("dump_identity") != legacy_data.get("dump_identity"):
        raise RuntimeError(
            "identity-family and legacy summaries have different dump_identity values"
        )
    if family_data != legacy_data:
        raise RuntimeError(
            "identity-family and legacy summaries describe different analyses"
        )
    if family_report.read_bytes() != legacy_report.read_bytes():
        raise RuntimeError(
            "identity-family and legacy reports describe different analyses"
        )

    analysis_root = output_dir / ".skydiag-analysis"
    family_summaries = sorted(analysis_root.rglob("Summary.json"))
    family_reports = sorted(analysis_root.rglob("Report.txt"))
    if family_summaries != [family_summary] or family_reports != [family_report]:
        raise RuntimeError(
            "packaged launcher must create exactly one matching dump identity family "
            f"(summaries={len(family_summaries)}, reports={len(family_reports)})"
        )
    return (
        legacy_report,
        legacy_summary,
        family_report,
        family_summary,
        legacy_data,
    )


def smoke_release_zip(
    repo_root: Path,
    zip_path: Path,
    build_dir: Path,
    configuration: str,
    timeout: int,
) -> None:
    if os.name != "nt":
        raise RuntimeError("packaged WinUI smoke requires native Windows")
    if not zip_path.is_file():
        raise FileNotFoundError(f"release zip not found: {zip_path}")

    writer = native_artifact_path(
        build_dir,
        configuration,
        "skydiag_minidump_fixture_writer.exe",
    )

    with tempfile.TemporaryDirectory(prefix="skydiag_release_smoke_") as temp:
        temp_root = Path(temp)
        dump_path = temp_root / "release-smoke.dmp"
        writer_result = _run([str(writer), str(dump_path)], cwd=temp_root, timeout=30)
        if writer_result.returncode != 0:
            raise RuntimeError(
                "minidump fixture writer failed "
                f"(exit={writer_result.returncode}):\n"
                f"{writer_result.stdout}{writer_result.stderr}"
            )
        if not dump_path.is_file() or dump_path.stat().st_size == 0:
            raise RuntimeError("minidump fixture writer did not create a non-empty dump")

        extracted = temp_root / "extracted"
        with zipfile.ZipFile(zip_path, "r") as archive:
            _safe_extract(archive, extracted)

        launcher = (
            extracted
            / "SKSE"
            / "Plugins"
            / "SkyrimDiagWinUI"
            / "SkyrimDiagDumpToolWinUI.exe"
        )
        if not launcher.is_file():
            raise FileNotFoundError(f"packaged top-level WinUI launcher missing: {launcher}")

        output_dir = temp_root / "analysis-output"
        output_dir.mkdir()
        result = _run(
            [
                str(launcher),
                "--headless",
                "--no-online-symbols",
                "--out-dir",
                str(output_dir),
                str(dump_path),
            ],
            cwd=launcher.parent,
            timeout=timeout,
        )
        if result.returncode != 0:
            logs: list[str] = []
            for log_name in (
                "SkyrimDiagDumpToolWinUI_launcher_error.log",
                "app/SkyrimDiagDumpToolWinUI_startup_error.log",
                "app/SkyrimDiagDumpToolWinUI_headless_bootstrap.log",
            ):
                log_path = launcher.parent / log_name
                if log_path.is_file():
                    logs.append(f"\n--- {log_name} ---\n{log_path.read_text(encoding='utf-8', errors='replace')}")
            raise RuntimeError(
                "packaged WinUI launcher smoke failed "
                f"(exit={result.returncode}):\nstdout:\n{result.stdout}\n"
                f"stderr:\n{result.stderr}{''.join(logs)}"
            )

        (
            report,
            summary,
            family_report,
            family_summary,
            summary_data,
        ) = _validate_analysis_artifacts(output_dir, dump_path)
        schema = summary_data["schema"]

        print(f"  - packaged launcher: {launcher}")
        print(f"  - valid minidump: {dump_path.stat().st_size} bytes")
        print(f"  - legacy report: {report.name} ({report.stat().st_size} bytes)")
        print(f"  - legacy summary: {summary.name} (schema v{schema.get('version')})")
        print(f"  - identity-family report: {family_report}")
        print(f"  - identity-family summary: {family_summary}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Extract a release zip and analyze a valid minidump through its top-level WinUI launcher."
    )
    parser.add_argument("--repo-root", default=str(Path(__file__).resolve().parents[1]))
    parser.add_argument("--zip", required=True, dest="zip_path")
    parser.add_argument("--build-dir", default="build-win")
    parser.add_argument("--config", default="RelWithDebInfo")
    parser.add_argument("--timeout", type=int, default=120)
    args = parser.parse_args(argv)

    root = Path(args.repo_root).resolve()
    try:
        smoke_release_zip(
            root,
            _resolve(root, args.zip_path),
            _resolve(root, args.build_dir),
            args.config,
            args.timeout,
        )
    except (
        FileNotFoundError,
        json.JSONDecodeError,
        OSError,
        RuntimeError,
        ValueError,
        zipfile.BadZipFile,
        subprocess.TimeoutExpired,
    ) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

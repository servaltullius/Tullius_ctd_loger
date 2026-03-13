import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]


def _copy(src: Path, dst: Path) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dst)


def _run(root: Path, *args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(root / "scripts" / "vibe.py"), *args],
        cwd=str(root),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="skydiag_vibe_smoke_") as td:
        temp_root = Path(td)
        _copy(REPO_ROOT / "scripts" / "vibe.py", temp_root / "scripts" / "vibe.py")
        _copy(REPO_ROOT / "scripts" / "setup_vibe_env.py", temp_root / "scripts" / "setup_vibe_env.py")

        (temp_root / "AGENTS.md").write_text(
            "# AGENTS\n\n- Read `.vibe/context/LATEST_CONTEXT.md`\n- Run `python3 scripts/vibe.py configure --apply`\n",
            encoding="utf-8",
        )
        copilot_dir = temp_root / ".github"
        copilot_dir.mkdir(parents=True, exist_ok=True)
        (copilot_dir / "copilot-instructions.md").write_text(
            "# Copilot Instructions\n\n- Use `.vibe/context/LATEST_CONTEXT.md` for repo context.\n- (Once) Run: `python3 scripts/vibe.py configure --apply`\n",
            encoding="utf-8",
        )

        configure = _run(temp_root, "configure", "--apply")
        if configure.returncode != 0:
            raise AssertionError(
                f"vibe configure failed ({configure.returncode})\nstdout:\n{configure.stdout}\nstderr:\n{configure.stderr}"
            )

        assert (temp_root / ".vibe" / "config.json").is_file(), "configure fallback must create .vibe/config.json"

        doctor = _run(temp_root, "doctor", "--full")
        if doctor.returncode != 0:
            raise AssertionError(
                f"vibe doctor failed ({doctor.returncode})\nstdout:\n{doctor.stdout}\nstderr:\n{doctor.stderr}"
            )

        agents = _run(temp_root, "agents", "doctor", "--fail")
        if agents.returncode != 0:
            raise AssertionError(
                f"vibe agents doctor failed ({agents.returncode})\nstdout:\n{agents.stdout}\nstderr:\n{agents.stderr}"
            )

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

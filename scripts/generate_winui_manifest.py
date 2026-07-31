#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import tempfile
from pathlib import Path

from release_contract import project_version


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Generate the WinUI application manifest from the project version."
    )
    parser.add_argument("--repo-root", default=str(Path(__file__).resolve().parents[1]))
    parser.add_argument("--template", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args(argv)

    root = Path(args.repo_root).resolve()
    template = Path(args.template).resolve()
    output = Path(args.output).resolve()
    rendered = template.read_text(encoding="utf-8").replace(
        "@SKYDIAG_VERSION@", project_version(root)
    )
    if "@SKYDIAG_VERSION@" in rendered:
        raise ValueError("WinUI manifest contains an unresolved version placeholder")

    output.parent.mkdir(parents=True, exist_ok=True)
    fd, temp_name = tempfile.mkstemp(
        prefix=f".{output.name}.", suffix=".tmp", dir=output.parent
    )
    os.close(fd)
    temp_path = Path(temp_name)
    try:
        temp_path.write_text(rendered, encoding="utf-8")
        os.replace(temp_path, output)
    finally:
        temp_path.unlink(missing_ok=True)
    print(f"Wrote WinUI manifest: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

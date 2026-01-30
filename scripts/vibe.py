#!/usr/bin/env python3
from __future__ import annotations

import argparse
import base64
import hashlib
import io
import subprocess
import sys
import textwrap
import time
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[1]


def _ensure_bootstrap(root: Path) -> None:
    cfg = root / ".vibe" / "config.json"
    if cfg.exists():
        return
    setup = root / "scripts" / "setup_vibe_env.py"
    subprocess.check_call([sys.executable, str(setup)], cwd=str(root))


def _run(script: Path, argv: list[str]) -> int:
    root = _repo_root()
    return subprocess.call([sys.executable, str(script), *argv], cwd=str(root))


def _seed_collect_files(root: Path) -> list[Path]:
    explicit = [
        root / "scripts" / "vibe.py",
        root / "scripts" / "vibe.cmd",
        root / "scripts" / "vibekit.py",
        root / "scripts" / "vibekit.cmd",
        root / "scripts" / "setup_vibe_env.py",
        root / "scripts" / "install_hooks.py",
        root / ".vibe" / "README.md",
        root / ".vibe" / "AGENT_CHECKLIST.md",
        root / ".vibe" / "agent_memory" / "DONT_DO_THIS.md",
        root / ".vibe" / "context" / "PROFILE_GUIDE.md",
        root / ".vibe" / "brain" / "requirements.txt",
    ]

    out: list[Path] = []
    for p in explicit:
        if p.exists() and p.is_file():
            out.append(p)

    # Keep the core brain scripts (no caches/zips).
    for p in sorted((root / ".vibe" / "brain").glob("*.py")):
        if p.name.endswith(".py") and p.is_file():
            out.append(p)

    # De-dup while preserving sort order.
    seen: set[Path] = set()
    uniq: list[Path] = []
    for p in out:
        rp = p.resolve()
        if rp in seen:
            continue
        seen.add(rp)
        uniq.append(p)
    return uniq


def _seed_build_zip(root: Path, files: list[Path]) -> tuple[bytes, list[str]]:
    buf = io.BytesIO()
    names: list[str] = []
    with ZipFile(buf, "w", compression=ZIP_DEFLATED) as z:
        for p in files:
            rel = p.relative_to(root).as_posix()
            names.append(rel)
            z.write(p, arcname=rel)
    return buf.getvalue(), names


def _seed_render_markdown(payload_zip: bytes, included_files: list[str]) -> str:
    sha = hashlib.sha256(payload_zip).hexdigest()
    b64 = base64.b64encode(payload_zip).decode("ascii")
    wrapped = "\n".join(textwrap.wrap(b64, width=76))
    created = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime())

    file_list = "\n".join(f"- `{p}`" for p in included_files)

    return (
        f"# VIBEKIT_SEED (v2)\n\n"
        "This is a single-file seed that can bootstrap **vibe-kit** into another repository.\n\n"
        "vibe-kit is a small **repo-local** toolkit that generates context summaries under `.vibe/context/` and reports under\n"
        "`.vibe/reports/`.\n\n"
        "- It is **not** an AI agent runner/sandbox.\n"
        "- It makes **no network/API calls** by default.\n\n"
        f"- Created: `{created}`\n"
        f"- Payload sha256: `{sha}`\n\n"
        "## Install\n\n"
        "Download `vibekit_seed_install.py` and `SHA256SUMS` from the same GitHub Release as this seed file.\n\n"
        "**Linux/macOS:**\n\n"
        "1) Verify: `sha256sum -c SHA256SUMS`\n"
        "2) Dry-run: `python3 vibekit_seed_install.py install VIBEKIT_SEED.md --root . --expected-seed-sha256 <sha256>`\n"
        "3) Apply: `python3 vibekit_seed_install.py install VIBEKIT_SEED.md --root . --expected-seed-sha256 <sha256> --apply`\n\n"
        "**Windows (PowerShell):**\n\n"
        "1) Verify: `Get-FileHash .\\VIBEKIT_SEED.md -Algorithm SHA256`\n"
        "2) Dry-run: `py vibekit_seed_install.py install VIBEKIT_SEED.md --root . --expected-seed-sha256 <sha256>`\n"
        "3) Apply: `py vibekit_seed_install.py install VIBEKIT_SEED.md --root . --expected-seed-sha256 <sha256> --apply`\n\n"
        "> Tip: Add `--agent codex|claude|copilot|cursor|gemini` to generate one agent instruction file.\n\n"
        "## Included files\n\n"
        f"{file_list}\n\n"
        "## Payload (do not edit)\n\n"
        "<!-- VIBEKIT_PAYLOAD_BASE64_BEGIN -->\n"
        f"{wrapped}\n"
        "<!-- VIBEKIT_PAYLOAD_BASE64_END -->\n"
    )


def main(argv: list[str]) -> int:
    root = _repo_root()

    parser = argparse.ArgumentParser(prog="vibe", description="Repo-local vibe-kit runner (friendly aliases).")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_boot = sub.add_parser("bootstrap", help="Create `.vibe/` scaffolding (no code changes).")
    p_boot.add_argument("--install-deps", action="store_true", help="Install Python deps (optional).")

    p_conf = sub.add_parser(
        "configure",
        help="Auto-detect this repo and update `.vibe/config.json` (dry-run by default).",
    )
    p_conf.add_argument("--apply", action="store_true", help="Write changes to `.vibe/config.json`.")
    p_conf.add_argument("--force", action="store_true", help="Overwrite existing configured values.")

    sub.add_parser("init", help="Initial index + baseline (safe, no code changes).")

    p_hooks = sub.add_parser("hooks", help="Install git hooks (optional).")
    p_hooks.add_argument("--install", action="store_true", help="Install pre-commit hook.")
    p_hooks.add_argument("--force", action="store_true", help="Overwrite existing hooks.")

    p_doc = sub.add_parser("doctor", help="Full scan + report.")
    p_doc.add_argument("--full", action="store_true")
    p_doc.add_argument("--profile", action="store_true", help="Summarize existing perf logs only.")

    p_watch = sub.add_parser("watch", help="Watch files and refresh LATEST_CONTEXT.")
    p_watch.add_argument("--debounce-ms", type=int, default=400)

    p_search = sub.add_parser("search", help="Search the context DB (FTS).")
    p_search.add_argument("query")
    p_search.add_argument("--limit", type=int, default=10)

    p_impact = sub.add_parser("impact", help="Impact analysis for a file.")
    p_impact.add_argument("path")
    p_impact.add_argument("--limit", type=int, default=40)

    p_coupling = sub.add_parser("coupling", help="Analyze change coupling (files that change together in git history).")
    p_coupling.add_argument("--max-commits", type=int, default=200)
    p_coupling.add_argument("--min-pair-count", type=int, default=3)
    p_coupling.add_argument("--min-jaccard", type=float, default=0.2)
    p_coupling.add_argument("--max-files-per-commit", type=int, default=80)
    p_coupling.add_argument(
        "--max-churn-per-commit",
        type=int,
        default=0,
        help="Skip commits with >N added+deleted lines (reduce noisy formatting commits). 0 disables.",
    )
    p_coupling.add_argument("--top", type=int, default=20)
    p_coupling.add_argument("--since", help="Git --since filter (e.g. '6 months ago').")
    p_coupling.add_argument("--detect-renames", action="store_true", help="Best-effort: treat renames/moves as the same node.")
    p_coupling.add_argument("--out", default=".vibe/reports/change_coupling.json")
    p_coupling.add_argument("--group-by", choices=["file", "dir"], default="file")
    p_coupling.add_argument("--dir-depth", type=int, default=2)
    p_coupling.add_argument("--min-cluster-size", type=int, default=2)
    p_coupling.add_argument("--max-clusters", type=int, default=10)
    p_coupling.add_argument("--max-boundary-leaks", type=int, default=20)
    p_coupling.add_argument("--max-hubs", type=int, default=20)

    p_bound = sub.add_parser("boundaries", help="Detect boundary violations (illegal dependencies) from config rules.")
    p_bound.add_argument("--out", default=".vibe/reports/boundaries.json")
    p_bound.add_argument("--md-out", default=".vibe/reports/boundaries.md")
    p_bound.add_argument("--max-violations", type=int, default=200)
    p_bound.add_argument("--best-effort", action="store_true", help="Never fail the process (exit 0).")

    p_qa = sub.add_parser("qa", help="Placeholder QA for xTranslator XML.")
    p_qa.add_argument("xml_path")
    p_qa.add_argument("--limit", type=int, default=80)

    p_pack = sub.add_parser("pack", help="Generate a compact context pack for LLMs.")
    p_pack.add_argument("--scope", choices=["staged", "changed", "path", "recent"], default="staged")
    p_pack.add_argument("--path", help="Path for --scope=path (file or directory).")
    p_pack.add_argument("--max-kb", type=int, default=24)
    p_pack.add_argument("--out", default=".vibe/context/PACK.md")
    p_pack.add_argument("--symbols-per-file", type=int, default=5)
    p_pack.add_argument("--refresh-index", action="store_true")

    p_seed = sub.add_parser("seed", help="Export a single-file `VIBEKIT_SEED.md` (portable bootstrap).")
    p_seed.add_argument("--out", default="VIBEKIT_SEED.md")
    p_seed.add_argument("--force", action="store_true", help="Overwrite output file if it exists.")

    p_agents = sub.add_parser("agents", help="AGENTS.md helpers.")
    agents_sub = p_agents.add_subparsers(dest="agents_cmd", required=True)
    p_agents_lint = agents_sub.add_parser("lint", help="Warn if AGENTS.md files are near/over the Codex size limit.")
    p_agents_lint.add_argument("--max-kb", type=int, default=32)
    p_agents_lint.add_argument("--fail", action="store_true")

    p_precommit = sub.add_parser("precommit", help="Run staged-only precommit chain (if git exists).")
    p_precommit.add_argument("--run-tests", action="store_true", help="Also run core tests (slower).")

    args, rest = parser.parse_known_args(argv)
    _ensure_bootstrap(root)

    brain = root / ".vibe" / "brain"

    if args.cmd == "bootstrap":
        setup = root / "scripts" / "setup_vibe_env.py"
        cmd = [sys.executable, str(setup)]
        if args.install_deps:
            cmd.append("--install-deps")
        return subprocess.call(cmd, cwd=str(root))

    if args.cmd == "configure":
        conf_args: list[str] = []
        if args.apply:
            conf_args.append("--apply")
        if args.force:
            conf_args.append("--force")
        return _run(brain / "configure.py", conf_args)

    if args.cmd == "init":
        rc = _run(brain / "indexer.py", ["--scan-all"])
        if rc:
            return rc
        _run(brain / "typecheck_baseline.py", ["--init"])
        _run(brain / "dependency_hotspots.py", [])
        _run(brain / "check_complexity.py", [])
        _run(brain / "check_circular.py", [])
        _run(brain / "summarizer.py", ["--full"])
        return 0

    if args.cmd == "hooks":
        if not args.install:
            print("[vibe] use: `python3 scripts/vibe.py hooks --install`")
            return 0
        install = root / "scripts" / "install_hooks.py"
        hook_args: list[str] = []
        if args.force:
            hook_args.append("--force")
        return _run(install, hook_args)

    if args.cmd == "doctor":
        doc_args: list[str] = []
        if args.full:
            doc_args.append("--full")
        if args.profile:
            doc_args.append("--profile")
        return _run(brain / "doctor.py", doc_args)

    if args.cmd == "watch":
        return _run(brain / "watcher.py", [f"--debounce-ms={args.debounce_ms}"])

    if args.cmd == "search":
        return _run(brain / "search.py", [args.query, f"--limit={args.limit}"])

    if args.cmd == "impact":
        return _run(brain / "impact_analyzer.py", [args.path, f"--limit={args.limit}"])

    if args.cmd == "coupling":
        coup_args = [
            f"--max-commits={args.max_commits}",
            f"--min-pair-count={args.min_pair_count}",
            f"--min-jaccard={args.min_jaccard}",
            f"--max-files-per-commit={args.max_files_per_commit}",
            f"--max-churn-per-commit={args.max_churn_per_commit}",
            f"--top={args.top}",
            f"--out={args.out}",
            f"--group-by={args.group_by}",
            f"--dir-depth={args.dir_depth}",
            f"--min-cluster-size={args.min_cluster_size}",
            f"--max-clusters={args.max_clusters}",
            f"--max-boundary-leaks={args.max_boundary_leaks}",
            f"--max-hubs={args.max_hubs}",
        ]
        if args.since:
            coup_args.append(f"--since={args.since}")
        if args.detect_renames:
            coup_args.append("--detect-renames")
        return _run(brain / "change_coupling.py", coup_args)

    if args.cmd == "boundaries":
        bound_args = [
            f"--out={args.out}",
            f"--md-out={args.md_out}",
            f"--max-violations={args.max_violations}",
        ]
        if args.best_effort:
            bound_args.append("--best-effort")
        return _run(brain / "check_boundaries.py", bound_args)

    if args.cmd == "qa":
        return _run(brain / "qa_placeholders.py", [args.xml_path, f"--limit={args.limit}"])

    if args.cmd == "precommit":
        pre_args: list[str] = []
        if args.run_tests:
            pre_args.append("--run-tests")
        return _run(brain / "precommit.py", pre_args)

    if args.cmd == "pack":
        pack_args = [
            f"--scope={args.scope}",
            f"--max-kb={args.max_kb}",
            f"--out={args.out}",
            f"--symbols-per-file={args.symbols_per_file}",
        ]
        if args.path:
            pack_args.append(f"--path={args.path}")
        if args.refresh_index:
            pack_args.append("--refresh-index")
        return _run(brain / "pack.py", pack_args)

    if args.cmd == "seed":
        out_path = root / args.out
        if out_path.exists() and not args.force:
            print(f"[seed] exists: {out_path} (use --force to overwrite)", file=sys.stderr)
            return 2
        files = _seed_collect_files(root)
        payload_zip, names = _seed_build_zip(root, files)
        md = _seed_render_markdown(payload_zip, names)
        out_path.write_text(md, encoding="utf-8")
        print(f"[seed] wrote: {out_path} (files={len(names)}, zip_bytes={len(payload_zip)})")
        return 0

    if args.cmd == "agents":
        if args.agents_cmd == "lint":
            lint_args = [f"--max-kb={args.max_kb}"]
            if args.fail:
                lint_args.append("--fail")
            return _run(brain / "agents_lint.py", lint_args)
        raise RuntimeError(f"unknown agents cmd: {args.agents_cmd}")

    raise RuntimeError(f"unknown cmd: {args.cmd}")


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

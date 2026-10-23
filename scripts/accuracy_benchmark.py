#!/usr/bin/env python3
"""Accuracy benchmark: how often each view names the right cause of a crash.

A corpus is a directory with manifest.json and cases/<id>/. Every case is a
real or test incident whose cause a person confirmed, scored three ways:

- tullius_only: our analyzer without a Crash Logger log (--no-crash-logger)
- tullius_with_crash_logger: our analyzer with the paired Crash Logger log
- crash_logger: what a reader of the Crash Logger log alone would pick, the
  first probable call-stack frame outside Windows, the C/C++ runtime and the
  game EXE

A view "leads" with the first module or mod it tells the user to check. For
our analyzer that is the first actionable candidate, else the first suspect.
Ground truth is either the module(s)/mod(s) at fault, or "none" when no
plugin caused it (a game or setup problem, e.g. the 2026-10-13 ContentCatalog
CTD); then the right answer is to lead with no plugin. A case can also name
the known-cause signature our analyzer should report.

The corpus holds analysis summaries, not dumps: keep it outside the repo (the
summaries carry mod lists and paths) and keep the dumps where they are.

  python scripts/accuracy_benchmark.py add-case --corpus <dir> --id <id> \
      --kind ctd|freeze --dump <dmp> [--crash-logger-log <log>] \
      (--expect-module <dll or mod> ... | --expect-none) \
      [--expect-known-cause <signature id>] [--note <text>] \
      --cli build-win/bin/SkyrimDiagDumpToolCli.exe
  python scripts/accuracy_benchmark.py score --corpus <dir> [--out-json <file>]
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import tempfile
from pathlib import Path, PureWindowsPath
from typing import Any

VIEWS = ("tullius_only", "tullius_with_crash_logger", "crash_logger")
SCHEMA_VERSION = 1

# Mirrors IsSystemishModuleAsciiLower / IsGameExeModuleAsciiLower in
# dump_tool/src/CrashLoggerParseCore.cpp: frames a reader skips.
SYSTEM_MODULES = {
    "kernelbase.dll", "ntdll.dll", "kernel32.dll", "ucrtbase.dll",
    "msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll", "concrt140.dll",
    "user32.dll", "gdi32.dll", "combase.dll", "ole32.dll",
    "ws2_32.dll", "win32u.dll", "advapi32.dll", "rpcrt4.dll", "sechost.dll",
    "d3d11.dll", "d3d12.dll", "dxgi.dll", "d3d9.dll",
    "opengl32.dll", "d3dcompiler_47.dll", "dxcore.dll",
    "dbghelp.dll", "dbgcore.dll",
}
GAME_MODULES = {"skyrimse.exe", "skyrimae.exe", "skyrimvr.exe", "skyrim.exe"}
FRAME_RE = re.compile(r"^\s*\[\s*\d+\]\s*(\[[PS]\])?\s*0x[0-9A-Fa-f]+\s+([^\s+]+)\+[0-9A-Fa-f]+")


def norm(value: str) -> str:
    return " ".join(str(value).strip().lower().split())


# ---------------------------------------------------------------- extraction

def crash_logger_probable_modules(log_text: str) -> list[str]:
    """Modules of the probable call-stack frames, innermost first."""
    modules: list[str] = []
    in_stack = False
    mixed = False
    for line in log_text.splitlines():
        stripped = line.strip()
        upper = stripped.upper()
        if not in_stack:
            if upper.startswith("PROBABLE CALL STACK") or upper.startswith("CALL STACK"):
                in_stack = True
                mixed = "[P]ROBABLE" in upper
            continue
        if not stripped:
            if modules:
                break
            continue
        match = FRAME_RE.match(line)
        if not match:
            if stripped.endswith(":") and not line.startswith(("\t", " ")):
                break
            continue
        tag, module = match.group(1), match.group(2)
        if mixed and tag != "[P]":
            continue
        modules.append(module)
    return modules


def crash_logger_view(log_text: str | None) -> dict[str, Any]:
    """What a reader of the Crash Logger log alone would check, in order."""
    ranked: list[str] = []
    if log_text:
        for module in crash_logger_probable_modules(log_text):
            low = module.lower()
            if low in SYSTEM_MODULES or low in GAME_MODULES or low in (m.lower() for m in ranked):
                continue
            ranked.append(module)
    return {"ranked": ranked, "lead": ranked[0] if ranked else ""}


def candidate_labels(candidate: dict[str, Any]) -> list[str]:
    keys = ("module_filename", "display_name", "primary_identifier", "mod_name", "inferred_mod_name",
            "secondary_label", "plugin_name")
    return [norm(candidate[k]) for k in keys if isinstance(candidate.get(k), str) and candidate[k].strip()]


def tullius_view(summary: dict[str, Any]) -> dict[str, Any]:
    """Actionable candidates first (what the report leads with), then suspects."""
    ranked: list[list[str]] = []
    for key in ("actionable_candidates", "suspects"):
        for candidate in summary.get(key) or []:
            if not isinstance(candidate, dict):
                continue
            labels = candidate_labels(candidate)
            if labels and not any(set(labels) & set(seen) for seen in ranked):
                ranked.append(labels)
    signature = summary.get("signature_match") or {}
    return {
        "ranked": ranked,
        "lead": ranked[0] if ranked else [],
        "known_cause": signature.get("id", "") if isinstance(signature, dict) else "",
    }


# ------------------------------------------------------------------- scoring

def _hit(labels: list[str] | str, expected: set[str]) -> bool:
    if isinstance(labels, str):
        labels = [norm(labels)] if labels else []
    return any(label in expected for label in labels)


def score_view(view: dict[str, Any], truth: dict[str, Any]) -> dict[str, Any]:
    expected = {norm(m) for m in truth.get("modules", []) if str(m).strip()}
    expect_none = bool(truth.get("none"))
    lead = view.get("lead")
    has_lead = bool(lead)
    if expect_none:
        outcome = "wrong" if has_lead else "correct"
        top3 = None
    else:
        outcome = "abstain" if not has_lead else ("correct" if _hit(lead, expected) else "wrong")
        top3 = any(_hit(labels, expected) for labels in view.get("ranked", [])[:3])
    result: dict[str, Any] = {"outcome": outcome, "top3": top3,
                              "lead": lead if isinstance(lead, str) else (lead[0] if lead else "")}
    want_cause = str(truth.get("known_cause", "")).strip()
    if want_cause and "known_cause" in view:
        result["known_cause_hit"] = view["known_cause"] == want_cause
    return result


def score_corpus(corpus: Path) -> dict[str, Any]:
    manifest = json.loads((corpus / "manifest.json").read_text(encoding="utf-8"))
    cases_out: list[dict[str, Any]] = []
    totals = {view: {"cases": 0, "correct": 0, "wrong": 0, "abstain": 0, "top3_cases": 0, "top3_hits": 0,
                     "known_cause_cases": 0, "known_cause_hits": 0} for view in VIEWS}
    for case in manifest.get("cases", []):
        base = corpus / "cases" / case["id"]
        truth = case["ground_truth"]
        views: dict[str, dict[str, Any]] = {}
        for name in ("tullius_only", "tullius_with_crash_logger"):
            summary = json.loads((base / f"{name}_summary.json").read_text(encoding="utf-8"))
            views[name] = tullius_view(summary)
        views["crash_logger"] = json.loads((base / "crash_logger_view.json").read_text(encoding="utf-8"))
        scored = {name: score_view(view, truth) for name, view in views.items()}
        for name, result in scored.items():
            t = totals[name]
            t["cases"] += 1
            t[result["outcome"]] += 1
            if result["top3"] is not None:
                t["top3_cases"] += 1
                t["top3_hits"] += 1 if result["top3"] else 0
            if "known_cause_hit" in result:
                t["known_cause_cases"] += 1
                t["known_cause_hits"] += 1 if result["known_cause_hit"] else 0
        cases_out.append({"id": case["id"], "kind": case.get("kind", ""), "truth": truth, "views": scored})

    def rate(n: int, d: int) -> float | None:
        return round(n / d, 3) if d else None

    overall = {
        name: {
            "cases": t["cases"],
            "top1_accuracy": rate(t["correct"], t["cases"]),
            "wrong_lead_rate": rate(t["wrong"], t["cases"]),
            "abstain_rate": rate(t["abstain"], t["cases"]),
            "top3_recall": rate(t["top3_hits"], t["top3_cases"]),
            "known_cause_rate": rate(t["known_cause_hits"], t["known_cause_cases"]),
        }
        for name, t in totals.items()
    }
    return {"schema_version": SCHEMA_VERSION, "overall": overall, "cases": cases_out}


def print_table(report: dict[str, Any]) -> None:
    def fmt(v: float | None) -> str:
        return "-" if v is None else f"{v:.0%}"
    print(f"{'view':28} {'cases':>5} {'top1':>6} {'wrong':>6} {'abstain':>8} {'top3':>6} {'cause':>6}")
    for name in VIEWS:
        o = report["overall"][name]
        print(f"{name:28} {o['cases']:>5} {fmt(o['top1_accuracy']):>6} {fmt(o['wrong_lead_rate']):>6} "
              f"{fmt(o['abstain_rate']):>8} {fmt(o['top3_recall']):>6} {fmt(o['known_cause_rate']):>6}")
    print()
    for case in report["cases"]:
        marks = "  ".join(f"{name}={case['views'][name]['outcome']}({case['views'][name]['lead'] or '-'})"
                          for name in VIEWS)
        print(f"{case['id']}: {marks}")


# ------------------------------------------------------------------ add-case

def run_analyzer(
    cli: list[str], dump: Path, out_dir: Path, with_crash_logger: bool, lang: str = "en"
) -> dict[str, Any]:
    out_dir.mkdir(parents=True, exist_ok=True)
    # CreateProcess does not search the working directory for "dir/x.exe".
    program = Path(cli[0])
    cli = [str(program.resolve()) if program.is_file() else cli[0], *cli[1:]]
    args = [*cli, str(dump), "--out-dir", str(out_dir), "--lang", lang, "--no-online-symbols"]
    if not with_crash_logger:
        args.append("--no-crash-logger")
    proc = subprocess.run(args, capture_output=True, text=True, encoding="utf-8", errors="replace")
    summary_path = out_dir / f"{dump.stem}_SkyrimDiagSummary.json"
    if proc.returncode != 0 or not summary_path.is_file():
        raise SystemExit(f"analyzer failed ({proc.returncode}) for {dump}:\n{proc.stdout}\n{proc.stderr}")
    return json.loads(summary_path.read_text(encoding="utf-8"))


def check_paired_log(summary: dict[str, Any], expected: Path | None) -> None:
    """The analyzer finds the Crash Logger log on its own; it must be the log
    the crash_logger view reads, or the views describe different incidents."""
    crash_logger = summary.get("crash_logger")
    log_path = crash_logger.get("log_path", "") if isinstance(crash_logger, dict) else ""
    paired = PureWindowsPath(log_path).name if isinstance(log_path, str) and log_path else ""
    wanted = expected.name if expected else ""
    if paired.lower() != wanted.lower():
        raise SystemExit(f"the analyzer paired Crash Logger log {paired or '(none)'!r}, "
                         f"but --crash-logger-log is {wanted or '(none)'!r}")


def add_case(args: argparse.Namespace) -> int:
    corpus: Path = args.corpus
    if not args.expect_none and not args.expect_module:
        raise SystemExit("give --expect-module (one or more) or --expect-none")
    if args.expect_none and args.expect_module:
        raise SystemExit("--expect-none and --expect-module are exclusive")
    case_dir = corpus / "cases" / args.id
    if case_dir.exists() and not args.replace:
        raise SystemExit(f"case {args.id} exists; pass --replace to rebuild it")
    summaries: dict[str, dict[str, Any]] = {}
    with tempfile.TemporaryDirectory(prefix="skydiag_bench_") as tmp:
        for name, with_cl in (("tullius_only", False), ("tullius_with_crash_logger", True)):
            summaries[name] = run_analyzer(args.cli, args.dump, Path(tmp) / name, with_cl)
    check_paired_log(summaries["tullius_with_crash_logger"], args.crash_logger_log)
    case_dir.mkdir(parents=True, exist_ok=True)
    for name, summary in summaries.items():
        (case_dir / f"{name}_summary.json").write_text(
            json.dumps(summary, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    log_text = args.crash_logger_log.read_text(encoding="utf-8", errors="replace") if args.crash_logger_log else None
    (case_dir / "crash_logger_view.json").write_text(
        json.dumps(crash_logger_view(log_text), indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    manifest_path = corpus / "manifest.json"
    manifest = (json.loads(manifest_path.read_text(encoding="utf-8")) if manifest_path.is_file()
                else {"schema_version": SCHEMA_VERSION, "cases": []})
    truth: dict[str, Any] = {"none": True} if args.expect_none else {"modules": args.expect_module}
    if args.expect_known_cause:
        truth["known_cause"] = args.expect_known_cause
    entry = {
        "id": args.id,
        "kind": args.kind,
        "dump": args.dump.name,
        "crash_logger_log": args.crash_logger_log.name if args.crash_logger_log else "",
        "ground_truth": truth,
        "note": args.note or "",
    }
    manifest["cases"] = [c for c in manifest.get("cases", []) if c.get("id") != args.id] + [entry]
    manifest_path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"added {args.id} to {manifest_path}")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    add = sub.add_parser("add-case", help="analyze one confirmed incident into the corpus")
    add.add_argument("--corpus", type=Path, required=True)
    add.add_argument("--id", required=True)
    add.add_argument("--kind", choices=("ctd", "freeze"), required=True)
    add.add_argument("--dump", type=Path, required=True)
    add.add_argument("--crash-logger-log", type=Path)
    add.add_argument("--expect-module", action="append", default=[])
    add.add_argument("--expect-none", action="store_true")
    add.add_argument("--expect-known-cause")
    add.add_argument("--note")
    add.add_argument("--replace", action="store_true")
    add.add_argument("--cli", nargs="+", required=True, help="analyzer command (SkyrimDiagDumpToolCli.exe)")

    score = sub.add_parser("score", help="score every case in the corpus")
    score.add_argument("--corpus", type=Path, required=True)
    score.add_argument("--out-json", type=Path)

    args = parser.parse_args(argv)
    if args.command == "add-case":
        return add_case(args)
    report = score_corpus(args.corpus)
    if args.out_json:
        args.out_json.parent.mkdir(parents=True, exist_ok=True)
        args.out_json.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print_table(report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

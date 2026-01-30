#!/usr/bin/env python3
from __future__ import annotations

import argparse
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path

from context_db import load_config


IGNORE_SUFFIXES = (".swp", ".tmp", ".bak", ".pyc", "~")


def _run(py: Path, args: list[str]) -> int:
    cmd = [sys.executable, str(py), *args]
    p = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return p.returncode


def _should_track(path: Path, cfg) -> bool:
    if any(str(path).endswith(s) for s in IGNORE_SUFFIXES):
        return False
    rel = None
    try:
        rel = path.relative_to(cfg.root)
    except ValueError:
        return False
    parts = {p.lower() for p in rel.parts}
    if any(ex.lower() in parts for ex in cfg.exclude_dirs):
        return False
    suf = path.suffix.lower()
    return suf in {".cs", ".xaml", ".csproj", ".sln", ".md"}


@dataclass
class Pending:
    last_ts: float


def _loop(cfg, pending: dict[Path, Pending], lock: threading.Lock, debounce_s: float) -> None:
    brain = cfg.root / ".vibe" / "brain"
    while True:
        time.sleep(0.2)
        now = time.time()
        ready: list[Path] = []
        with lock:
            for p, meta in list(pending.items()):
                if now - meta.last_ts >= debounce_s:
                    ready.append(p)
                    pending.pop(p, None)

        if not ready:
            continue

        for p in ready:
            rel = p.relative_to(cfg.root).as_posix()
            rc = _run(brain / "indexer.py", ["--file", rel])
            if rc != 0:
                # one retry (sqlite lock etc)
                time.sleep(0.3)
                _run(brain / "indexer.py", ["--file", rel])

        _run(brain / "summarizer.py", [])


def _watch_with_watchdog(cfg, debounce_s: float) -> int:
    try:
        from watchdog.events import FileSystemEventHandler
        from watchdog.observers import Observer
    except Exception as ex:
        print(
            "[watcher] missing dependency: watchdog. Install with:\n"
            "  python3 scripts/vibe.py bootstrap --install-deps\n"
            "or:\n"
            "  python3 -m pip install -r .vibe/brain/requirements.txt\n"
            f"(details: {ex})",
            file=sys.stderr,
        )
        return 1

    pending: dict[Path, Pending] = {}
    lock = threading.Lock()

    class Handler(FileSystemEventHandler):
        def on_modified(self, event):  # noqa: N802
            if event.is_directory:
                return
            p = Path(event.src_path)
            if not _should_track(p, cfg):
                return
            with lock:
                pending[p] = Pending(last_ts=time.time())

        def on_created(self, event):  # noqa: N802
            self.on_modified(event)

        def on_moved(self, event):  # noqa: N802
            if event.is_directory:
                return
            p = Path(getattr(event, "dest_path", event.src_path))
            if not _should_track(p, cfg):
                return
            with lock:
                pending[p] = Pending(last_ts=time.time())

    observer = Observer()
    handler = Handler()
    observer.schedule(handler, str(cfg.root), recursive=True)

    thread = threading.Thread(target=_loop, args=(cfg, pending, lock, debounce_s), daemon=True)
    thread.start()

    observer.start()
    print(f"[watcher] watching: {cfg.root} (debounce={debounce_s:.2f}s)")
    try:
        while True:
            time.sleep(1.0)
    except KeyboardInterrupt:
        print("[watcher] stopping...")
        observer.stop()
    observer.join()
    return 0


def _watch_with_polling(cfg, debounce_s: float) -> int:
    # Fallback when watchdog isn't installed: poll mtimes.
    tracked: dict[Path, float] = {}
    for p in cfg.root.rglob("*"):
        if p.is_file() and _should_track(p, cfg):
            tracked[p] = p.stat().st_mtime

    pending: dict[Path, Pending] = {}
    lock = threading.Lock()
    thread = threading.Thread(target=_loop, args=(cfg, pending, lock, debounce_s), daemon=True)
    thread.start()

    print(f"[watcher] watchdog not available; polling every 1s (debounce={debounce_s:.2f}s)")
    try:
        while True:
            time.sleep(1.0)
            for p, last in list(tracked.items()):
                try:
                    cur = p.stat().st_mtime
                except OSError:
                    continue
                if cur != last:
                    tracked[p] = cur
                    with lock:
                        pending[p] = Pending(last_ts=time.time())
    except KeyboardInterrupt:
        print("[watcher] stopping...")
        return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Watch repo changes and refresh LATEST_CONTEXT.md.")
    parser.add_argument("--debounce-ms", type=int, default=400)
    args = parser.parse_args(argv)

    cfg = load_config()
    debounce_s = max(0.3, min(1.0, args.debounce_ms / 1000.0))
    rc = _watch_with_watchdog(cfg, debounce_s)
    if rc == 0:
        return 0
    return _watch_with_polling(cfg, debounce_s)


if __name__ == "__main__":
    raise SystemExit(main(__import__("sys").argv[1:]))

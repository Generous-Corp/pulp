#!/usr/bin/env python3
"""Read macOS crash reports for Rack and say what killed it.

A headless Rack that dies leaves two traces: a non-zero exit status, which
says only that something went wrong, and a crash report in
~/Library/Logs/DiagnosticReports, which says what. Only the second one can
tell an abort inside our patch apart from an abort that happened before Rack
had read a single line of it.

That distinction decides whether a launch may be retried. A crash in
CoreMIDI initialisation happens ~170 ms in, before the patch is parsed, so it
carries no information about the patch and retrying it cannot hide a real
defect. Any other crash might be ours, so it stays fatal.

Usage:
    since = crash_watch.now()
    ...launch Rack...
    for c in crash_watch.since(since):
        print(c.kind, c.summary)
"""

from __future__ import annotations

import json
import re
import time
from pathlib import Path
from typing import NamedTuple

REPORT_DIR = Path.home() / "Library/Logs/DiagnosticReports"

# Frames that prove the crash happened during MIDI driver init, which Rack
# runs from main() before it opens the patch. Matching any one is enough.
_COREMIDI_FRAMES = (
    "rtmidiInit",
    "RtMidiDriver",
    "MidiInCore",
    "getCoreMidiClientSingleton",
)


class Crash(NamedTuple):
    path: Path
    when: float
    kind: str  # "coremidi-init" | "unknown"
    signal: str
    parent: str
    frames: list[str]

    @property
    def retryable(self) -> bool:
        """Only a crash we can prove predates patch loading may be retried."""
        return self.kind == "coremidi-init"

    @property
    def summary(self) -> str:
        if self.kind == "coremidi-init":
            return (
                "Rack aborted in CoreMIDI driver init, before it read the "
                "patch. Intermittent and unrelated to the patch under test."
            )
        top = " <- ".join(self.frames[:6]) if self.frames else "no symbols"
        return f"Rack died ({self.signal}) with an unrecognised stack: {top}"


def now() -> float:
    return time.time()


def _body(text: str) -> dict | None:
    """An .ips is a one-line JSON header followed by a JSON body."""
    start = text.find("\n{")
    if start < 0:
        return None
    try:
        return json.loads(text[start:])
    except ValueError:
        return None


def _frames(doc: dict) -> list[str]:
    threads = doc.get("threads") or []
    index = doc.get("faultingThread")
    if index is None or index >= len(threads):
        # Some reports omit faultingThread; fall back to the flagged one.
        index = next(
            (i for i, t in enumerate(threads) if t.get("triggered")), None
        )
    if index is None or index >= len(threads):
        return []
    out = []
    for frame in threads[index].get("frames", []):
        symbol = frame.get("symbol")
        if symbol:
            # Template noise buries the useful name; keep the callable part.
            out.append(re.sub(r"\(.*", "", symbol))
    return out


def read(path: Path) -> Crash | None:
    try:
        doc = _body(path.read_text(errors="replace"))
    except OSError:
        return None
    if not doc:
        return None
    frames = _frames(doc)
    joined = " ".join(frames)
    kind = (
        "coremidi-init"
        if any(f in joined for f in _COREMIDI_FRAMES)
        else "unknown"
    )
    return Crash(
        path=path,
        when=path.stat().st_mtime,
        kind=kind,
        signal=(doc.get("exception") or {}).get("signal", "?"),
        parent=doc.get("parentProc", "?"),
        frames=frames,
    )


def since(timestamp: float, process: str = "Rack") -> list[Crash]:
    """Crash reports for `process` written after `timestamp`, oldest first.

    Matching is on the report filename, which macOS builds from the process
    name, so a report for an unrelated binary never reaches the caller.
    """
    if not REPORT_DIR.is_dir():
        return []
    found = []
    for path in REPORT_DIR.glob(f"{process}-*.ips"):
        try:
            if path.stat().st_mtime <= timestamp:
                continue
        except OSError:
            continue
        crash = read(path)
        if crash:
            found.append(crash)
    return sorted(found, key=lambda c: c.when)


def main(argv: list[str]) -> int:
    seconds = float(argv[1]) if len(argv) > 1 else 3600.0
    crashes = since(now() - seconds)
    if not crashes:
        print(f"no Rack crash reports in the last {seconds:.0f}s")
        return 0
    for c in crashes:
        mark = "retryable" if c.retryable else "FATAL"
        stamp = time.strftime("%H:%M:%S", time.localtime(c.when))
        print(f"{stamp}  [{mark}] {c.kind}  parent={c.parent}  {c.signal}")
        print(f"           {c.summary}")
        print(f"           {c.path}")
    return 0


if __name__ == "__main__":
    import sys

    raise SystemExit(main(sys.argv))

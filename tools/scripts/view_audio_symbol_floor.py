#!/usr/bin/env python3
"""view_audio_symbol_floor.py — no view library may need pulp-view-audio's code.

The include-boundary and link-closure gates read headers and target links. They
cannot see a symbol reference: a view-core source that dynamic_casts to Meter
names only a view-core header, yet its object needs Meter's typeinfo, which
pulp-view-audio defines. A single-pass linker (GNU ld) then fails for any
consumer that links view-core without view-audio.

This gate reads the archives themselves. For each checked library it lists the
undefined symbols of every object and fails on any that pulp-view-audio
defines, naming the object and the demangled symbol. "Defines" includes weak
definitions (W/V): Meter's typeinfo is often weak, and a library that needs it
needs pulp-view-audio. The reading and the verdict are
archive_symbol_closure_check.py's, run with --forbidden-defines any. A library may carry a
named debt (--debt LIB=REASON) while a known edge is being removed; a debt that
no longer matches anything is itself a failure, so a fixed edge cannot leave a
stale exemption behind.

Exit codes: 0 = clean, 1 = violation or stale debt, 2 = unusable input
(missing archive, nm failed, or the view-audio archive defines nothing, which
would make every check pass vacuously).
"""
from __future__ import annotations

import argparse
import pathlib
import shutil
import subprocess
import sys


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--nm", default=shutil.which("nm") or "nm")
    parser.add_argument("--audio", type=pathlib.Path, required=True,
                        help="the pulp-view-audio archive")
    parser.add_argument("--check", action="append", default=[], metavar="NAME=ARCHIVE",
                        help="a library that must not need pulp-view-audio's symbols")
    parser.add_argument("--debt", action="append", default=[], metavar="NAME=REASON",
                        help="a library allowed to need them while a tracked edge is removed")
    args = parser.parse_args(argv)

    if not args.check:
        print("view-audio-symbol-floor: no --check library given", file=sys.stderr)
        return 2
    checker = pathlib.Path(__file__).with_name("archive_symbol_closure_check.py")
    command = [sys.executable, str(checker), "--nm", args.nm, "--forbidden", str(args.audio),
               "--forbidden-defines", "any", "--no-control"]
    for item in args.check:
        command.extend(["--check", item])
    for item in args.debt:
        command.extend(["--debt", item])
    proc = subprocess.run(command, capture_output=True, text=True, encoding="utf-8")
    if proc.stdout:
        print(proc.stdout, end="")
    if proc.stderr:
        print(proc.stderr, end="", file=sys.stderr)
    return proc.returncode


if __name__ == "__main__":
    raise SystemExit(main())

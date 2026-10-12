#!/usr/bin/env python3
"""view_audio_symbol_floor.py — no view library may need pulp-view-audio's code.

The include-boundary and link-closure gates read headers and target links. They
cannot see a symbol reference: a view-core source that dynamic_casts to Meter
names only a view-core header, yet its object needs Meter's typeinfo, which
pulp-view-audio defines. A single-pass linker (GNU ld) then fails for any
consumer that links view-core without view-audio.

This gate reads the archives themselves. For each checked library it lists the
undefined symbols of every object and fails on any that pulp-view-audio
defines, naming the object and the demangled symbol. A library may carry a
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
import re
import shutil
import subprocess
import sys

LINE_RE = re.compile(r"^(?P<archive>.*?)\[(?P<object>[^\]]+)\]: (?P<symbol>\S+) (?P<type>\S)")


def nm_listing(nm: str, archive: pathlib.Path) -> str:
    proc = subprocess.run([nm, "-A", "-P", "-g", str(archive)], capture_output=True,
                          text=True, encoding="utf-8", errors="replace", check=False)
    if proc.returncode != 0:
        raise RuntimeError(f"{nm} failed on {archive}: {proc.stderr.strip()}")
    return proc.stdout


def parse(listing: str) -> tuple[set[str], list[tuple[str, str]]]:
    """(defined symbols, [(object, undefined symbol)]) from `nm -A -P -g` text."""
    defined: set[str] = set()
    undefined: list[tuple[str, str]] = []
    for line in listing.splitlines():
        m = LINE_RE.match(line)
        if not m:
            continue
        if m["type"] == "U":
            undefined.append((m["object"], m["symbol"]))
        elif m["type"] not in ("w", "v"):
            defined.add(m["symbol"])
    return defined, undefined


def offenders(audio_defined: set[str], undefined: list[tuple[str, str]]) -> list[tuple[str, str]]:
    return sorted({(obj, sym) for obj, sym in undefined if sym in audio_defined})


def demangle(symbols: list[str]) -> list[str]:
    tool = shutil.which("c++filt") or shutil.which("llvm-cxxfilt")
    if not tool or not symbols:
        return symbols
    proc = subprocess.run([tool], input="\n".join(symbols), capture_output=True, text=True,
                          encoding="utf-8", errors="replace", check=False)
    out = proc.stdout.splitlines()
    return out if proc.returncode == 0 and len(out) == len(symbols) else symbols


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

    checks = dict(item.split("=", 1) for item in args.check)
    debts = dict(item.split("=", 1) for item in args.debt)
    if not checks:
        print("view-audio-symbol-floor: no --check library given", file=sys.stderr)
        return 2
    try:
        audio_defined, _ = parse(nm_listing(args.nm, args.audio))
        if not audio_defined:
            print(f"view-audio-symbol-floor: {args.audio} defines no symbols; "
                  "refusing to report a clean result", file=sys.stderr)
            return 2
        failed = False
        for name, archive in checks.items():
            _, undefined = parse(nm_listing(args.nm, pathlib.Path(archive)))
            hits = offenders(audio_defined, undefined)
            if name in debts:
                if hits:
                    print(f"view-audio-symbol-floor: {name}: {len(hits)} reference(s), "
                          f"allowed as debt: {debts[name]}")
                else:
                    print(f"view-audio-symbol-floor: {name}: debt '{debts[name]}' no longer "
                          "matches anything; remove it")
                    failed = True
                continue
            if hits:
                failed = True
                print(f"view-audio-symbol-floor: {name} needs pulp-view-audio code:")
                for (obj, _), pretty in zip(hits, demangle([s for _, s in hits])):
                    print(f"  {obj}: {pretty}")
            else:
                print(f"view-audio-symbol-floor: {name}: clean")
    except RuntimeError as exc:
        print(f"view-audio-symbol-floor: {exc}", file=sys.stderr)
        return 2
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Check a Mach-O plug-in binary against Pulp's footprint budgets.

A plug-in's footprint is what the host maps when it loads the bundle. The
check measures it deterministically from the binary itself:

* ``mapped_bytes`` - the file size of every segment except ``__LINKEDIT``.
  This is the code and data that dyld maps, independent of whether the
  symbol table was stripped, so it is stable across strip settings.
* ``icu_data_bytes`` - the size of the embedded ICU data package
  (``_icudtNN_dat``), which Pulp links trimmed (see
  ``tools/cmake/scripts/icu_data_subset.py``).
* forbidden symbols - for an editor-less plug-in, any symbol from the view,
  canvas, GPU or JavaScript stack means the link graph pulled in the UI
  runtime.

Budgets live in ``plugin_footprint_budget.json`` beside this script, keyed by
profile. Raising a budget is a deliberate, reviewed edit of that file.

Usage::

    plugin_footprint_check.py --profile editor BINARY [BINARY...]
    plugin_footprint_check.py --profile no-editor --json BINARY

Exit status: 0 within budget, 1 over budget, 2 usage or unreadable input,
77 when the host has no ``nm`` (non-Apple), which ctest reports as skipped.
"""

from __future__ import annotations

import argparse
import json
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

BUDGET_FILE = Path(__file__).with_name("plugin_footprint_budget.json")

_FAT_MAGIC = 0xCAFEBABE
_FAT_MAGIC_64 = 0xCAFEBABF
_MH_MAGIC_64 = 0xFEEDFACF
_LC_SEGMENT_64 = 0x19
_CPU_ARM64 = 0x0100000C
_CPU_X86_64 = 0x01000007
_ARCH_NAMES = {_CPU_ARM64: "arm64", _CPU_X86_64: "x86_64"}


class FootprintError(RuntimeError):
    pass


def macho_slices(data: bytes) -> dict[str, tuple[int, int]]:
    """Map arch name -> (offset, size) of each 64-bit Mach-O slice."""
    magic_be = struct.unpack_from(">I", data, 0)[0]
    if magic_be in (_FAT_MAGIC, _FAT_MAGIC_64):
        count = struct.unpack_from(">I", data, 4)[0]
        out = {}
        for i in range(count):
            if magic_be == _FAT_MAGIC:
                cpu, _sub, off, size, _align = struct.unpack_from(">iiIII", data, 8 + 20 * i)
            else:
                cpu, _sub, off, size, _align, _res = struct.unpack_from(">iiQQII", data, 8 + 32 * i)
            out[_ARCH_NAMES.get(cpu & 0xFFFFFFFF, f"cpu{cpu}")] = (off, size)
        return out
    magic, cpu = struct.unpack_from("<Ii", data, 0)
    if magic != _MH_MAGIC_64:
        raise FootprintError("not a 64-bit Mach-O binary")
    return {_ARCH_NAMES.get(cpu & 0xFFFFFFFF, f"cpu{cpu}"): (0, len(data))}


def segment_sizes(data: bytes, offset: int = 0) -> dict[str, int]:
    magic, _cpu, _sub, _ft, ncmds, _sz, _flags, _res = struct.unpack_from("<IiiIIIII", data, offset)
    if magic != _MH_MAGIC_64:
        raise FootprintError("slice is not a 64-bit Mach-O image")
    pos = offset + 32
    out: dict[str, int] = {}
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", data, pos)
        if cmd == _LC_SEGMENT_64:
            name = data[pos + 8:pos + 24].split(b"\0", 1)[0].decode("ascii")
            filesize = struct.unpack_from("<Q", data, pos + 48)[0]
            out[name] = out.get(name, 0) + filesize
        pos += cmdsize
    return out


def symbol_table(binary: Path, arch: str) -> list[tuple[int, str]]:
    res = subprocess.run(["nm", "-n", "-arch", arch, str(binary)],
                         capture_output=True, text=True, encoding="utf-8")
    if res.returncode != 0:
        raise FootprintError(f"nm failed: {res.stderr.strip()}")
    out = []
    for line in res.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 3 and re.fullmatch(r"[0-9a-fA-F]+", parts[0]):
            out.append((int(parts[0], 16), parts[-1]))
    return out


def icu_data_bytes(symbols: list[tuple[int, str]], segment_end: int | None = None) -> int:
    """Size of _icudtNN_dat: distance to the next higher symbol address."""
    for i, (addr, name) in enumerate(symbols):
        if re.fullmatch(r"_icudt\d+_dat", name):
            for later, _ in symbols[i + 1:]:
                if later > addr:
                    return later - addr
            return (segment_end or addr) - addr
    return 0


def measure(binary: Path) -> dict:
    data = binary.read_bytes()
    report = {"binary": str(binary), "file_bytes": len(data), "slices": {}}
    for arch, (off, _size) in macho_slices(data).items():
        segs = segment_sizes(data, off)
        symbols = symbol_table(binary, arch)
        report["slices"][arch] = {
            "mapped_bytes": sum(v for k, v in segs.items() if k != "__LINKEDIT"),
            "linkedit_bytes": segs.get("__LINKEDIT", 0),
            "icu_data_bytes": icu_data_bytes(symbols),
            "symbol_names": [name for _, name in symbols],
        }
    return report


def evaluate(report: dict, budget: dict) -> list[str]:
    failures = []
    forbidden = [re.compile(p) for p in budget.get("forbidden_symbol_patterns", [])]
    arches = budget.get("arches")
    for arch, s in report["slices"].items():
        if arches is not None and arch not in arches:
            continue
        where = f"{report['binary']} [{arch}]"
        limit = budget.get("max_mapped_bytes")
        if limit is not None and s["mapped_bytes"] > limit:
            failures.append(f"{where}: mapped {s['mapped_bytes']} bytes > budget {limit}")
        limit = budget.get("max_icu_data_bytes")
        if limit is not None and s["icu_data_bytes"] > limit:
            failures.append(f"{where}: ICU data {s['icu_data_bytes']} bytes > budget {limit}")
        if budget.get("require_icu_data") and s["icu_data_bytes"] == 0:
            failures.append(f"{where}: no _icudtNN_dat symbol; the measurement cannot see ICU data")
        hits = sorted({n for n in s["symbol_names"] for p in forbidden if p.search(n)})
        if hits:
            failures.append(f"{where}: {len(hits)} forbidden symbol(s), e.g. {', '.join(hits[:5])}")
    return failures


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--profile", required=True)
    ap.add_argument("--budget-file", type=Path, default=BUDGET_FILE)
    ap.add_argument("--json", action="store_true", help="print the measurement as JSON")
    ap.add_argument("binaries", nargs="+", type=Path)
    args = ap.parse_args(argv)

    if shutil.which("nm") is None:
        print("plugin_footprint_check: nm not available; skipping")
        return 77
    try:
        budgets = json.loads(args.budget_file.read_text(encoding="utf-8"))["profiles"]
        budget = budgets[args.profile]
    except (OSError, KeyError, ValueError) as exc:
        print(f"plugin_footprint_check: cannot read profile {args.profile!r}: {exc}", file=sys.stderr)
        return 2

    failures = []
    for binary in args.binaries:
        try:
            report = measure(binary)
        except (OSError, FootprintError, struct.error) as exc:
            print(f"plugin_footprint_check: {binary}: {exc}", file=sys.stderr)
            return 2
        failures += evaluate(report, budget)
        for arch, s in report["slices"].items():
            line = (f"{binary.name} [{arch}]: mapped={s['mapped_bytes']} "
                    f"linkedit={s['linkedit_bytes']} icu_data={s['icu_data_bytes']}")
            if budget.get("arches") is not None and arch not in budget["arches"]:
                line += " (no budget for this architecture; not checked)"
            print(line)
        if args.json:
            for s in report["slices"].values():
                s.pop("symbol_names")
            print(json.dumps(report, indent=2, sort_keys=True))
    for f in failures:
        print(f"FAIL: {f}", file=sys.stderr)
    if failures:
        print(f"Budgets: {args.budget_file} (profile {args.profile}). Raise one only "
              "with the measurement that justifies it.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

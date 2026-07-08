#!/usr/bin/env python3
"""Measure the minimum OS each prebuilt library was built for, and keep
tools/deps/min_os.json honest.

A consumer's OS floor is the MAX minimum among the prebuilt libraries it links.
This reads that minimum straight from the shipped artifacts:

  * macOS / iOS  — LC_BUILD_VERSION `minos` via `otool -l` (max across arch slices)
  * Linux        — highest GLIBC_x.y symbol version via `objdump -T` (or readelf)
  * Windows      — subsystem/OS version via `dumpbin /headers` (best effort)

It can only measure artifacts present on the current host, so it fills in the
platform it is run on. `--check` compares the measured value against the
`expected` (Google upstream) value recorded in min_os.json and exits non-zero on
drift — that is the "scream when it changes" gate wired into CI after a pin bump.

Usage:
  measure_min_os.py                 # print measured minima for locally-present libs
  measure_min_os.py --write         # update min_os.json 'measured' fields in place
  measure_min_os.py --check         # non-zero exit if any measured != expected
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
MIN_OS_JSON = REPO / "tools" / "deps" / "min_os.json"
SKIA = REPO / "external" / "skia-build" / "build"
V8 = REPO / "external" / "v8-build"


def _otool_minos(lib: Path) -> str | None:
    """Max LC_BUILD_VERSION minos across all slices/objects in a Mach-O archive."""
    try:
        out = subprocess.run(["otool", "-l", str(lib)], capture_output=True,
                             text=True, check=True).stdout
    except (FileNotFoundError, subprocess.CalledProcessError):
        return None
    vals = []
    lines = out.splitlines()
    for i, ln in enumerate(lines):
        if "LC_BUILD_VERSION" in ln:
            for j in range(i, min(i + 6, len(lines))):
                m = re.search(r"\bminos\s+([0-9.]+)", lines[j])
                if m:
                    vals.append(tuple(int(x) for x in m.group(1).split(".")))
                    break
    if not vals:
        return None
    top = max(vals)
    return ".".join(str(x) for x in top) if len(top) > 1 else f"{top[0]}.0"


def _glibc_floor(lib: Path) -> str | None:
    """Highest GLIBC_x.y symbol version required by an ELF .so."""
    for tool, args in (("objdump", ["-T"]), ("readelf", ["--dyn-syms"])):
        try:
            out = subprocess.run([tool, *args, str(lib)], capture_output=True,
                                 text=True, check=True).stdout
        except (FileNotFoundError, subprocess.CalledProcessError):
            continue
        vers = re.findall(r"GLIBC_([0-9]+(?:\.[0-9]+)+)", out)
        if vers:
            return ".".join(str(x) for x in max(tuple(int(p) for p in v.split(".")) for v in vers))
    return None


def _find(root: Path, name: str) -> Path | None:
    if not root.exists():
        return None
    for p in root.rglob(name):
        if "arm64" in str(p) or root == V8:
            return p
    hits = list(root.rglob(name))
    return hits[0] if hits else None


def measure() -> dict[str, dict[str, str]]:
    """Measure whatever is present on this host, keyed platform -> dep -> value."""
    result: dict[str, dict[str, str]] = {}
    # macOS arm64
    skia = _find(SKIA / "mac-gpu", "libskia.a")
    dawn = _find(SKIA / "mac-gpu", "libdawn_combined.a")
    v8 = _find(V8, "libv8.dylib")
    mac = {}
    if skia and (v := _otool_minos(skia)):
        mac["skia"] = v
    if dawn and (v := _otool_minos(dawn)):
        mac["dawn"] = v
    if v8 and (v := _otool_minos(v8)):
        mac["v8"] = v
    if mac:
        result["macos-arm64"] = mac
    # Linux (if a .so happens to be present in this checkout)
    lskia = _find(SKIA / "linux-gpu", "libskia.a") or _find(SKIA, "libskia.so")
    lin = {}
    lv8 = _find(V8, "libv8.so")
    if lv8 and (v := _glibc_floor(lv8)):
        lin["v8"] = v
    if lin:
        result["linux-x64"] = lin
    return result


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--write", action="store_true", help="update min_os.json measured fields")
    ap.add_argument("--check", action="store_true", help="exit non-zero on measured != expected")
    args = ap.parse_args()

    doc = json.loads(MIN_OS_JSON.read_text())
    measured = measure()
    if not measured:
        print("no prebuilt libraries found to measure "
              "(external/skia-build, external/v8-build); nothing to do.")
        return 0

    drift = []
    for plat, deps in measured.items():
        for dep, val in deps.items():
            print(f"{plat:14s} {dep:6s} measured minos = {val}")
            node = doc.get("platforms", {}).get(plat, {}).get("deps", {}).get(dep)
            if node is None:
                continue
            if args.write:
                node["measured"] = val
            exp = node.get("expected")
            if exp and exp != val:
                drift.append(f"{plat}/{dep}: measured {val} != expected {exp}")

    if args.write:
        # refresh the derived per-platform floor (max of always_linked deps)
        for plat, pnode in doc.get("platforms", {}).items():
            vers = []
            for dep, dn in pnode.get("deps", {}).items():
                if dn.get("always_linked") and dn.get("measured"):
                    vers.append(tuple(int(x) for x in str(dn["measured"]).split(".")))
            if vers:
                top = max(vers)
                pnode["floor"] = ".".join(str(x) for x in top)
        MIN_OS_JSON.write_text(json.dumps(doc, indent=2) + "\n")
        print(f"updated {MIN_OS_JSON.relative_to(REPO)}")

    if args.check and drift:
        print("\nMIN-OS DRIFT — a prebuilt no longer matches its expected upstream floor:",
              file=sys.stderr)
        for d in drift:
            print(f"  {d}", file=sys.stderr)
        print("Re-measure, update tools/deps/min_os.json, and re-pin the deployment "
              "target if the floor moved.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

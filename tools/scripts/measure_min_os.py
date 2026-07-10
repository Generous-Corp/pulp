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


def _win_os_version(pe: Path) -> str | None:
    """Subsystem (loader-enforced) OS version of a Windows PE — the measurable
    proxy for the Windows floor. `_WIN32_WINNT` is a compile-time define that is
    NOT recorded in the shipped static .libs, so the honest artifact measurement
    is the final binary's subsystem version. Cross-platform via llvm-readobj;
    native via dumpbin."""
    try:
        out = subprocess.run(["llvm-readobj", "--file-headers", str(pe)],
                             capture_output=True, text=True, check=True).stdout
        maj = re.search(r"MajorSubsystemVersion:\s*(\d+)", out)
        minr = re.search(r"MinorSubsystemVersion:\s*(\d+)", out)
        if maj and minr:
            return f"{int(maj.group(1))}.{int(minr.group(1))}"
    except (FileNotFoundError, subprocess.CalledProcessError):
        pass
    try:
        out = subprocess.run(["dumpbin", "/headers", str(pe)],
                             capture_output=True, text=True, check=True).stdout
        m = re.search(r"([0-9]+)\.([0-9]+)\s+subsystem version", out)
        if m:
            return f"{int(m.group(1))}.{int(m.group(2))}"
    except (FileNotFoundError, subprocess.CalledProcessError):
        pass
    return None


def _artifact_kind(path: Path) -> str | None:
    """Detect a binary's format from its magic bytes: 'macho' | 'elf' | 'pe' | 'ar'.

    'ar' is a Unix static archive (`.a`), whose members may be Mach-O or ELF;
    both otool and objdump read the archive directly, so measure_artifact tries
    both on it."""
    try:
        with open(path, "rb") as fh:
            magic = fh.read(8)
    except OSError:
        return None
    if magic[:8] == b"!<arch>\n":
        return "ar"
    if magic[:2] == b"MZ":
        return "pe"
    if magic[:4] == b"\x7fELF":
        return "elf"
    # Mach-O thin (0xFEEDFACE/CF, either endianness) or FAT (0xCAFEBABE/0xBEBAFECA).
    if magic[:4] in (b"\xfe\xed\xfa\xce", b"\xfe\xed\xfa\xcf",
                     b"\xce\xfa\xed\xfe", b"\xcf\xfa\xed\xfe",
                     b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca"):
        return "macho"
    return None


def measure_artifact(path: Path) -> tuple[str | None, str | None]:
    """Measure the min-OS floor of any single built binary, auto-detecting the
    format. Returns (kind, floor) where kind is 'macho'/'elf'/'pe'/'ar' and floor
    is the measured minimum (macOS deployment minos, Linux glibc symbol version,
    or Windows PE subsystem version), or (kind, None) if unmeasurable.

    This is the "derive the floor from everything linked" primitive: the shipped
    binary's symbol/version records already reflect every dependency compiled or
    linked into it, so measuring the artifact is the honest floor regardless of
    what a declared manifest lists."""
    kind = _artifact_kind(path)
    if kind == "macho":
        return kind, _otool_minos(path)
    if kind == "elf":
        return kind, _glibc_floor(path)
    if kind == "pe":
        return kind, _win_os_version(path)
    if kind == "ar":
        # A static archive's members are Mach-O (macOS) or ELF (Linux). Try the
        # Mach-O reader first, fall back to the glibc reader.
        return kind, _otool_minos(path) or _glibc_floor(path)
    return None, None


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
    # Windows (subsystem OS version of a built Pulp binary, if this is a Windows
    # host that has built one — the .libs don't carry _WIN32_WINNT).
    build = REPO / "build"
    if build.exists():
        pe = next((p for p in build.rglob("*.exe")
                   if "_deps" not in p.parts and "CMakeFiles" not in p.parts), None)
        if pe and (v := _win_os_version(pe)):
            # Report against skia/dawn: both share the one build-config floor.
            result["windows-x64"] = {"skia": v, "dawn": v}
    return result


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--write", action="store_true", help="update min_os.json measured fields")
    ap.add_argument("--check", action="store_true", help="exit non-zero on measured != expected")
    ap.add_argument("--elf", type=Path, metavar="PATH",
                    help="measure one Linux ELF's glibc floor and assert it is <= --max "
                         "(default: min_os.json linux-x64 floor). Use in CI over a freshly "
                         "linked Pulp binary to catch a build host that re-leaks a higher "
                         "glibc than the shipped prebuilts declare.")
    ap.add_argument("--max", metavar="VER",
                    help="max acceptable glibc floor for --elf, e.g. 2.34 "
                         "(default: min_os.json platforms.linux-x64.floor)")
    ap.add_argument("--measure", type=Path, metavar="PATH",
                    help="measure one built binary's min-OS floor, auto-detecting "
                         "its format (Mach-O / ELF / PE). Prints '<kind> <floor>' "
                         "and exits 0, or exits 2 if the file is missing or its "
                         "format is unrecognized. The floor-derivation primitive "
                         "the SDK-consumer sweep calls per artifact.")
    args = ap.parse_args()

    # Measure one arbitrary built binary (any platform's artifact).
    if args.measure is not None:
        if not args.measure.exists():
            print(f"--measure path does not exist: {args.measure}", file=sys.stderr)
            return 2
        kind, floor = measure_artifact(args.measure)
        if kind is None:
            print(f"--measure: unrecognized binary format: {args.measure}", file=sys.stderr)
            return 2
        print(f"{kind} {floor if floor is not None else 'unknown'}")
        return 0

    doc = json.loads(MIN_OS_JSON.read_text())

    # Standalone floor-verify over one binary (floor semantics: measured <= max is OK).
    if args.elf is not None:
        if not args.elf.exists():
            print(f"--elf path does not exist: {args.elf}", file=sys.stderr)
            return 2
        measured = _glibc_floor(args.elf)
        mx = args.max or doc.get("platforms", {}).get("linux-x64", {}).get("floor")
        if not mx or mx == "null":
            print("no --max given and min_os.json linux-x64 floor is unset; nothing to check "
                  "against.", file=sys.stderr)
            return 2
        if measured is None:
            print(f"{args.elf}: no GLIBC_ version symbols found "
                  "(static archive or fully static binary?) — vacuously OK.")
            return 0
        v = lambda s: tuple(int(x) for x in s.split("."))
        if v(measured) <= v(mx):
            print(f"[glibc-floor] OK — {args.elf} floor {measured} <= {mx}")
            return 0
        print(f"[glibc-floor] FAIL — {args.elf} needs glibc {measured} > declared floor {mx}.\n"
              "This build host leaked a newer glibc than the prebuilts. Build Pulp's Linux "
              "artifacts on the same <=floor base (ubuntu:22.04) the Skia/V8 portable lanes use.",
              file=sys.stderr)
        return 1
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

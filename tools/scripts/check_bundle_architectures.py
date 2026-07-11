#!/usr/bin/env python3
"""Fail when a macOS bundle's binaries do not carry the expected architectures
(and a valid code signature).

The G3 universal-build footgun: a plugin/app binary is built universal
(arm64 + x86_64), but an EMBEDDED runtime dylib is still thin. wgpu-native
ships no universal dylib, so without PulpWgpuUniversal.cmake the bundle's
`libwgpu_native.dylib` is arm64-only; the app then crashes at load on an Intel
Mac ("no suitable image found") even though the main binary is universal. The
same trap exists for any thin embedded dylib (e.g. libv8.dylib). And a fat
dylib produced by a naive `lipo -create` is NOT validly signed — `codesign
--verify` fails and the arm64 slice is killed at load — so architecture parity
alone is not enough; the signature must verify too.

This scanner asserts, for the bundle's main binary AND every embedded Mach-O
dylib:
  1. `lipo -archs` == exactly the requested architecture set, and
  2. `codesign --verify` passes (a valid, if ad-hoc, signature).

Usage:
  check_bundle_architectures.py <bundle-or-binary> --archs arm64,x86_64 \
      [--strict] [--label NAME] [--no-verify-signature]

Exit codes: 0 = all binaries match (or warn-only), 1 = findings in --strict
mode, 2 = bad usage / unreadable / lipo missing.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys


def parse_archs(spec: str) -> set[str]:
    """Split an --archs value ('arm64,x86_64' or 'arm64;x86_64' or spaces)
    into a normalized set."""
    parts = spec.replace(";", ",").replace(" ", ",").split(",")
    return {p.strip() for p in parts if p.strip()}


# ── Pure decision core (front-ends injected for testability) ─────────────────

def check_binary(binary, expected, lipo_fn, sign_fn, verify_signature=True):
    """Return a list of findings (strings) for one Mach-O binary.

    `lipo_fn(path)` → list[str] of archs, or None if the file is not a Mach-O
    (skipped). `sign_fn(path)` → bool (signature verifies). `expected` is the
    required arch set. Pure given those two callables."""
    archs = lipo_fn(binary)
    if archs is None:
        return []  # not a Mach-O (Info.plist, resource, symlink target) — skip
    findings = []
    have = set(archs)
    if have != expected:
        missing = expected - have
        extra = have - expected
        detail = []
        if missing:
            detail.append(f"missing {sorted(missing)}")
        if extra:
            detail.append(f"unexpected {sorted(extra)}")
        findings.append(
            f"architecture mismatch: has {sorted(have)}, "
            f"want {sorted(expected)} ({'; '.join(detail)})")
    if verify_signature and not sign_fn(binary):
        findings.append(
            "code signature does not verify (codesign --verify failed) — a raw "
            "lipo'd fat dylib must be ad-hoc re-signed, or the arm64 slice is "
            "killed at load")
    return findings


def evaluate(binaries, expected, lipo_fn, sign_fn, relpath_fn, verify_signature=True):
    """Run check_binary over every binary; return {relpath: [findings]} for
    binaries that had findings, and the count of Mach-O binaries actually
    inspected. Pure given the injected callables."""
    results = {}
    inspected = 0
    for b in binaries:
        archs = lipo_fn(b)
        if archs is None:
            continue
        inspected += 1
        findings = check_binary(b, expected, lipo_fn, sign_fn, verify_signature)
        if findings:
            results[relpath_fn(b)] = findings
    return results, inspected


# ── macOS front-ends ─────────────────────────────────────────────────────────

def lipo_archs(path):
    """Architectures in a Mach-O via `lipo -archs`, or None if not a Mach-O."""
    try:
        out = subprocess.run(["lipo", "-archs", path],
                             capture_output=True, text=True)
    except FileNotFoundError:
        raise
    if out.returncode != 0:
        return None  # "not a valid Mach-O file" etc.
    return out.stdout.split()


def codesign_ok(path):
    """True if `codesign --verify` accepts the binary's signature."""
    r = subprocess.run(["codesign", "--verify", "--strict", path],
                       capture_output=True, text=True)
    return r.returncode == 0


def macho_binaries(path):
    """Yield candidate Mach-O binaries: the main executable + embedded dylibs.

    Mirrors check_bundle_relocatable._macho_binaries: for a bundle, the main
    executable in Contents/MacOS plus any nested dylibs; for a bare path, the
    path itself."""
    if os.path.isfile(path):
        yield path
        return
    macos = os.path.join(path, "Contents", "MacOS")
    seen = set()
    if os.path.isdir(macos):
        for name in sorted(os.listdir(macos)):
            full = os.path.join(macos, name)
            if os.path.isfile(full):
                seen.add(os.path.realpath(full))
                yield full
    # Also sweep the whole bundle for dylibs (Frameworks, Resources) so an
    # embedded thin dylib anywhere in the bundle is caught, not just next to
    # the executable.
    for root, _dirs, files in os.walk(path):
        for name in sorted(files):
            if name.endswith(".dylib"):
                full = os.path.join(root, name)
                if os.path.realpath(full) not in seen:
                    seen.add(os.path.realpath(full))
                    yield full


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("target",
                    help="a .app/.component/.vst3/.clap bundle or a Mach-O binary")
    ap.add_argument("--archs", required=True,
                    help="required architectures, e.g. 'arm64,x86_64' or 'arm64'")
    ap.add_argument("--strict", action="store_true",
                    help="exit 1 on findings (default: warn only)")
    ap.add_argument("--label", default="", help="name to show in messages")
    ap.add_argument("--no-verify-signature", action="store_true",
                    help="check architectures only, skip codesign --verify")
    args = ap.parse_args(argv)

    if not os.path.exists(args.target):
        print(f"check_bundle_architectures: not found: {args.target}", file=sys.stderr)
        return 2
    if subprocess.run(["which", "lipo"], capture_output=True).returncode != 0:
        print("check_bundle_architectures: lipo not found (macOS only)", file=sys.stderr)
        return 2

    expected = parse_archs(args.archs)
    if not expected:
        print("check_bundle_architectures: --archs is empty", file=sys.stderr)
        return 2

    label = args.label or os.path.basename(args.target)

    def relpath(b):
        return os.path.relpath(b, args.target) if os.path.isdir(args.target) else b

    results, inspected = evaluate(
        macho_binaries(args.target), expected,
        lipo_archs, codesign_ok, relpath,
        verify_signature=not args.no_verify_signature)

    if inspected == 0:
        print(f"check_bundle_architectures: {label} — no Mach-O binaries found "
              f"to inspect under {args.target}", file=sys.stderr)
        return 2

    if not results:
        print(f"check_bundle_architectures: {label} — all {inspected} Mach-O "
              f"binary/binaries are {sorted(expected)}"
              + ("" if args.no_verify_signature else " and validly signed") + " ✅")
        return 0

    tag = "ERROR" if args.strict else "WARNING"
    print(f"{tag}: {label} — {len(results)} of {inspected} Mach-O binary/binaries "
          f"do not match the required architectures {sorted(expected)}:",
          file=sys.stderr)
    for rel, findings in sorted(results.items()):
        for f in findings:
            print(f"  - {rel}: {f}", file=sys.stderr)
    print("  Fix: build every embedded dylib universal (see "
          "PulpWgpuUniversal.cmake for the wgpu-native lipo+re-sign recipe) and "
          "sign the bundle after lipo.", file=sys.stderr)
    return 1 if args.strict else 0


if __name__ == "__main__":
    sys.exit(main())

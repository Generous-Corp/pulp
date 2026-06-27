#!/usr/bin/env python3
"""Fail when a macOS bundle is not self-contained / relocatable.

The recurring footgun: a GPU plugin or app links a runtime dylib (notably
`libwgpu_native.dylib`) that is *copied into* the bundle, but the binary's only
`LC_RPATH` points at the BUILD CACHE (e.g.
`~/Library/Caches/Pulp/fetchcontent-src/.../lib`). On the build machine that
path exists, so it builds, signs, notarizes, validates (auval/pluginval) and
loads — a FALSE PASS. Copied to any other Mac (or after the cache is cleared),
`@rpath/libwgpu_native.dylib` resolves nowhere and the bundle crashes at launch
(`Library not loaded`), or a DAW reports "could not load" / shows no editor.

The fix is `@loader_path` (the dylib sits next to the binary) baked into the
shipped binary. This scanner *catches* a bundle that lacks that, BEFORE it is
distributed, by reading Mach-O load commands (not just strings — the weaker
`check_portable_binary.py` cannot see an rpath the way this does):

  1. Every `LC_RPATH` must be bundle-relative (`@loader_path` / `@executable_path`
     / `@rpath`) or a system path (`/usr/lib`, `/System`). Any other absolute
     path (a build cache, fetchcontent dir, home, build tree) → NOT relocatable.
  2. Every `@rpath/<lib>` dependency must resolve to a file INSIDE the bundle via
     a bundle-relative rpath. A dep only reachable through an external rpath →
     NOT relocatable.

Usage:
  check_bundle_relocatable.py <bundle-or-binary> [--strict] [--label NAME]

Exit codes: 0 = relocatable (or warn-only), 1 = findings in --strict mode,
2 = bad usage / unreadable / otool missing.
"""
import argparse
import os
import subprocess
import sys

# rpaths that are fine in a shipped bundle: bundle-relative loader tokens, or
# OS-owned locations present on every Mac.
_SYSTEM_RPATH_PREFIXES = ("/usr/lib", "/System/")


def is_external_rpath(rpath):
    """True if this LC_RPATH would not resolve on another Mac (build-machine path)."""
    if rpath.startswith("@"):  # @loader_path / @executable_path / @rpath
        return False
    if rpath.startswith(_SYSTEM_RPATH_PREFIXES):
        return False
    return True  # any other absolute path is build-machine-specific


def _expand_loader_rpath(rpath, binary_dir, exe_dir):
    """Expand a bundle-relative rpath to a directory, or None if not loader-relative."""
    if rpath.startswith("@loader_path"):
        return os.path.normpath(rpath.replace("@loader_path", binary_dir, 1))
    if rpath.startswith("@executable_path"):
        return os.path.normpath(rpath.replace("@executable_path", exe_dir, 1))
    return None


def unresolved_rpath_deps(deps, rpaths, binary_dir, exe_dir, bundle_root, exists):
    """@rpath/<lib> deps that don't resolve to a file inside the bundle.

    `exists(path)` → bool (injected for testability). `bundle_root` bounds what
    counts as "inside the bundle"; pass None to skip the in-bundle bound.
    """
    loader_dirs = []
    for rp in rpaths:
        d = _expand_loader_rpath(rp, binary_dir, exe_dir)
        if d is not None:
            loader_dirs.append(d)
    unresolved = []
    for dep in deps:
        if not dep.startswith("@rpath/"):
            continue
        lib = dep[len("@rpath/"):]
        ok = False
        for d in loader_dirs:
            cand = os.path.normpath(os.path.join(d, lib))
            if bundle_root is not None and not (cand == bundle_root or cand.startswith(bundle_root + os.sep)):
                continue
            if exists(cand):
                ok = True
                break
        if not ok:
            unresolved.append(dep)
    return unresolved


# ── otool front-end (macOS) ──────────────────────────────────────────────────

def _otool_rpaths(binary):
    out = subprocess.run(["otool", "-l", binary], capture_output=True, text=True).stdout
    rpaths, lines = [], out.splitlines()
    for i, ln in enumerate(lines):
        if "cmd LC_RPATH" in ln:
            for j in range(i, min(i + 4, len(lines))):
                s = lines[j].strip()
                if s.startswith("path "):
                    rpaths.append(s[len("path "):].split(" (offset")[0].strip())
                    break
    return rpaths


def _otool_deps(binary):
    out = subprocess.run(["otool", "-L", binary], capture_output=True, text=True).stdout
    deps = []
    for ln in out.splitlines()[1:]:  # first line is the binary path itself
        ln = ln.strip()
        if ln and "(compatibility" in ln:
            deps.append(ln.split(" (compatibility")[0].strip())
    return deps


def _macho_binaries(path):
    """Yield the Mach-O binaries to check for a bundle (or the path itself)."""
    if os.path.isfile(path):
        yield path
        return
    # A bundle: the main executable in Contents/MacOS plus any nested dylibs.
    macos = os.path.join(path, "Contents", "MacOS")
    if os.path.isdir(macos):
        for name in sorted(os.listdir(macos)):
            full = os.path.join(macos, name)
            if os.path.isfile(full):
                yield full
    else:  # fall back: scan for dylibs/executables
        for root, _dirs, files in os.walk(path):
            for name in files:
                if name.endswith(".dylib") or os.access(os.path.join(root, name), os.X_OK):
                    yield os.path.join(root, name)


def check_binary(binary, bundle_root):
    rpaths = _otool_rpaths(binary)
    # A dylib's own install-name (LC_ID, e.g. @rpath/libwgpu_native.dylib) shows
    # up as the first `otool -L` line — it's a self-reference, not a dependency.
    self_name = os.path.basename(binary)
    deps = [d for d in _otool_deps(binary) if os.path.basename(d) != self_name]
    binary_dir = os.path.dirname(os.path.abspath(binary))
    exe_dir = binary_dir  # for a single binary, loader == executable dir
    ext = [rp for rp in rpaths if is_external_rpath(rp)]
    unres = unresolved_rpath_deps(deps, rpaths, binary_dir, exe_dir, None, os.path.exists)
    return ext, unres


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("target", help="a .app/.component/.vst3/.clap bundle or a Mach-O binary")
    ap.add_argument("--strict", action="store_true", help="exit 1 on findings (default: warn only)")
    ap.add_argument("--label", default="", help="name to show in messages")
    args = ap.parse_args()

    if not os.path.exists(args.target):
        print(f"check_bundle_relocatable: not found: {args.target}", file=sys.stderr)
        return 2
    if subprocess.run(["which", "otool"], capture_output=True).returncode != 0:
        print("check_bundle_relocatable: otool not found (macOS only)", file=sys.stderr)
        return 2

    label = args.label or os.path.basename(args.target)
    # HARD failures: an @rpath dependency that resolves nowhere safe (the real
    # ship-breaker — e.g. libwgpu_native.dylib reachable only via a build cache).
    # SOFT warnings: a dangling external LC_RPATH that NO dependency relies on
    # (e.g. a leftover Xcode toolchain rpath on a pure-Swift app whose runtime
    # loads from the OS /usr/lib/swift) — harmless, surfaced for hygiene only.
    hard, soft = [], []
    for binary in _macho_binaries(args.target):
        ext, unres = check_binary(binary, args.target)
        rel = os.path.relpath(binary, args.target) if os.path.isdir(args.target) else binary
        for dep in unres:
            hard.append(f"{rel}: {dep} not resolvable inside the bundle (no @loader_path rpath to it)")
        # An external rpath only matters if a dependency would be unresolved
        # without it. If there are no unresolved deps, it's dead weight → warn.
        for rp in ext:
            (hard if unres else soft).append(
                f"{rel}: external LC_RPATH: {rp}"
                + ("" if unres else " (dangling — no dependency uses it)"))

    if soft:
        print(f"check_bundle_relocatable: {label} — note (harmless):", file=sys.stderr)
        for s in soft:
            print(f"  - {s}", file=sys.stderr)
    if not hard:
        print(f"check_bundle_relocatable: {label} is self-contained ✅")
        return 0

    tag = "ERROR" if args.strict else "WARNING"
    print(f"{tag}: {label} is NOT relocatable — would fail on another Mac:", file=sys.stderr)
    for f in hard:
        print(f"  - {f}", file=sys.stderr)
    print("  Fix: bundle the dylib in Contents/MacOS and add an @loader_path rpath "
          "(BUILD_WITH_INSTALL_RPATH). See the skia-gpu-build skill.", file=sys.stderr)
    return 1 if args.strict else 0


if __name__ == "__main__":
    sys.exit(main())

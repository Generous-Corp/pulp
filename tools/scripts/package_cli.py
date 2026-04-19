#!/usr/bin/env python3
"""
Package a portable pulp CLI release tarball.

Bundles libwgpu_native.{dylib,so,dll} (and any other transitive
dynamic deps the build dir resolves to) alongside the pulp binary,
rewrites macOS/Linux rpath to @loader_path / $ORIGIN so the tarball
runs on user machines without the build runner's home directory in
the loader path.

Closes #391 — the v0.20.0 macOS tarball baked an LC_RPATH pointing
at /Users/runner/Library/Caches/Pulp/... and crashed on every user
machine. release-cli.yml now invokes this script so the artifact is
self-contained.

Usage (called from .github/workflows/release-cli.yml):
    python3 tools/scripts/package_cli.py \\
        --binary build/tools/cli/pulp \\
        --build-dir build \\
        --platform darwin-arm64 \\
        --out pulp-darwin-arm64.tar.gz

Layout produced inside the tarball:
    pulp                        (the binary, rpath rewritten)
    libwgpu_native.dylib        (or libwgpu_native.so / wgpu_native.dll)

The smoke gate from PR #395 verifies the output runs on a runner
that did NOT build the binary.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import zipfile
from pathlib import Path


def find_wgpu_lib(build_dir: Path, platform: str) -> Path | None:
    """Locate the wgpu-native dylib/so/dll the build linked against.

    Strategy:
    1. Inspect the binary's load commands for the bad absolute rpath
       wgpu fetched into Pulp's cache (most reliable — that's the
       directory the binary ACTUALLY looks in at load time).
    2. Fall back to scanning the build dir + the standard Pulp
       FetchContent cache for libwgpu_native.* if step 1 misses.
    """
    suffix = {
        "darwin-arm64":  "*.dylib",
        "darwin-x64":    "*.dylib",
        "linux-x64":     "*.so",
        "linux-arm64":   "*.so",
        "windows-x64":   "*.dll",
        "windows-arm64": "*.dll",
    }.get(platform, "*")

    # Source cache roots must match what setup.sh /
    # tools/cmake/PulpFetchContent.cmake actually use across platforms,
    # otherwise the packager silently misses the runtime library and
    # ships a tarball that crashes on clean machines (#438 P1 / #397).
    roots: list[Path] = [build_dir]
    home = Path.home()
    # macOS Pulp cache (Title-Case matches setup.sh)
    roots.append(home / "Library" / "Caches" / "Pulp")
    # Linux XDG cache — pulp uses lowercase 'pulp' dir per
    # tools/cmake/PulpFetchContent.cmake + setup.sh
    xdg_cache = os.environ.get("XDG_CACHE_HOME")
    if xdg_cache:
        roots.append(Path(xdg_cache) / "pulp")
        roots.append(Path(xdg_cache) / "Pulp")
    roots.append(home / ".cache" / "pulp")
    roots.append(home / ".cache" / "Pulp")
    # Windows: %LOCALAPPDATA%\Pulp\fetchcontent-src (PulpFetchContent.cmake)
    local_appdata = os.environ.get("LOCALAPPDATA")
    if local_appdata:
        roots.append(Path(local_appdata) / "Pulp")
    # GitHub-hosted runner historical default (kept for backwards
    # compatibility with tarballs built on runners that still hardcode
    # the runner's home directory).
    roots.append(Path("/Users/runner/Library/Caches/Pulp"))

    # De-dupe while preserving order.
    seen: set[Path] = set()
    unique_roots = []
    for root in roots:
        if root not in seen:
            seen.add(root)
            unique_roots.append(root)

    candidates: list[Path] = []
    for root in unique_roots:
        if not root.exists():
            continue
        for path in root.rglob(f"libwgpu_native{suffix.lstrip('*')}"):
            if path.is_file():
                candidates.append(path)
        for path in root.rglob(f"wgpu_native{suffix.lstrip('*')}"):
            if path.is_file():
                candidates.append(path)
    return candidates[0] if candidates else None


def fix_rpath_macos(binary: Path) -> None:
    """Drop every absolute LC_RPATH and add @loader_path."""
    out = subprocess.check_output(
        ["otool", "-l", str(binary)], text=True)
    bad_rpaths: list[str] = []
    in_rpath = False
    for line in out.splitlines():
        s = line.strip()
        if s == "cmd LC_RPATH":
            in_rpath = True
            continue
        if in_rpath and s.startswith("path "):
            m = re.match(r"path (.+) \(offset \d+\)", s)
            if m:
                rp = m.group(1).strip()
                # Anything starting with / (absolute) that isn't already
                # @loader_path / @executable_path / @rpath is bad.
                if rp.startswith("/") or rp.startswith("@"):
                    if rp.startswith("/"):
                        bad_rpaths.append(rp)
            in_rpath = False

    for rp in bad_rpaths:
        print(f"  removing rpath: {rp}", flush=True)
        subprocess.check_call(
            ["install_name_tool", "-delete_rpath", rp, str(binary)])

    print("  adding rpath: @loader_path", flush=True)
    # Idempotent: if @loader_path is already present, install_name_tool
    # exits with an error; suppress and continue.
    subprocess.run(
        ["install_name_tool", "-add_rpath", "@loader_path", str(binary)],
        check=False,
    )

    # Also rewrite the install_name on the bundled wgpu dylib to use
    # @rpath/<basename> if it's currently absolute; pulp's load command
    # references @rpath/libwgpu_native.dylib, so the dylib's own
    # install name doesn't actually matter here, but keeping it sane
    # avoids surprises if someone re-codesigns or moves the bundle.


def fix_rpath_linux(binary: Path) -> None:
    """Use patchelf if available; ensure the binary's RUNPATH is $ORIGIN."""
    if not shutil.which("patchelf"):
        print("  patchelf not on PATH — skipping Linux rpath rewrite. "
              "(Install via apt: `sudo apt install -y patchelf`.)",
              flush=True)
        return
    print("  patchelf --set-rpath '$ORIGIN'", flush=True)
    subprocess.check_call(
        ["patchelf", "--set-rpath", "$ORIGIN", str(binary)])


def write_tarball(out: Path, files: list[Path], inner_names: list[str]) -> None:
    out.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(out, "w:gz") as tar:
        for src, name in zip(files, inner_names):
            tar.add(src, arcname=name)


def write_zip(out: Path, files: list[Path], inner_names: list[str]) -> None:
    out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for src, name in zip(files, inner_names):
            z.write(src, arcname=name)


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--binary", required=True, type=Path)
    p.add_argument("--build-dir", required=True, type=Path)
    p.add_argument("--platform", required=True)
    p.add_argument("--out", required=True, type=Path)
    args = p.parse_args()

    if not args.binary.exists():
        print(f"FAIL: binary not at {args.binary}", file=sys.stderr)
        return 2

    is_windows = args.platform.startswith("windows-")
    is_macos = args.platform.startswith("darwin-")
    is_linux = args.platform.startswith("linux-")

    with tempfile.TemporaryDirectory() as td:
        stage = Path(td)
        bin_name = "pulp.exe" if is_windows else "pulp"
        staged_bin = stage / bin_name
        shutil.copy2(args.binary, staged_bin)
        # Restore exec bit (shutil.copy2 preserves perms but be safe).
        if not is_windows:
            staged_bin.chmod(0o755)

        files = [staged_bin]
        names = [bin_name]

        wgpu = find_wgpu_lib(args.build_dir, args.platform)
        if wgpu is not None and wgpu.exists():
            staged_wgpu = stage / wgpu.name
            shutil.copy2(wgpu, staged_wgpu)
            files.append(staged_wgpu)
            names.append(wgpu.name)
            print(f"bundled: {wgpu} -> {wgpu.name}", flush=True)
        else:
            # Hard failure. Previously this was a warning-and-continue,
            # which lets release-cli.yml treat script success as a pass
            # and publish an artifact without its required runtime
            # library — crashing every user machine on startup.
            # See #438 P1 Codex review on #397 and the v0.15/0.16/0.17
            # release-pipeline history.
            print("ERROR: wgpu native library not found — refusing to "
                  "package an unportable tarball. Searched cache roots "
                  "(see find_wgpu_lib). Verify setup.sh ran and the "
                  "FetchContent cache is populated before running "
                  "package_cli.py.",
                  file=sys.stderr)
            return 1

        # Rewrite rpaths so the bundled dylib is found via @loader_path
        # (macOS) or $ORIGIN (Linux). Windows finds DLLs in the same
        # directory automatically.
        if is_macos:
            print("rewriting macOS rpath", flush=True)
            fix_rpath_macos(staged_bin)
        elif is_linux:
            print("rewriting Linux rpath", flush=True)
            fix_rpath_linux(staged_bin)

        if is_windows:
            write_zip(args.out, files, names)
        else:
            write_tarball(args.out, files, names)

        print(f"wrote {args.out} ({args.out.stat().st_size} bytes)",
              flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())

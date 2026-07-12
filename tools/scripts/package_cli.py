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

Dual-binary support (#783): when `--cpp-binary` is passed,
the tarball also ships `pulp-cpp[.exe]` so the post-swap layout
(Rust `pulp` + C++ `pulp-cpp` delegate) can land both binaries with
one upgrade. Without `--cpp-binary` the script keeps the legacy
single-binary contract, so pre-swap release lanes stay byte-identical.

MCP binary support (#2067): when `--mcp-binary` is passed, the tarball
also ships `pulp-mcp[.exe]` next to `pulp`. The Claude Code plugin's
`.mcp.json` invokes `tools/mcp/pulp-mcp-launcher`, which falls back to
`pulp-mcp` on `$PATH` — so once `~/.pulp/bin/` is on PATH (the existing
install.sh / install.ps1 behavior) the plugin's MCP server "just
works" without the plugin shipping the binary itself. Without
`--mcp-binary` the script omits it, keeping older release lanes
byte-identical.

Usage (called from .github/workflows/release-cli.yml):
    python3 tools/scripts/package_cli.py \\
        --binary build/pulp \\
        --cpp-binary build/tools/cli/pulp-cpp \\
        --mcp-binary build/tools/mcp/pulp-mcp \\
        --build-dir build \\
        --platform darwin-arm64 \\
        --out pulp-darwin-arm64.tar.gz

Layout produced inside the tarball:
    pulp                        (the binary, rpath rewritten)
    pulp-cpp                    (optional, post-swap; rpath rewritten)
    pulp-mcp                    (optional, plugin-MCP server; rpath rewritten)
    libwgpu_native.dylib        (or libwgpu_native.so / wgpu_native.dll)

The smoke gate from PR #395 verifies the output runs on a runner
that did NOT build the binary. Dual-binary support extends the smoke check to
exercise pulp-cpp too when the dual-binary flag is set, and #2067
extends it again to exercise pulp-mcp.
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

    Scans the build dir and the standard Pulp FetchContent cache roots for
    ``libwgpu_native.*``. On macOS the cache can hold BOTH arch slices at once
    (e.g. an arm64 prefetch plus an x86_64 cross-build), so darwin candidates
    are arch-filtered via ``lipo -archs`` to the target arch — otherwise the
    packager can bundle the wrong-arch dylib into the tarball, which only
    surfaces as a load-time crash on the user's machine. ``lipo`` is macOS-only,
    so the filter is a best-effort no-op elsewhere.
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
    # ships a tarball that crashes on clean machines.
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
    if not candidates:
        return None

    # macOS: a warm cache can hold both arm64 and x86_64 wgpu slices, so blindly
    # taking candidates[0] can bundle the wrong arch (an arm64 dylib into an
    # x86_64 cross-build tarball crashes at load). Prefer a candidate whose
    # Mach-O arch matches the target. `lipo` is macOS-only → no-op elsewhere;
    # fat/universal dylibs match any arch. If none match (lipo-less env), fall
    # through to candidates[0] rather than dropping the dylib entirely — the
    # release-cli smoke gate's arch assert is the backstop.
    if platform.startswith("darwin-"):
        want = "arm64" if platform == "darwin-arm64" else "x86_64"
        for cand in candidates:
            try:
                archs = subprocess.run(
                    ["lipo", "-archs", str(cand)],
                    capture_output=True, text=True, check=True,
                ).stdout.split()
            except (OSError, subprocess.CalledProcessError):
                archs = []
            if want in archs:
                return cand
    return candidates[0]


def fix_rpath_macos(binary: Path) -> None:
    """Drop every absolute LC_RPATH and add @loader_path.

    Honors `OTOOL` and `INSTALL_NAME_TOOL` environment overrides so
    Linux-hosted darwin cross builds can run this packager with the
    osxcross/LLVM equivalents (e.g. ``x86_64-apple-darwin24-otool`` and
    ``x86_64-apple-darwin24-install_name_tool``) instead of the native
    macOS tools that don't exist on Linux. Native builds leave both env
    vars unset and inherit the historical hard-coded ``otool`` /
    ``install_name_tool`` from PATH.
    See planning/2026-05-24-linux-hosted-macos-arm64-cross-lane.md.
    """
    otool = os.environ.get("OTOOL", "otool")
    install_name_tool = os.environ.get("INSTALL_NAME_TOOL", "install_name_tool")
    out = subprocess.check_output(
        [otool, "-l", str(binary)], text=True)
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
            [install_name_tool, "-delete_rpath", rp, str(binary)])

    print("  adding rpath: @loader_path", flush=True)
    # Idempotent: if @loader_path is already present, install_name_tool
    # exits with an error; suppress and continue.
    subprocess.run(
        [install_name_tool, "-add_rpath", "@loader_path", str(binary)],
        check=False,
    )

    # Re-sign ad-hoc. Every install_name_tool write above INVALIDATES the
    # binary's code signature, and on Apple Silicon a binary with a stale
    # signature is SIGKILL'd ("killed: 9", exit 137) the instant it launches.
    # GitHub-hosted never hit this because its cargo build produced no absolute
    # LC_RPATHs to strip (a no-op rewrite leaves the original signature
    # intact); the self-hosted Studio build bakes in absolute rpaths, so the
    # rewrite is real and the unsigned result fails the `pulp help` smoke test
    # (2026-06-10 release thread). Honor a CODESIGN env override so a
    # Linux-hosted darwin cross-build can point at the llvm/cctools
    # equivalent; skip with a clear note when no codesign tool is available.
    codesign = os.environ.get("CODESIGN", "codesign")
    # shutil.which() resolves both a bare command on PATH AND an absolute path
    # (returning None if that absolute path doesn't exist or isn't executable),
    # so it is the complete guard. A prior `or os.path.isabs(codesign)` made a
    # NON-EXISTENT absolute CODESIGN look usable, then `subprocess.run(...,
    # check=False)` raised an uncaught FileNotFoundError (check=False suppresses
    # only non-zero exits, not a missing executable) — crashing the packager
    # instead of taking the intended skip-with-note path.
    if shutil.which(codesign):
        print("  re-signing (ad-hoc) after rpath rewrite", flush=True)
        result = subprocess.run(
            [codesign, "--force", "--sign", "-", str(binary)])
        if result.returncode != 0:
            # FATAL, not swallowed. When codesign IS available the ad-hoc
            # re-sign is expected to succeed; a failure leaves the binary with
            # the install_name_tool-invalidated signature, which is SIGKILL'd on
            # launch (Apple Silicon today, and any future non-arm64 darwin lane
            # where the downstream `pulp help` smoke gate would NOT catch it —
            # the gate only reproduces SIGKILL on arm64). Fail here so a broken,
            # unsigned release binary can never ship regardless of the smoke
            # lane's architecture.
            raise SystemExit(
                f"FAIL: codesign re-sign of {binary} failed "
                f"(exit {result.returncode}); refusing to ship a binary that "
                "will be SIGKILL'd on launch.")
    else:
        print(f"  note: '{codesign}' not found — skipping ad-hoc re-sign; "
              "the binary may be SIGKILL'd on Apple Silicon", flush=True)

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


def stage_binary(src: Path, stage: Path, out_name: str, is_windows: bool) -> Path:
    """Copy `src` into `stage/<out_name>`, restore exec bit on Unix."""
    staged = stage / out_name
    shutil.copy2(src, staged)
    if not is_windows:
        staged.chmod(0o755)
    return staged


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--binary", required=True, type=Path,
                   help="Primary user-facing pulp binary (Rust post-swap, C++ pre-swap).")
    p.add_argument("--cpp-binary", required=False, type=Path, default=None,
                   help="Optional pulp-cpp delegate binary (dual-binary tarball).")
    p.add_argument("--mcp-binary", required=False, type=Path, default=None,
                   help="Optional pulp-mcp server binary (#2067 — Claude plugin MCP).")
    p.add_argument("--build-dir", required=True, type=Path)
    p.add_argument("--platform", required=True)
    p.add_argument("--out", required=True, type=Path)
    args = p.parse_args()

    if not args.binary.exists():
        print(f"FAIL: binary not at {args.binary}", file=sys.stderr)
        return 2
    if args.cpp_binary is not None and not args.cpp_binary.exists():
        print(f"FAIL: --cpp-binary not at {args.cpp_binary}", file=sys.stderr)
        return 2
    if args.mcp_binary is not None and not args.mcp_binary.exists():
        print(f"FAIL: --mcp-binary not at {args.mcp_binary}", file=sys.stderr)
        return 2

    is_windows = args.platform.startswith("windows-")
    is_macos = args.platform.startswith("darwin-")
    is_linux = args.platform.startswith("linux-")

    with tempfile.TemporaryDirectory() as td:
        stage = Path(td)
        bin_name = "pulp.exe" if is_windows else "pulp"
        staged_bin = stage_binary(args.binary, stage, bin_name, is_windows)

        files = [staged_bin]
        names = [bin_name]

        # When caller passes --cpp-binary, bundle it as
        # pulp-cpp[.exe]. The Rust pulp's `upgrade --install` looks
        # for this name in the extracted archive and drops it into
        # the sibling slot.
        staged_cpp: Path | None = None
        if args.cpp_binary is not None:
            cpp_name = "pulp-cpp.exe" if is_windows else "pulp-cpp"
            staged_cpp = stage_binary(args.cpp_binary, stage, cpp_name, is_windows)
            files.append(staged_cpp)
            names.append(cpp_name)
            print(f"bundled: {args.cpp_binary} -> {cpp_name}", flush=True)

        # #2067: bundle pulp-mcp so `curl install.sh | sh` drops it into
        # ~/.pulp/bin/, where the Claude Code plugin's MCP launcher
        # picks it up off $PATH. Without this the plugin's MCP server
        # fails with "cannot locate pulp-mcp binary" on every fresh
        # macOS install.
        staged_mcp: Path | None = None
        if args.mcp_binary is not None:
            mcp_name = "pulp-mcp.exe" if is_windows else "pulp-mcp"
            staged_mcp = stage_binary(args.mcp_binary, stage, mcp_name, is_windows)
            files.append(staged_mcp)
            names.append(mcp_name)
            print(f"bundled: {args.mcp_binary} -> {mcp_name}", flush=True)

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
            # The v0.15/0.16/0.17 release-pipeline history showed this
            # must fail before publication.
            print("ERROR: wgpu native library not found — refusing to "
                  "package an unportable tarball. Searched cache roots "
                  "(see find_wgpu_lib). Verify setup.sh ran and the "
                  "FetchContent cache is populated before running "
                  "package_cli.py.",
                  file=sys.stderr)
            return 1

        # Rewrite rpaths so the bundled dylib is found via @loader_path
        # (macOS) or $ORIGIN (Linux). Windows finds DLLs in the same
        # directory automatically. Apply to both staged binaries so
        # `pulp-cpp` can also load the bundled wgpu — without this the
        # delegate path crashes on a clean user machine.
        if is_macos:
            print("rewriting macOS rpath: pulp", flush=True)
            fix_rpath_macos(staged_bin)
            if staged_cpp is not None:
                print("rewriting macOS rpath: pulp-cpp", flush=True)
                fix_rpath_macos(staged_cpp)
            if staged_mcp is not None:
                print("rewriting macOS rpath: pulp-mcp", flush=True)
                fix_rpath_macos(staged_mcp)
        elif is_linux:
            print("rewriting Linux rpath: pulp", flush=True)
            fix_rpath_linux(staged_bin)
            if staged_cpp is not None:
                print("rewriting Linux rpath: pulp-cpp", flush=True)
                fix_rpath_linux(staged_cpp)
            if staged_mcp is not None:
                print("rewriting Linux rpath: pulp-mcp", flush=True)
                fix_rpath_linux(staged_mcp)

        if is_windows:
            write_zip(args.out, files, names)
        else:
            write_tarball(args.out, files, names)

        print(f"wrote {args.out} ({args.out.stat().st_size} bytes)",
              flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())

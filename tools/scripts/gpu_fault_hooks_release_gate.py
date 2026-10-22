#!/usr/bin/env python3
"""Prove a build carries none of the GPU-audio fault-injection test hooks.

The GPU fault-continuity test plug-in (examples/gpu-fault-continuity) can lose
its own GPU device on request, read a run configuration from a fixed path, and
interpose Dawn's proc table. It is built only with
-DPULP_GPU_AUDIO_FAULT_HOOKS=ON. This gate checks a build directory that is
meant to ship, or to be the base for one:

1. the configure did not enable PULP_GPU_AUDIO_FAULT_HOOKS;
2. the generated build declares no fault-continuity target;
3. no library, plug-in or executable in the build contains any fault-hook
   symbol or string (byte scan, so mangled and local symbols are covered);
4. no plug-in bundle's metadata (Info.plist, moduleinfo.json) names the test
   plug-in, including its AU subtype `GpFc`. The four-character code is
   checked only as a parsed AudioComponents subtype: as raw bytes it would
   match unrelated binary data.

`--extra-dir` adds directories outside the build to scan the same way, such
as an install prefix or ~/Library/Audio/Plug-Ins. Archives are opened and
their contents scanned the same way: .zip and .tar.* in-process, .pkg via
`pkgutil --expand-full`, .dmg mounted read-only and hidden (`hdiutil attach
-readonly -nobrowse`) and detached afterwards. An archive that cannot be
opened is itself a finding: the gate fails closed rather than skip it.

Exit codes: 0 clean, 1 a hook was found, 2 the build directory is unusable.
"""
from __future__ import annotations

import argparse
import os
import plistlib
import re
import shutil
import subprocess
import tarfile
import tempfile
import zipfile
import sys
from pathlib import Path

# Identifiers that exist only in examples/gpu-fault-continuity. Mangled C++
# names contain them verbatim; the last three are strings the plug-in embeds.
FORBIDDEN = (
    b"arm_provider_fault",
    b"install_provider_fault_interposer",
    b"resolve_run_config",
    b"default_rendezvous_root",
    b"GpuFaultContinuityProcessor",
    b"PULP_GPU_FAULT_",
    b"pulp.gpu-fault.rendezvous",
    b"gpu-fault-continuity",
)
TEST_AU_SUBTYPE = "GpFc"
BUNDLE_METADATA = ("Info.plist", "moduleinfo.json")
TARGET = re.compile(r"\bPulpGpuFaultContinuity_\w+|\bgpu-fault-continuity-test\b")
BINARY_SUFFIXES = (".a", ".dylib", ".so", ".o", ".dll", ".lib", ".exe")
BUNDLES = (".clap", ".vst3", ".component", ".appex", ".app", ".aaxplugin", ".lv2")
SKIP_DIRS = {"CMakeFiles", "_deps", "Testing", ".cmake"}
ARCHIVE_SUFFIXES = (".zip", ".tar", ".tar.gz", ".tgz", ".tar.bz2", ".tar.xz", ".pkg", ".dmg")
MAX_ARCHIVE_DEPTH = 3


def _cmake_true(value: str) -> bool:
    """CMake's if() truth for a cache value: ON/YES/TRUE/Y or a non-zero number."""
    v = value.strip().upper()
    if v in ("ON", "YES", "TRUE", "Y"):
        return True
    try:
        return float(v) != 0.0
    except ValueError:
        return False


def _cache_value(cache: Path, key: str) -> str | None:
    pattern = re.compile(rf"^{re.escape(key)}:[A-Z]+=(.*)$")
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        match = pattern.match(line)
        if match:
            return match.group(1)
    return None


def _walk(root: Path):
    """os.walk that follows symlinked directories (a Components entry may be a
    link to a build tree), visiting each real directory once."""
    seen: set[str] = set()
    for here, dirs, files in os.walk(root, followlinks=True):
        real = os.path.realpath(here)
        if real in seen:
            dirs[:] = []
            continue
        seen.add(real)
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS and
                   os.path.realpath(os.path.join(here, d)) not in seen]
        yield Path(here), dirs, files


def _candidates(build: Path):
    for here, dirs, files in _walk(build):
        in_bundle = any(part.endswith(BUNDLES) for part in here.parts)
        for name in files:
            path = here / name
            if path.is_symlink() and not path.resolve().is_file():
                continue
            if name.endswith(BINARY_SUFFIXES) or (in_bundle and here.name in ("MacOS",
                                                                               "x86_64-win",
                                                                               "arm64-win")):
                yield path
            elif os.access(path, os.X_OK) and not name.endswith((".sh", ".py", ".cmake")):
                yield path


def _bundle_metadata(build: Path):
    for here, dirs, files in _walk(build):
        if not any(part.endswith(BUNDLES) for part in here.parts):
            continue
        for name in files:
            if name in BUNDLE_METADATA:
                yield here / name


def _metadata_findings(path: Path) -> list[str]:
    found = _scan(path)
    if path.name == "Info.plist":
        try:
            info = plistlib.loads(path.read_bytes())
        except (plistlib.InvalidFileException, ValueError, OSError):
            info = {}
        for component in info.get("AudioComponents", []) if isinstance(info, dict) else []:
            if isinstance(component, dict) and component.get("subtype") == TEST_AU_SUBTYPE:
                found.append(f"AU subtype {TEST_AU_SUBTYPE}")
    return found


def _scan(path: Path) -> list[str]:
    """Forbidden identifiers in the file; a file that cannot be read is
    reported too (the gate fails closed rather than call it clean)."""
    try:
        data = path.read_bytes()
    except OSError as exc:
        return [f"<unreadable: {exc.strerror or exc}>"]
    return [needle.decode() for needle in FORBIDDEN if needle in data]


def _archives(root: Path):
    for here, dirs, files in _walk(root):
        for name in files:
            if name.lower().endswith(ARCHIVE_SUFFIXES):
                path = here / name
                if path.resolve().is_file():
                    yield path


def _detach(mount: Path) -> bool:
    """Detach a disk image this gate mounted; True once it is gone."""
    for extra in ([], ["-force"]):
        try:
            done = subprocess.run(["hdiutil", "detach", str(mount), *extra],
                                  capture_output=True, timeout=120)
        except subprocess.TimeoutExpired:
            continue
        if done.returncode == 0 or not os.path.ismount(mount):
            return True
    return not os.path.ismount(mount)


def _extract(archive: Path, into: Path) -> str | None:
    """Unpack `archive` into `into`; return why it could not be, or None."""
    name = archive.name.lower()
    try:
        if name.endswith(".zip"):
            with zipfile.ZipFile(archive) as z:
                for member in z.infolist():
                    target = (into / member.filename).resolve()
                    if into.resolve() not in target.parents and target != into.resolve():
                        return f"member {member.filename!r} escapes the archive"
                z.extractall(into)
            return None
        if name.endswith((".tar", ".tar.gz", ".tgz", ".tar.bz2", ".tar.xz")):
            with tarfile.open(archive) as tar:
                if hasattr(tarfile, "data_filter"):
                    tar.extractall(into, filter="data")
                else:  # Python < 3.11.4: refuse what the data filter would
                    for member in tar.getmembers():
                        target = (into / member.name).resolve()
                        if (member.issym() or member.islnk() or
                                into.resolve() not in target.parents):
                            return f"member {member.name!r} is a link or escapes the archive"
                    tar.extractall(into)
            return None
        if name.endswith(".pkg"):
            if shutil.which("pkgutil") is None:
                return "pkgutil is not available"
            done = subprocess.run(["pkgutil", "--expand-full", str(archive), str(into / "pkg")],
                                  capture_output=True, text=True, timeout=300, encoding="utf-8")
            return None if done.returncode == 0 else f"pkgutil failed: {done.stderr.strip()}"
        if name.endswith(".dmg"):
            if shutil.which("hdiutil") is None:
                return "hdiutil is not available"
            mount = into / "mnt"
            mount.mkdir()
            try:
                done = subprocess.run(["hdiutil", "attach", "-readonly", "-nobrowse",
                                       "-noautoopen", "-mountpoint", str(mount), str(archive)],
                                      capture_output=True, text=True, timeout=300,
                                      encoding="utf-8")
            except subprocess.TimeoutExpired:
                # It may have mounted before the timeout: never leave it attached.
                _detach(mount)
                return "hdiutil attach timed out"
            if done.returncode != 0:
                _detach(mount)
                return f"hdiutil attach failed: {done.stderr.strip()}"
            try:
                shutil.copytree(mount, into / "dmg", symlinks=True)
            finally:
                detached = _detach(mount)
            return None if detached else f"{archive.name} stayed mounted at {mount}"
    except (OSError, zipfile.BadZipFile, tarfile.TarError, subprocess.SubprocessError) as exc:
        return f"{type(exc).__name__}: {exc}"
    return "unknown archive type"


def scan_tree(root: Path, depth: int = 0) -> list[str]:
    """Binary, bundle-metadata and archive findings under `root`, relative to it."""
    findings: list[str] = []
    for archive in _archives(root):
        rel = archive.relative_to(root)
        if depth >= MAX_ARCHIVE_DEPTH:
            findings.append(f"{rel} is nested deeper than {MAX_ARCHIVE_DEPTH} archives")
            continue
        with tempfile.TemporaryDirectory(prefix="pulp-gate-", ignore_cleanup_errors=True) as tmp:
            why = _extract(archive, Path(tmp))
            if why is not None:
                findings.append(f"{rel} could not be opened to scan ({why})")
                continue
            findings += [f"{rel}: {f}" for f in scan_tree(Path(tmp), depth + 1)]
    for path in _candidates(root):
        hits = _scan(path)
        if hits:
            findings.append(f"{path.relative_to(root)}: {', '.join(hits)}")
    for path in _bundle_metadata(root):
        hits = _metadata_findings(path)
        if hits:
            findings.append(f"{path.relative_to(root)} names {', '.join(hits)}")
    return findings


def scan_extra(path: Path) -> list[str]:
    """An --extra-dir: a directory is scanned like the build, an archive is
    opened and scanned, anything else (missing, a plain file) is a finding:
    a mistyped path must not report clean."""
    if path.is_dir():
        return scan_tree(path.resolve())
    if path.is_file() and path.name.lower().endswith(ARCHIVE_SUFFIXES):
        with tempfile.TemporaryDirectory(prefix="pulp-gate-", ignore_cleanup_errors=True) as tmp:
            stage = Path(tmp)
            (stage / path.name).symlink_to(path.resolve())
            return scan_tree(stage)
    return [f"{path} is not a directory or an archive the gate can open"]


def check(build: Path) -> list[str]:
    """Return every finding; empty means clean."""
    findings: list[str] = []
    cache = build / "CMakeCache.txt"
    value = _cache_value(cache, "PULP_GPU_AUDIO_FAULT_HOOKS")
    if value is not None and _cmake_true(value):
        findings.append(f"configure enabled PULP_GPU_AUDIO_FAULT_HOOKS={value}")
    # Every generator's top-level manifest: Ninja (incl. Multi-Config
    # build-<Config>.ninja), Makefiles, Xcode and Visual Studio projects.
    manifests = sorted({*build.glob("*.ninja"), *build.glob("Makefile"),
                        *build.glob("*.xcodeproj/project.pbxproj"), *build.glob("*.sln"),
                        *build.glob("*.vcxproj")})
    for path in manifests:
        match = TARGET.search(path.read_text(encoding="utf-8", errors="replace"))
        if match:
            findings.append(f"{path.relative_to(build)} declares the test target "
                            f"{match.group(0)}")
    return findings + scan_tree(build)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--build-dir", type=Path, required=True)
    ap.add_argument("--extra-dir", type=Path, action="append", default=[],
                    help="Another tree to scan (an install prefix, a plug-in folder).")
    args = ap.parse_args(argv)
    build = args.build_dir.resolve()
    if not (build / "CMakeCache.txt").is_file():
        print(f"gpu-fault-hooks-release-gate: {build} is not a configured build directory")
        return 2
    findings = check(build)
    for extra in args.extra_dir:
        findings += [f"{extra}: {f}" for f in scan_extra(extra)]
    for finding in findings:
        print(f"gpu-fault-hooks-release-gate: FOUND {finding}")
    if findings:
        return 1
    print(f"gpu-fault-hooks-release-gate: clean ({build})")
    return 0


if __name__ == "__main__":
    sys.exit(main())

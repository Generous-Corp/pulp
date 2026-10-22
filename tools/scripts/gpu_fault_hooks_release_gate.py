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

Exit codes: 0 clean, 1 a hook was found, 2 the build directory is unusable.
"""
from __future__ import annotations

import argparse
import os
import plistlib
import re
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


def _cache_value(cache: Path, key: str) -> str | None:
    pattern = re.compile(rf"^{re.escape(key)}:[A-Z]+=(.*)$")
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        match = pattern.match(line)
        if match:
            return match.group(1)
    return None


def _candidates(build: Path):
    for root, dirs, files in os.walk(build):
        here = Path(root)
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        in_bundle = any(part.endswith(BUNDLES) for part in here.parts)
        for name in files:
            path = here / name
            if path.is_symlink():
                continue
            if name.endswith(BINARY_SUFFIXES) or (in_bundle and here.name in ("MacOS",
                                                                               "x86_64-win",
                                                                               "arm64-win")):
                yield path
            elif os.access(path, os.X_OK) and not name.endswith((".sh", ".py", ".cmake")):
                yield path


def _bundle_metadata(build: Path):
    for root, dirs, files in os.walk(build):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        here = Path(root)
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
    try:
        data = path.read_bytes()
    except OSError:
        return []
    return [needle.decode() for needle in FORBIDDEN if needle in data]


def check(build: Path) -> list[str]:
    """Return every finding; empty means clean."""
    findings: list[str] = []
    cache = build / "CMakeCache.txt"
    value = _cache_value(cache, "PULP_GPU_AUDIO_FAULT_HOOKS")
    if value is not None and value.upper() in ("ON", "1", "TRUE", "YES"):
        findings.append(f"configure enabled PULP_GPU_AUDIO_FAULT_HOOKS={value}")
    for manifest in ("build.ninja", "Makefile"):
        path = build / manifest
        if path.is_file():
            match = TARGET.search(path.read_text(encoding="utf-8", errors="replace"))
            if match:
                findings.append(f"{manifest} declares the test target {match.group(0)}")
    for path in _candidates(build):
        hits = _scan(path)
        if hits:
            findings.append(f"{path.relative_to(build)} contains {', '.join(hits)}")
    for path in _bundle_metadata(build):
        hits = _metadata_findings(path)
        if hits:
            findings.append(f"{path.relative_to(build)} names {', '.join(hits)}")
    return findings


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--build-dir", type=Path, required=True)
    args = ap.parse_args(argv)
    build = args.build_dir.resolve()
    if not (build / "CMakeCache.txt").is_file():
        print(f"gpu-fault-hooks-release-gate: {build} is not a configured build directory")
        return 2
    findings = check(build)
    for finding in findings:
        print(f"gpu-fault-hooks-release-gate: FOUND {finding}")
    if findings:
        return 1
    print(f"gpu-fault-hooks-release-gate: clean ({build})")
    return 0


if __name__ == "__main__":
    sys.exit(main())

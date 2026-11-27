#!/usr/bin/env python3
"""Regenerate or verify every consumption-census profile this host can configure, in one command.

docs/status/consumption-profiles.json carries one profile per feature set, and a
configured build tree measures exactly one of them. A CMake change therefore left
the profile the author happened to configure fresh and the others stale, and the
stale ones broke the local gates of every host that configures to them. This
walks the committed profiles instead of the one local tree:

    census_profiles.py --write            regenerate every profile this host can configure
    census_profiles.py --check            diff every such profile against its committed block
    census_profiles.py --check --cached   same, but skip a profile already verified for these inputs

Each profile is configured in its own configure-only tree (no compile, one
governed background core, removed afterwards) with exactly its features. The two
SDK switches are decided by what exists under external/, so a profile that
records no VST3 or AudioUnit SDK is configured from a symlink overlay of the
checkout that omits them; the checkout itself is never touched. A profile whose
tree configures to a different key than the one asked for (no Skia library on
this host, a platform the census records but this host is not) is reported as
not configurable and left exactly as committed.

An uncommitted result cache (under the git common dir) remembers which profiles
were verified for a digest of every tracked CMake file, the header inventory,
this script, the census generator and the committed census. The digest is
computed in about 0.1 s; a profile whose digest matches is not configured again.

Exit codes: 0 every configurable profile is current, 1 drift (check) or a failed
write, 2 internal error, 3 a profile could not be configured here (and none drifted).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import configure_only  # noqa: E402
import consumption_census as census  # noqa: E402

CACHE_NAME = "pulp-census-verified.json"
SDK_DIRS = {"PULP_HAS_VST3": "vst3sdk", "PULP_HAS_AUSDK": "AudioUnitSDK"}
# Feature switches that are CMake options; the rest of a profile's features are derived.
OPTION_FEATURES = {
    "PULP_HAS_SKIA": "PULP_ENABLE_GPU",
    "PULP_ENABLE_JS": "PULP_ENABLE_JS",
    "PULP_BUILD_WEBVIEW": "PULP_BUILD_WEBVIEW",
    "PULP_ENABLE_DESIGN_IMPORT": "PULP_ENABLE_DESIGN_IMPORT",
    "PULP_ENABLE_INSPECTOR": "PULP_ENABLE_INSPECTOR",
    "PULP_TRACING": "PULP_TRACING",
}
EXIT_DRIFT, EXIT_ERROR, EXIT_NOT_CONFIGURABLE = 1, 2, 3


@dataclass(frozen=True)
class Spec:
    key: str
    features: dict[str, str]

    def sdks(self) -> set[str]:
        return {d for feature, d in SDK_DIRS.items() if self.features.get(feature) == "ON"}

    def cmake_args(self) -> list[str]:
        return [f"-D{option}={self.features[feature]}" for feature, option in OPTION_FEATURES.items()
                if feature in self.features]


def say(msg: str = "") -> None:
    print(msg, file=sys.stderr)


def host_prefix() -> str:
    return f"{platform.system().lower()}-{platform.machine().lower()}-"


def read_census(root: Path) -> dict:
    return json.loads((root / census.CENSUS_RELPATH).read_text(encoding="utf-8"))


def committed_specs(root: Path, document: dict | None = None) -> tuple[list[Spec], list[str]]:
    """(profiles this host could configure, keys recorded for other platforms)."""
    document = document or read_census(root)
    mine, other = [], []
    for key, profile in sorted(document["profiles"].items()):
        if key.startswith(host_prefix()):
            mine.append(Spec(key, profile["features"]))
        else:
            other.append(key)
    return mine, other


def present_sdks(root: Path) -> set[str]:
    return {d for d in SDK_DIRS.values() if (root / "external" / d).exists()}


def gitlink_paths(root: Path) -> set[str]:
    """Top-level submodule paths of the checkout."""
    out = subprocess.run(["git", "-C", str(root), "ls-files", "-s", "-z"], capture_output=True, text=True,
                         encoding="utf-8", errors="replace", check=False).stdout
    return {rec.split("\t", 1)[1] for rec in out.split("\0") if rec.startswith("160000 ") and "/" not in rec.split("\t", 1)[1]}


def make_overlay(root: Path, dest: Path, keep_sdks: set[str]) -> Path:
    """A symlink view of ``root`` whose external/ holds only the SDKs in ``keep_sdks``."""
    # git refuses `status` over a submodule path that is a symlink, and the install provenance
    # check in the configure runs it, so a submodule is an empty directory (an uninitialized one).
    submodules = gitlink_paths(root)
    for entry in sorted(root.iterdir()):
        if entry.name.startswith("build") or entry.name == "external":
            continue
        if entry.name in submodules:
            (dest / entry.name).mkdir()
        else:
            (dest / entry.name).symlink_to(entry)
    external = dest / "external"
    external.mkdir()
    for entry in sorted((root / "external").iterdir()):
        if entry.name in SDK_DIRS.values() and entry.name not in keep_sdks:
            continue
        (external / entry.name).symlink_to(entry.resolve())
    return dest


def scratch_source_dir(root: Path) -> tempfile.TemporaryDirectory:
    """Where an overlay lives. CMake refuses a checkout under the system temp dir, so it sits in
    the worktree under a ``build`` name: git-ignored, skipped by the overlay itself and by the
    build-dir chooser (it holds no CMakeCache), and removed when the profile is done."""
    return tempfile.TemporaryDirectory(prefix="build-census-src.", dir=root)


def inputs_digest(root: Path) -> str:
    """A digest of everything a profile's measurement depends on that is checked in."""
    out = subprocess.run(["git", "-C", str(root), "ls-files", "-z"], capture_output=True, check=True).stdout
    digest = hashlib.sha256()
    for raw in sorted(out.split(b"\0")):
        name = raw.decode("utf-8", "replace")
        if name.endswith((".h", ".hpp")):
            digest.update(raw + b"\n")
        elif name.endswith(("CMakeLists.txt", ".cmake")):
            try:
                digest.update(raw + b"\0" + (root / name).read_bytes())
            except OSError:
                continue
    for extra in (Path("tools/scripts/consumption_census.py"), Path("tools/scripts/census_profiles.py")):
        digest.update((root / extra).read_bytes())
    digest.update(host_prefix().encode())
    return digest.hexdigest()


def profile_digest(common: str, document: dict, key: str) -> str:
    """One profile's cache key: the shared inputs, the committed header lists, and its own block, so
    editing one profile's block (or regenerating it) invalidates only that profile's cached result."""
    digest = hashlib.sha256(common.encode())
    digest.update(json.dumps(document.get("public_headers_by_root", {}), sort_keys=True).encode())
    digest.update(json.dumps(document["profiles"][key], sort_keys=True).encode())
    return digest.hexdigest()


def cache_path(root: Path) -> Path:
    proc = subprocess.run(["git", "-C", str(root), "rev-parse", "--path-format=absolute", "--git-common-dir"],
                          capture_output=True, text=True, encoding="utf-8", check=False)
    common = Path(proc.stdout.strip()) if proc.returncode == 0 and proc.stdout.strip() else root / ".git"
    return common / CACHE_NAME


def load_cache(root: Path) -> dict:
    try:
        return json.loads(cache_path(root).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def store_cache(root: Path, entries: dict) -> None:
    try:
        cache_path(root).write_text(json.dumps(entries, sort_keys=True), encoding="utf-8")
    except OSError:
        pass  # a cache that cannot be written only costs the next run a configure


def census_cmd(root: Path, build: Path, mode: str, source: Path) -> list[str]:
    """The generator reads include roots relative to the tree CMake was pointed at, so it is given
    that tree (the overlay, for an SDK-less profile) and the real census file to read or write."""
    return [sys.executable, str(root / "tools/scripts/consumption_census.py"), "--build-dir", str(build),
            "--source-root", str(source), "--census", str(root / census.CENSUS_RELPATH),
            "--schema", str(root / census.SCHEMA_RELPATH), mode]


def run_census(root: Path, build: Path, mode: str, source: Path) -> tuple[int, str]:
    proc = subprocess.run(census_cmd(root, build, mode, source), capture_output=True, text=True,
                          encoding="utf-8", errors="replace", check=False)
    return proc.returncode, (proc.stdout + proc.stderr).strip()


def configure_profile(root: Path, spec: Spec, build: Path, scratch: Path,
                      force_overlay: bool = False) -> tuple[str, Path]:
    """Configure ``build`` for ``spec``: ("", source tree used) on success, else (why this host cannot, root)."""
    missing = spec.sdks() - present_sdks(root)
    if missing:
        return (f"needs {', '.join(sorted(missing))} under external/; run ./setup.sh --deps-only "
                "--non-interactive"), root
    source = root if spec.sdks() == present_sdks(root) and not force_overlay else make_overlay(
        root, scratch, spec.sdks())
    cmd = configure_only.governor_prefix(root) + [
        os.environ.get("CMAKE", "cmake"), "-S", str(source), "-B", str(build),
        *configure_only.GATE_CONFIGURE_ARGS, *spec.cmake_args()]
    why = configure_only._run(cmd, root, {**os.environ, **configure_only.GOVERNED_ENV})  # noqa: SLF001
    if why:
        return f"configure failed: {why}", source
    compiled = configure_only.compile_evidence(build)
    if compiled:
        return f"the configure compiled {', '.join(compiled[:3])}", source
    try:
        configured = census.profile_key(census.load_facts(build))
    except census.CensusError as error:
        return str(error), source
    if configured != spec.key:
        return f"this host configures that feature set as {configured}", source
    return "", source


def process(root: Path, spec: Spec, mode: str, force_overlay: bool = False) -> tuple[str, str]:
    """(status, detail): status is ok, drift, not-configurable or error."""
    with configure_only.temporary_build(root) as build, scratch_source_dir(root) as scratch:
        why, source = configure_profile(root, spec, build, Path(scratch), force_overlay)
        if why:
            return "not-configurable", why
        rc, text = run_census(root, build, mode, source)
        if rc == 0:
            return "ok", text.splitlines()[-1] if text else ""
        return ("drift" if mode == "--check" and rc == 1 else "error"), text


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--repo-root", type=Path, default=HERE.parents[1])
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", action="store_true")
    mode.add_argument("--check", action="store_true")
    ap.add_argument("--cached", action="store_true", help="skip a profile already verified for these inputs")
    ap.add_argument("--force-overlay", action="store_true",
                    help="configure every profile from the symlink view, even one the checkout can build "
                         "directly: a control that the view measures what the checkout does")
    ap.add_argument("--skip", action="append", default=[], metavar="KEY",
                    help="a profile another check already verified from a build of this tree")
    args = ap.parse_args(argv[1:])
    root = args.repo_root.resolve()
    flag = "--write" if args.write else "--check"

    try:
        document = read_census(root)
        specs, other = committed_specs(root, document)
        common = inputs_digest(root)
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        say(f"census_profiles: cannot read the committed census: {error}")
        return EXIT_ERROR
    cache = load_cache(root)
    results: dict[str, tuple[str, str]] = {}
    for spec in specs:
        if spec.key in args.skip:
            results[spec.key] = ("ok", "verified by the caller's build")
        elif args.check and args.cached and cache.get(spec.key) == profile_digest(common, document, spec.key):
            results[spec.key] = ("ok", "verified for these inputs (cached)")
        else:
            say(f"census_profiles: {flag[2:]} {spec.key}")
            results[spec.key] = process(root, spec, flag, args.force_overlay)
    if args.check:
        for key, (status, _) in results.items():
            if status == "ok":
                cache[key] = profile_digest(common, document, key)
            else:
                cache.pop(key, None)
        store_cache(root, cache)
    else:
        # A write changes the committed census, so every remembered result is for dead inputs.
        store_cache(root, {})

    drift = [k for k, (s, _) in results.items() if s == "drift"]
    errors = [k for k, (s, _) in results.items() if s == "error"]
    skipped = [k for k, (s, _) in results.items() if s == "not-configurable"]
    for key, (status, detail) in results.items():
        say(f"  {status:16} {key}: {detail.splitlines()[-1] if detail else ''}")
        if status in ("drift", "error"):
            for line in detail.splitlines()[:14]:
                say(f"      {line}")
    for key in other:
        say(f"  {'other platform':16} {key}: carried through untouched")
    if drift or errors:
        if drift:
            say("census_profiles: these profiles no longer match the build graph. Regenerate every profile "
                "in one command and commit the file:")
            say("    python3 tools/scripts/census_profiles.py --write")
            say("    git add docs/status/consumption-profiles.json")
        return EXIT_DRIFT
    if skipped:
        say(f"census_profiles: NOT CHECKED here: {', '.join(skipped)}")
        return EXIT_NOT_CONFIGURABLE
    say(f"census_profiles: {len(results)} profile(s) {'written' if args.write else 'current'}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

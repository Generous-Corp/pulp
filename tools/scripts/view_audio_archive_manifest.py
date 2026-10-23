#!/usr/bin/env python3
"""Describe and verify a published pulp-view-audio archive.

A downstream package that carries a view unit proves its inertness against the
symbols pulp-view-audio defines, so it needs the archive itself, built from an
exact Pulp commit. This tool writes the manifest that travels with that archive
and checks an archive against a manifest:

    view_audio_archive_manifest.py describe --archive libpulp-view-audio.a
        --source-sha <40-hex> --source-root <checkout> --floor-out floor.txt
        --gate-out gate.txt [--check NAME=ARCHIVE ...] --output manifest.json

    view_audio_archive_manifest.py verify --archive libpulp-view-audio.a
        --manifest manifest.json [--floor floor.txt] [--source-sha <40-hex>]

The symbol floor is the demangled list of symbols the archive defines, read
from `nm -A -P -g` with the same rule view_audio_symbol_floor.py applies: every
listed type except U, w and v is a definition. The gate output is that script's
own verdict, run from the source checkout against the named libraries.

Exit codes: 0 = ok, 1 = verification mismatch, 2 = unusable input.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import platform
import re
import shutil
import subprocess
import sys

SCHEMA = "pulp.view-audio-archive.v1"
SHA_RE = re.compile(r"^[0-9a-f]{40}$")
LINE_RE = re.compile(r"^(?P<archive>.*?)\[(?P<object>[^\]]+)\]: (?P<symbol>\S+) (?P<type>\S)")


class InputError(RuntimeError):
    pass


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def run(command: list[str], **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(command, capture_output=True, text=True, encoding="utf-8",
                          errors="replace", check=False, **kwargs)


def defined_symbols(listing: str) -> list[str]:
    """Sorted symbols an `nm -A -P -g` listing defines."""
    defined = set()
    for line in listing.splitlines():
        match = LINE_RE.match(line)
        if match and match["type"] not in ("U", "w", "v"):
            defined.add(match["symbol"])
    return sorted(defined)


def demangle(symbols: list[str], cxxfilt: str | None) -> list[str]:
    if not symbols:
        return symbols
    if not cxxfilt:
        raise InputError("c++filt (or llvm-cxxfilt) is required to demangle the floor")
    proc = run([cxxfilt], input="\n".join(symbols))
    out = proc.stdout.splitlines()
    if proc.returncode != 0 or len(out) != len(symbols):
        raise InputError(f"{cxxfilt} did not demangle {len(symbols)} symbols one-to-one")
    return out


def symbol_floor(archive: pathlib.Path, nm: str, cxxfilt: str | None) -> str:
    proc = run([nm, "-A", "-P", "-g", str(archive)])
    if proc.returncode != 0:
        raise InputError(f"{nm} failed on {archive}: {proc.stderr.strip()}")
    symbols = defined_symbols(proc.stdout)
    if not symbols:
        # An empty floor would let every downstream inertness check pass vacuously.
        raise InputError(f"{archive} defines no symbol")
    return "".join(f"{name}\n" for name in sorted(demangle(symbols, cxxfilt)))


def tool_version(command: list[str]) -> str | None:
    if not shutil.which(command[0]):
        return None
    proc = run(command)
    text = (proc.stdout or proc.stderr).strip()
    return text if proc.returncode == 0 and text else None


def toolchain(cache: pathlib.Path | None) -> dict:
    """Compiler, archiver, SDK and CMake identity of the building host."""
    cached: dict[str, str] = {}
    if cache and cache.is_file():
        wanted = ("CMAKE_BUILD_TYPE", "CMAKE_CXX_COMPILER", "CMAKE_AR", "CMAKE_RANLIB",
                  "CMAKE_OSX_ARCHITECTURES", "CMAKE_OSX_DEPLOYMENT_TARGET",
                  "CMAKE_OSX_SYSROOT", "CMAKE_GENERATOR")
        for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
            key, sep, value = line.partition("=")
            name = key.split(":", 1)[0]
            if sep and name in wanted:
                cached[name] = value
    compiler = cached.get("CMAKE_CXX_COMPILER") or "c++"
    return {
        "host": {"system": platform.system(), "release": platform.release(),
                 "machine": platform.machine()},
        "cmake_cache": cached,
        "compiler": tool_version([compiler, "--version"]),
        "xcode": tool_version(["xcodebuild", "-version"]),
        "sdk_version": tool_version(["xcrun", "--show-sdk-version"]),
        "sdk_build": tool_version(["xcrun", "--show-sdk-build-version"]),
        "cmake": tool_version(["cmake", "--version"]),
        "ninja": tool_version(["ninja", "--version"]),
        "zero_ar_date": os.environ.get("ZERO_AR_DATE"),
    }


def run_gate(source_root: pathlib.Path, nm: str, archive: pathlib.Path,
             checks: list[str]) -> tuple[int, str]:
    # The gate runs from the source checkout, so every path it is handed must
    # survive the change of working directory.
    source_root = source_root.resolve()
    script = source_root / "tools/scripts/view_audio_symbol_floor.py"
    if not script.is_file():
        raise InputError(f"{script} is missing from the source checkout")
    if os.sep in nm:
        nm = str(pathlib.Path(nm).resolve())
    command = [sys.executable, str(script), "--nm", nm, "--audio", str(archive.resolve())]
    for item in checks:
        name, sep, path = item.partition("=")
        if not sep or not name or not path:
            raise InputError(f"--check {item!r} is not NAME=ARCHIVE")
        command.extend(["--check", f"{name}={pathlib.Path(path).resolve()}"])
    proc = run(command, cwd=source_root)
    return proc.returncode, proc.stdout + proc.stderr


def git_head(root: pathlib.Path) -> str:
    proc = run(["git", "-C", str(root), "rev-parse", "HEAD"])
    if proc.returncode != 0:
        raise InputError(f"cannot read HEAD of {root}: {proc.stderr.strip()}")
    return proc.stdout.strip()


def describe(args: argparse.Namespace) -> int:
    if not SHA_RE.fullmatch(args.source_sha):
        raise InputError(f"--source-sha must be a full 40-hex commit, got {args.source_sha!r}")
    head = git_head(args.source_root)
    if head != args.source_sha:
        raise InputError(f"source checkout is at {head}, not {args.source_sha}")
    if not args.archive.is_file():
        raise InputError(f"archive {args.archive} does not exist")
    nm = args.nm or shutil.which("nm")
    if not nm:
        raise InputError("nm is required")
    cxxfilt = args.cxxfilt or shutil.which("c++filt") or shutil.which("llvm-cxxfilt")

    floor_text = symbol_floor(args.archive, nm, cxxfilt)
    args.floor_out.write_text(floor_text, encoding="utf-8")
    gate_status, gate_text = run_gate(args.source_root, nm, args.archive, args.check)
    if gate_status == 2:
        raise InputError(f"view_audio_symbol_floor.py could not run:\n{gate_text}")
    args.gate_out.write_text(gate_text, encoding="utf-8")

    env = os.environ
    manifest = {
        "schema": SCHEMA,
        "source": {"repository": env.get("GITHUB_REPOSITORY", "Generous-Corp/pulp"),
                   "sha": args.source_sha},
        "target": "pulp-view-audio",
        "configure": args.configure,
        "archive": {"name": args.archive.name, "sha256": sha256_file(args.archive),
                    "size": args.archive.stat().st_size},
        "symbol_floor": {"name": args.floor_out.name, "rule": "nm -A -P -g; types other than U, w, v; demangled",
                         "sha256": sha256_file(args.floor_out),
                         "symbols": floor_text.count("\n")},
        "symbol_floor_gate": {"name": args.gate_out.name, "script": "tools/scripts/view_audio_symbol_floor.py",
                              "checks": [item.split("=", 1)[0] for item in args.check],
                              "exit_code": gate_status, "sha256": sha256_file(args.gate_out)},
        "rebuild": None,
        "toolchain": toolchain(args.cmake_cache),
        "workflow": {"run_id": env.get("GITHUB_RUN_ID"), "run_attempt": env.get("GITHUB_RUN_ATTEMPT"),
                     "workflow_ref": env.get("GITHUB_WORKFLOW_REF"), "workflow_sha": env.get("GITHUB_SHA"),
                     "server_url": env.get("GITHUB_SERVER_URL")},
    }
    if args.rebuild_archive:
        rebuilt = sha256_file(args.rebuild_archive)
        manifest["rebuild"] = {"sha256": rebuilt, "identical": rebuilt == manifest["archive"]["sha256"]}
    args.output.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"view-audio-archive: {args.archive.name} sha256={manifest['archive']['sha256']} "
          f"floor={manifest['symbol_floor']['symbols']} symbols gate_exit={gate_status}")
    return 0


def verify(args: argparse.Namespace) -> int:
    try:
        manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise InputError(f"cannot read manifest {args.manifest}: {exc}") from exc
    if manifest.get("schema") != SCHEMA:
        raise InputError(f"manifest schema is {manifest.get('schema')!r}, not {SCHEMA}")
    if not args.archive.is_file():
        raise InputError(f"archive {args.archive} does not exist")
    errors = []
    recorded_sha = manifest.get("source", {}).get("sha")
    if not isinstance(recorded_sha, str) or not SHA_RE.fullmatch(recorded_sha):
        errors.append(f"manifest source sha {recorded_sha!r} is not a full commit")
    if args.source_sha and recorded_sha != args.source_sha:
        errors.append(f"manifest source sha {recorded_sha} != expected {args.source_sha}")
    archive = manifest.get("archive", {})
    actual = sha256_file(args.archive)
    if archive.get("sha256") != actual:
        errors.append(f"archive sha256 {actual} != manifest {archive.get('sha256')}")
    if archive.get("size") != args.archive.stat().st_size:
        errors.append(f"archive size {args.archive.stat().st_size} != manifest {archive.get('size')}")
    if args.floor:
        floor_sha = sha256_file(args.floor)
        if manifest.get("symbol_floor", {}).get("sha256") != floor_sha:
            errors.append(f"symbol floor sha256 {floor_sha} != manifest")
    for error in errors:
        print(f"view-audio-archive: {error}", file=sys.stderr)
    if not errors:
        print(f"view-audio-archive: {args.archive.name} matches {args.manifest.name} "
              f"for {recorded_sha}")
    return 1 if errors else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)

    desc = sub.add_parser("describe", help="write the manifest for a freshly built archive")
    desc.add_argument("--archive", type=pathlib.Path, required=True)
    desc.add_argument("--source-sha", required=True)
    desc.add_argument("--source-root", type=pathlib.Path, required=True)
    desc.add_argument("--floor-out", type=pathlib.Path, required=True)
    desc.add_argument("--gate-out", type=pathlib.Path, required=True)
    desc.add_argument("--output", type=pathlib.Path, required=True)
    desc.add_argument("--check", action="append", default=[], metavar="NAME=ARCHIVE")
    desc.add_argument("--configure", action="append", default=[], metavar="-DNAME=VALUE",
                      help="a configure argument the archive was built with (pass as --configure=-DX=Y)")
    desc.add_argument("--cmake-cache", type=pathlib.Path)
    desc.add_argument("--rebuild-archive", type=pathlib.Path,
                      help="a second build of the same source, compared byte for byte")
    desc.add_argument("--nm")
    desc.add_argument("--cxxfilt")
    desc.set_defaults(func=describe)

    ver = sub.add_parser("verify", help="check an archive against its manifest")
    ver.add_argument("--archive", type=pathlib.Path, required=True)
    ver.add_argument("--manifest", type=pathlib.Path, required=True)
    ver.add_argument("--floor", type=pathlib.Path)
    ver.add_argument("--source-sha")
    ver.set_defaults(func=verify)

    args = parser.parse_args(argv)
    if args.command == "describe" and not args.check:
        parser.error("describe needs at least one --check library for the floor gate")
    try:
        return args.func(args)
    except InputError as exc:
        print(f"view-audio-archive: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())

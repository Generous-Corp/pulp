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
        --manifest manifest.json --floor floor.txt --gate gate.txt
        --source-sha <40-hex> [--require-gate-pass] [--require-identical-rebuild]

    view_audio_archive_manifest.py verify-release --dir <downloaded assets>
        --source-sha <40-hex> --repository Generous-Corp/pulp [--gh gh]

    view_audio_archive_manifest.py check-draft --releases releases.json
        --out <asset dir> --source-sha <40-hex>
    view_audio_archive_manifest.py check-release --releases releases.json
        --tag-ref tag.tsv --out <asset dir> --source-sha <40-hex>

verify-release is the consumer check, in one command: the six release assets
are exactly present, SHA256SUMS holds, `gh attestation verify` accepts all five
attested files from the bundle alone (no token, the signer workflow on
refs/heads/main at the source commit, a GitHub-hosted runner), and `verify`
passes with the floor, the gate output, a passing gate and an identical
rebuild.

check-draft and check-release read what the GitHub API reports for the
release tag (every release using it, and the tag ref) and hold it to the
assets in --out: a draft may be reused only if it targets the source commit
and holds exactly these bytes plus a Sigstore bundle; a published release must
be immutable, a prerelease, at a commit tag on the source commit, with exactly
these assets.

The symbol floor is the demangled list of symbols the archive provides, read
from `nm -A -P -g` through nm_symbols: strong definitions and weak ones (W/V),
because a class's typeinfo or vtable is often weak and is still the archive's.
That is deliberately wider than the dependency gate, which counts only strong
definitions. The gate output is view_audio_symbol_floor.py's own verdict, run
from the source checkout against the named libraries.

The attestation binds the files to the workflow run; the manifest records the
source commit, the gate verdict and the rebuild comparison. A consumer checks
both, which is what verify-release does.

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
import tempfile

SCHEMA = "pulp.view-audio-archive.v1"
SHA_RE = re.compile(r"^[0-9a-f]{40}$")
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import nm_symbols  # noqa: E402

ATTESTED = ("libpulp-view-audio.a", "pulp-view-audio-archive.manifest.json",
            "pulp-view-audio-symbol-floor.txt", "pulp-view-audio-symbol-floor-gate.txt", "SHA256SUMS")
SIGNER_WORKFLOW = ".github/workflows/view-audio-archive.yml"


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
    """Sorted symbols an `nm -A -P -g` listing provides, weak definitions included."""
    return sorted(nm_symbols.defined(listing, include_weak=True))


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


def describe_rebuild(build_dir: pathlib.Path | None, rebuild_dir: pathlib.Path | None,
                     rebuilt: pathlib.Path, archive_sha: str) -> dict:
    """What the second build was, derived from where it ran rather than declared."""
    if build_dir is None or rebuild_dir is None:
        raise InputError("--rebuild-archive needs --build-dir and --rebuild-dir")
    first, second = build_dir.resolve(), rebuild_dir.resolve()
    for directory in (first, second):
        if not (directory / "CMakeCache.txt").is_file():
            raise InputError(f"{directory} is not a configured build directory")
    if second not in rebuilt.resolve().parents:
        raise InputError(f"{rebuilt} was not produced in {second}")
    sha = sha256_file(rebuilt)
    return {"kind": "same-build-dir" if first == second else "separate-build-dir",
            "build_dir": first.name, "rebuild_dir": second.name,
            "sha256": sha, "identical": sha == archive_sha}


BUNDLE = "pulp-view-audio-archive.sigstore.json"


def asset_digests(out: pathlib.Path) -> dict[str, str]:
    return {path.name: "sha256:" + sha256_file(path) for path in sorted(out.iterdir()) if path.is_file()}


def releases_for_tag(path: pathlib.Path) -> list[dict]:
    try:
        rows = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise InputError(f"cannot read {path}: {exc}") from exc
    if not isinstance(rows, list):
        raise InputError(f"{path} is not a list of releases")
    return rows


def published_assets(release: dict) -> dict[str, str | None]:
    return {asset["name"]: asset.get("digest") for asset in release.get("assets", [])}


def draft_problems(rows: list[dict], ours: dict[str, str], source_sha: str) -> tuple[list[str], bool]:
    """(refusals, whether a reusable draft exists) for the releases using the tag."""
    if not rows:
        return [], False
    if len(rows) > 1:
        return [f"{len(rows)} releases already use the tag"], False
    row = rows[0]
    if not row.get("draft"):
        return ["the release is already published"], False
    problems = []
    if row.get("target_commitish") != source_sha:
        problems.append(f"draft targets {row.get('target_commitish')!r}, not {source_sha}")
    # Nothing is ever uploaded onto a draft: it is reused only when it already
    # holds exactly these bytes plus a bundle, which every attestation mints anew.
    theirs = published_assets(row)
    if set(theirs) != set(ours) | {BUNDLE}:
        problems.append(f"draft assets {sorted(theirs)} are not {sorted(set(ours) | {BUNDLE})}")
    changed = sorted(name for name, digest in ours.items() if name in theirs and theirs[name] != digest)
    if changed:
        problems.append(f"draft holds other bytes for {changed}")
    return problems, not problems


def release_problems(rows: list[dict], tag_ref: str, ours: dict[str, str], source_sha: str) -> list[str]:
    problems = []
    if tag_ref.split() != ["commit", source_sha]:
        problems.append(f"tag resolves to {tag_ref.strip()!r}, not commit {source_sha}")
    if len(rows) != 1:
        return problems + [f"{len(rows)} releases use the tag"]
    release = rows[0]
    for key, want in (("immutable", True), ("draft", False), ("prerelease", True)):
        if release.get(key) is not want:
            problems.append(f"release {key} is {release.get(key)!r}")
    if published_assets(release) != ours:
        problems.append(f"published assets {sorted(published_assets(release).items())} "
                        f"!= {sorted(ours.items())}")
    return problems


def check_draft(args: argparse.Namespace) -> int:
    problems, reusable = draft_problems(releases_for_tag(args.releases), asset_digests(args.out),
                                        args.source_sha)
    for problem in problems:
        print(f"view-audio-archive: {problem}", file=sys.stderr)
    if not problems:
        print(f"draft={'true' if reusable else 'false'}")
    return 1 if problems else 0


def check_release(args: argparse.Namespace) -> int:
    try:
        tag_ref = args.tag_ref.read_text(encoding="utf-8")
    except OSError as exc:
        raise InputError(f"cannot read {args.tag_ref}: {exc}") from exc
    problems = release_problems(releases_for_tag(args.releases), tag_ref, asset_digests(args.out),
                                args.source_sha)
    for problem in problems:
        print(f"view-audio-archive: {problem}", file=sys.stderr)
    if not problems:
        print(f"view-audio-archive: release is immutable at {args.source_sha} with its exact assets")
    return 1 if problems else 0


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
        "symbol_floor": {"name": args.floor_out.name, "rule": "nm -A -P -g; strong (TDBRSCG) and weak (WV) definitions; demangled",
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
        manifest["rebuild"] = describe_rebuild(args.build_dir, args.rebuild_dir,
                                               args.rebuild_archive, manifest["archive"]["sha256"])
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
    if recorded_sha != args.source_sha:
        errors.append(f"manifest source sha {recorded_sha} != expected {args.source_sha}")
    archive = manifest.get("archive", {})
    actual = sha256_file(args.archive)
    if archive.get("sha256") != actual:
        errors.append(f"archive sha256 {actual} != manifest {archive.get('sha256')}")
    if archive.get("size") != args.archive.stat().st_size:
        errors.append(f"archive size {args.archive.stat().st_size} != manifest {archive.get('size')}")
    for path in (args.floor, args.gate):
        if not path.is_file():
            raise InputError(f"{path} does not exist")
    floor = manifest.get("symbol_floor", {})
    floor_sha = sha256_file(args.floor)
    if floor.get("sha256") != floor_sha:
        errors.append(f"symbol floor sha256 {floor_sha} != manifest")
    floor_lines = len(args.floor.read_text(encoding="utf-8").splitlines())
    if floor_lines == 0 or floor.get("symbols") != floor_lines:
        errors.append(f"symbol floor has {floor_lines} symbols, manifest records {floor.get('symbols')!r}")
    gate = manifest.get("symbol_floor_gate", {})
    gate_sha = sha256_file(args.gate)
    if gate.get("sha256") != gate_sha:
        errors.append(f"symbol floor gate sha256 {gate_sha} != manifest")
    if args.require_gate_pass and gate.get("exit_code") != 0:
        errors.append(f"symbol floor gate exit code is {gate.get('exit_code')!r}, not 0")
    if args.require_identical_rebuild:
        rebuild = manifest.get("rebuild") or {}
        if rebuild.get("kind") != "separate-build-dir" or rebuild.get("identical") is not True:
            errors.append(f"no identical rebuild in a separate build directory was recorded: {rebuild or None}")
    for error in errors:
        print(f"view-audio-archive: {error}", file=sys.stderr)
    if not errors:
        print(f"view-audio-archive: {args.archive.name} matches {args.manifest.name} "
              f"for {recorded_sha}")
    return 1 if errors else 0


def verify_release(args: argparse.Namespace) -> int:
    """The consumer check: exact assets, checksums, attestation, manifest."""
    directory = args.dir.resolve()
    if not SHA_RE.fullmatch(args.source_sha):
        raise InputError(f"--source-sha must be a full 40-hex commit, got {args.source_sha!r}")
    present = {path.name for path in directory.iterdir() if path.is_file()} if directory.is_dir() else set()
    expected = set(ATTESTED) | {BUNDLE}
    if present != expected:
        print(f"view-audio-archive: {directory} holds {sorted(present)}, not {sorted(expected)}",
              file=sys.stderr)
        return 1
    errors = []
    sums = {}
    for line in (directory / "SHA256SUMS").read_text(encoding="utf-8").splitlines():
        digest, _, name = line.partition("  ")
        sums[name] = digest
    for name in ATTESTED[:-1]:
        if sums.get(name) != sha256_file(directory / name):
            errors.append(f"SHA256SUMS does not hold for {name}")
    if set(sums) != set(ATTESTED[:-1]):
        errors.append(f"SHA256SUMS lists {sorted(sums)}")
    # Token-free on purpose: the bundle alone must be enough for a consumer.
    env = {key: value for key, value in os.environ.items() if key not in ("GH_TOKEN", "GITHUB_TOKEN")}
    with tempfile.TemporaryDirectory() as config:
        env["GH_CONFIG_DIR"] = config
        for name in ATTESTED:
            proc = run([args.gh, "attestation", "verify", str(directory / name),
                        "--bundle", str(directory / BUNDLE),
                        "--repo", args.repository,
                        "--signer-workflow", f"{args.repository}/{SIGNER_WORKFLOW}",
                        "--source-ref", "refs/heads/main",
                        "--source-digest", args.source_sha,
                        "--deny-self-hosted-runners"], env=env)
            if proc.returncode != 0:
                errors.append(f"attestation rejected {name}: {(proc.stderr or proc.stdout).strip()}")
    for error in errors:
        print(f"view-audio-archive: {error}", file=sys.stderr)
    if errors:
        return 1
    return verify(argparse.Namespace(
        archive=directory / ATTESTED[0], manifest=directory / ATTESTED[1],
        floor=directory / ATTESTED[2], gate=directory / ATTESTED[3], source_sha=args.source_sha,
        require_gate_pass=True, require_identical_rebuild=True))


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
    desc.add_argument("--build-dir", type=pathlib.Path,
                      help="the build directory the archive came from")
    desc.add_argument("--rebuild-dir", type=pathlib.Path,
                      help="the build directory --rebuild-archive came from")
    desc.add_argument("--nm")
    desc.add_argument("--cxxfilt")
    desc.set_defaults(func=describe)

    ver = sub.add_parser("verify", help="check an archive against its manifest")
    ver.add_argument("--archive", type=pathlib.Path, required=True)
    ver.add_argument("--manifest", type=pathlib.Path, required=True)
    ver.add_argument("--floor", type=pathlib.Path, required=True)
    ver.add_argument("--source-sha", required=True)
    ver.add_argument("--gate", type=pathlib.Path, required=True, help="the floor gate output")
    ver.add_argument("--require-gate-pass", action="store_true",
                     help="fail unless the recorded floor gate exited 0")
    ver.add_argument("--require-identical-rebuild", action="store_true",
                     help="fail unless a separate-build-dir rebuild was recorded byte-identical")
    ver.set_defaults(func=verify)

    rel = sub.add_parser("verify-release", help="the consumer check of a downloaded release")
    rel.add_argument("--dir", type=pathlib.Path, required=True, help="the downloaded release assets")
    rel.add_argument("--source-sha", required=True)
    rel.add_argument("--repository", required=True)
    rel.add_argument("--gh", default=shutil.which("gh") or "gh")
    rel.set_defaults(func=verify_release)

    for name, func, help_text in (("check-draft", check_draft, "refuse a draft that is not exactly ours"),
                                  ("check-release", check_release, "check the published release")):
        chk = sub.add_parser(name, help=help_text)
        chk.add_argument("--releases", type=pathlib.Path, required=True,
                         help="JSON list of every release using the tag")
        chk.add_argument("--out", type=pathlib.Path, required=True, help="the assets this run publishes")
        chk.add_argument("--source-sha", required=True)
        if name == "check-release":
            chk.add_argument("--tag-ref", type=pathlib.Path, required=True,
                             help="'<object type> <sha>' of the tag ref")
        chk.set_defaults(func=func)

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

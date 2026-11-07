#!/usr/bin/env python3
"""Check that external/lilv is still the unmodified upstream release.

external/lilv vendors lilv, serd, sord, sratom, and zix from the author's
release tarballs (see external/lilv/README.md). external/lilv/vendored.json
records, for each library, the version, the tarball's SHA-256, the upstream
files deliberately left out, and a digest of the vendored tree (every file
under include/ and src/, plus COPYING).

  verify_vendored_lilv.py --check            offline: the tree matches vendored.json
  verify_vendored_lilv.py --verify-upstream  download each tarball, check its
                                             SHA-256, and check that the tree it
                                             holds (minus the recorded omissions)
                                             has the recorded digest
  verify_vendored_lilv.py --write            record the current tree's digests

--check runs in CI. --verify-upstream needs the network; run it when bumping a
library, before --write.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
VENDOR_DIR = ROOT / "external/lilv"
RECORD = VENDOR_DIR / "vendored.json"
KEPT = ("include", "src", "COPYING")


def tree_files(root: Path) -> list[Path]:
    files: list[Path] = []
    for kept in KEPT:
        path = root / kept
        if path.is_file():
            files.append(path)
        elif path.is_dir():
            files.extend(p for p in path.rglob("*") if p.is_file())
    return sorted(files, key=lambda p: p.relative_to(root).as_posix())


def tree_digest(root: Path, omitted: list[str] = ()) -> str:
    """SHA-256 over each kept file's relative path and contents, in path order."""
    h = hashlib.sha256()
    for path in tree_files(root):
        rel = path.relative_to(root).as_posix()
        if rel in omitted:
            continue
        h.update(rel.encode() + b"\0")
        h.update(hashlib.sha256(path.read_bytes()).hexdigest().encode() + b"\n")
    return h.hexdigest()


def load_record() -> dict:
    return json.loads(RECORD.read_text(encoding="utf-8"))


def check(vendor_dir: Path = VENDOR_DIR, record: dict | None = None) -> list[str]:
    record = record or load_record()
    problems = []
    for name, entry in sorted(record["libraries"].items()):
        lib = vendor_dir / name
        if not lib.is_dir():
            problems.append(f"{name}: external/lilv/{name} is missing")
            continue
        for omitted in entry.get("omitted", []):
            if (lib / omitted).exists():
                problems.append(f"{name}: {omitted} was omitted upstream-side but is present")
        got = tree_digest(lib)
        if got != entry["tree_sha256"]:
            problems.append(
                f"{name}: tree digest {got} does not match the recorded {entry['tree_sha256']}; "
                "the vendored sources were changed")
    on_disk = {p.name for p in vendor_dir.iterdir() if p.is_dir()}
    for extra in sorted(on_disk - set(record["libraries"])):
        problems.append(f"external/lilv/{extra} is not recorded in vendored.json")
    return problems


def verify_upstream(record: dict) -> list[str]:
    problems = []
    with tempfile.TemporaryDirectory() as tmp:
        for name, entry in sorted(record["libraries"].items()):
            base = f"{name}-{entry['version']}"
            url = f"{record['download_base']}/{base}.tar.xz"
            archive = Path(tmp) / f"{base}.tar.xz"
            with urllib.request.urlopen(url, timeout=60) as response:
                archive.write_bytes(response.read())
            sha = hashlib.sha256(archive.read_bytes()).hexdigest()
            if sha != entry["tarball_sha256"]:
                problems.append(f"{name}: {url} has SHA-256 {sha}, recorded {entry['tarball_sha256']}")
                continue
            out = Path(tmp) / "x"
            with tarfile.open(archive) as tar:
                tar.extractall(out, filter="data")
            upstream = out / base
            got = tree_digest(upstream, entry.get("omitted", []))
            if got != entry["tree_sha256"]:
                problems.append(f"{name}: the release tarball's tree digest is {got}, "
                                f"recorded {entry['tree_sha256']}")
    return problems


def write(record: dict) -> None:
    for name, entry in record["libraries"].items():
        entry["tree_sha256"] = tree_digest(VENDOR_DIR / name)
    RECORD.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--verify-upstream", action="store_true")
    mode.add_argument("--write", action="store_true")
    args = parser.parse_args(argv)
    record = load_record()
    if args.write:
        write(record)
        print(f"verify-vendored-lilv: recorded {len(record['libraries'])} tree digests")
        return 0
    problems = verify_upstream(record) if args.verify_upstream else check(record=record)
    for p in problems:
        print(f"verify-vendored-lilv: {p}", file=sys.stderr)
    if problems:
        return 1
    what = "match their release tarballs" if args.verify_upstream else "match vendored.json"
    print(f"verify-vendored-lilv: {len(record['libraries'])} libraries {what}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

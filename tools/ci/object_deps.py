#!/usr/bin/env python3
"""Which headers did each object file of this build actually include?

Ninja records, as each object compiles, the exact files the compiler read
(`ninja -t deps`). The reuse replay needs that per job: with it, a changed
header reaches exactly the objects that included it in the build that ran,
instead of every object an older census graph cannot place. One document per
job holds it, header paths stored once and referenced by index:

    {"schema": "pulp-object-deps/v1",
     "headers": ["<src>/core/state/include/pulp/state/store.hpp", ...],
     "objects": {"<build>/core/state/CMakeFiles/pulp-state.dir/src/store.cpp.o": [0, 3, ...]},
     "stale": ["<build>/..."],          # objects Ninja marks STALE: deps unknown
     "members": {"<build>/core/state/libpulp-state.a": {"store.cpp.o": ["<build>/core/.../store.cpp.o"]}}}

Paths are `<src>/...` or `<build>/...`. Headers outside both trees (the SDK,
the toolchain) and under `<build>/_deps` (pinned FetchContent builds, which
move only with a CMake change) are left out. `members` maps each static
archive's member names (as link-members records them) to the objects that
produce them, so a pulled member can be joined to its headers.

A reader must call `unusable()` first. An object missing from `objects`, or
listed in `stale`, has unknown dependencies and must be treated as changed.

    object_deps.py collect --build-dir B --source-root S --out F
"""
from __future__ import annotations

import argparse
import functools
import json
import os
import re
import subprocess
import sys
from pathlib import Path

SCHEMA = "pulp-object-deps/v1"
READABLE_SCHEMAS = (SCHEMA,)
_HEAD = re.compile(r"^(?P<out>.+?): #deps \d+, deps mtime \d+ \((?P<state>VALID|STALE)\)\s*$")


class Unavailable(RuntimeError):
    """The build has no usable Ninja dependency log."""


def _roots(build_dir: Path, source_root: Path) -> list[tuple[str, str]]:
    pairs = {(os.path.normpath(str(build_dir)), "<build>"), (os.path.realpath(build_dir), "<build>"),
             (os.path.normpath(str(source_root)), "<src>"), (os.path.realpath(source_root), "<src>")}
    # Longest first, and the build root before an equal source root: the build
    # directory usually lives inside the checkout.
    return sorted(pairs, key=lambda r: (-len(r[0]), r[1] != "<build>"))


@functools.lru_cache(maxsize=None)
def _resolve(path: str) -> str:
    # Ninja records the spelling the compiler was given, which may go through a
    # symlink the roots do not (macOS reaches /tmp as /private/tmp).
    return os.path.realpath(path)


def _token(path: str, build_dir: Path, roots: list[tuple[str, str]]) -> str | None:
    full = _resolve(os.path.normpath(path if os.path.isabs(path) else os.path.join(build_dir, path)))
    for root, token in roots:
        if full == root or full.startswith(root + os.sep):
            return token + full[len(root):]
    return None


def parse_deps(text: str, build_dir: Path, source_root: Path) -> tuple[dict[str, list[str]], list[str]]:
    """object -> its in-tree dependencies, and the objects Ninja marks STALE."""
    roots = _roots(build_dir, source_root)
    objects: dict[str, list[str]] = {}
    stale: list[str] = []
    current: list[str] | None = None
    for line in text.splitlines():
        if not line.strip():
            current = None
            continue
        if not line.startswith(" "):
            m = _HEAD.match(line)
            current = None
            if not m:
                continue
            out = _token(m.group("out"), build_dir, roots) or m.group("out")
            if m.group("state") == "STALE":
                stale.append(out)
                continue
            current = objects.setdefault(out, [])
            continue
        if current is None:
            continue
        dep = _token(line.strip(), build_dir, roots)
        if dep is not None and not dep.startswith("<build>/_deps/"):
            current.append(dep)
    return objects, sorted(set(stale))


def parse_archive_inputs(text: str, build_dir: Path, source_root: Path) -> dict[str, dict[str, list[str]]]:
    """archive -> {member name -> objects}, from `ninja -t query <archives...>`.
    Only explicit inputs are members; `|` implicit and `||` order-only inputs
    are not archived."""
    roots = _roots(build_dir, source_root)
    out: dict[str, dict[str, list[str]]] = {}
    archive = None
    section = None
    for line in text.splitlines():
        if not line.startswith(" "):
            name = line.rstrip(":").strip()
            archive = (_token(name, build_dir, roots) or name) if name.endswith(".a") else None
            if archive:
                out.setdefault(archive, {})
            continue
        stripped = line.strip()
        if stripped.startswith("input:"):
            section = "input"
            continue
        if stripped.startswith("outputs:") or stripped.startswith("validations:"):
            section = None
            continue
        if archive is None or section != "input" or stripped.startswith("|"):
            continue
        obj = _token(stripped, build_dir, roots) or stripped
        out[archive].setdefault(os.path.basename(stripped), []).append(obj)
    return out


def _ninja(build_dir: Path, *args: str) -> str:
    proc = subprocess.run(["ninja", "-C", str(build_dir), "-t", *args], capture_output=True, text=True)
    if proc.returncode != 0:
        raise Unavailable(f"ninja -t {args[0]} failed: {proc.stderr.strip()[:200]}")
    return proc.stdout


def collect(build_dir: Path, source_root: Path) -> dict:
    if not (build_dir / ".ninja_deps").is_file():
        raise Unavailable("the build has no Ninja dependency log (.ninja_deps)")
    objects, stale = parse_deps(_ninja(build_dir, "deps"), build_dir, source_root)
    archives = [line.split(":", 1)[0] for line in _ninja(build_dir, "targets", "all").splitlines()
                if line.split(":", 1)[0].endswith(".a") and "STATIC_LIBRARY" in line]
    members = parse_archive_inputs(_ninja(build_dir, "query", *archives), build_dir, source_root) if archives else {}
    return compact(objects, stale, members)


def compact(objects: dict[str, list[str]], stale: list[str], members: dict) -> dict:
    headers = sorted({h for deps in objects.values() for h in deps})
    index = {h: i for i, h in enumerate(headers)}
    return {"schema": SCHEMA, "headers": headers,
            "objects": {o: sorted({index[h] for h in deps}) for o, deps in sorted(objects.items())},
            "stale": stale, "members": members}


def expand(doc: dict) -> dict[str, list[str]]:
    """object -> header paths. Raises ValueError for a schema it cannot read."""
    if doc.get("schema") not in READABLE_SCHEMAS:
        raise ValueError(f"unknown object-deps schema {doc.get('schema')!r}")
    headers = doc.get("headers") or []
    return {o: [headers[i] for i in idx] for o, idx in (doc.get("objects") or {}).items()}


def unusable(doc: dict | None) -> str | None:
    """Why a reader must not use this record at all, or None. Per-object gaps
    (an object absent from `objects`, or listed in `stale`) are the reader's
    to treat as changed; this answers only for the whole document."""
    if not isinstance(doc, dict):
        return "no record"
    if doc.get("schema") not in READABLE_SCHEMAS:
        return f"unknown schema {doc.get('schema')!r}"
    if not doc.get("objects"):
        return "no objects recorded"
    return None


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(allow_abbrev=False, description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("collect", allow_abbrev=False)
    c.add_argument("--build-dir", required=True)
    c.add_argument("--source-root", required=True)
    c.add_argument("--out", required=True)
    a = ap.parse_args(argv[1:])
    doc = collect(Path(a.build_dir).resolve(), Path(a.source_root).resolve())
    Path(a.out).write_text(json.dumps(doc, sort_keys=True, separators=(",", ":")), encoding="utf-8")
    print(f"object-deps: {len(doc['objects'])} objects, {len(doc['headers'])} headers, "
          f"{len(doc['stale'])} stale, {len(doc['members'])} archives")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

#!/usr/bin/env python3
"""Which archive members did each executable's link actually pull?

An executable depends on a static archive only through the members the
linker loaded, and ld64 / ld-prime write that set when asked (`-Wl,-map,F`):
the map's "# Object files:" section lists every loaded input, archive members
as `core/audio/libpulp-audio.a(finite_time_stretch.cpp.o)`. A reuse key over
those members, instead of whole archives, reaches only the executables a
changed member can affect.

With PULP_RECORD_LINK_MAPS=ON (tools/cmake/PulpLinkMaps.cmake) every link runs
through tools/ci/link-members-launcher.sh, which keeps the head of each
executable's map (`<build>/link-members/*.objects`) and its link arguments
(`*.args`). `collect` turns those into one document per job. Member names
are stored once per archive and referenced by index, because a thousand
executables pull largely the same members:

    {"members": {"<build>/core/state/libpulp-state.a": ["state_migration.cpp.o", "store.cpp.o", ...]},
     "executables": {"<build>/test/pulp-test-state": {
         "objects": ["<build>/test/CMakeFiles/.../test_state.cpp.o", ...],
         "archives": {"<build>/core/state/libpulp-state.a": {"members": [0, 1], "whole": false}}}}}

`expand()` turns that back into member names per executable.

An archive is `whole` when the link line loads every member regardless of use
(`-force_load <archive>`, `-all_load`, `-ObjC`): a key over it must cover all
of its members, not only those the map shows.

    link_members.py collect --build-dir B --out F
    link_members.py parse --objects O --args A --build-root B   (one link, for inspection)
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path

SCHEMA = "pulp-link-members/v1"
MEMBERS_DIR = "link-members"
_ROW = re.compile(r"^\[\s*\d+\]\s+(.*)$")
_MEMBER = re.compile(r"^(.*\.a)\((.*)\)$")


def parse_map(lines) -> tuple[str | None, str | None, list[str]]:
    """(link working directory, output path, loaded inputs) from the head of
    an ld64/ld-prime map. Stops at the end of the object list, so a full map's
    symbol table is never read."""
    cwd = path = None
    inputs: list[str] = []
    in_objects = False
    for line in lines:
        line = line.rstrip("\n")
        if line.startswith("# Cwd:"):
            cwd = line[len("# Cwd:"):].strip()
        elif line.startswith("# Path:"):
            path = line[len("# Path:"):].strip()
        elif line.startswith("# Object files:"):
            in_objects = True
        elif line.startswith("#"):
            if in_objects:
                break
        elif in_objects:
            m = _ROW.match(line)
            if m:
                inputs.append(m.group(1).strip())
    return cwd, path, inputs


def _normalise(path: str, cwd: Path, build_root: Path) -> str:
    """An input as `<build>/...` when it lives in the build tree, else its real
    path. A relative input is relative to the link's working directory."""
    p = Path(path) if os.path.isabs(path) else cwd / path
    resolved = os.path.realpath(p)
    root = os.path.realpath(build_root)
    if resolved == root or resolved.startswith(root + os.sep):
        return "<build>/" + os.path.relpath(resolved, root)
    return resolved


def members_of(inputs: list[str], cwd: Path, build_root: Path) -> dict:
    """{"objects": [...], "archives": {archive: [members]}}; SDK stubs (.tbd),
    dylibs, frameworks and the linker's synthesized input are left out."""
    objects: list[str] = []
    archives: dict[str, list[str]] = {}
    for item in inputs:
        if item == "linker synthesized" or item.endswith((".tbd", ".dylib")) or ".framework/" in item:
            continue
        m = _MEMBER.match(item)
        if m:
            archives.setdefault(_normalise(m.group(1), cwd, build_root), []).append(m.group(2))
        else:
            objects.append(_normalise(item, cwd, build_root))
    return {"objects": sorted(set(objects)),
            "archives": {a: sorted(set(ms)) for a, ms in sorted(archives.items())}}


def _expand_response_files(args: list[str], cwd: Path) -> list[str]:
    out = []
    for arg in args:
        if arg.startswith("@") and (cwd / arg[1:]).is_file():
            out.extend((cwd / arg[1:]).read_text(encoding="utf-8", errors="replace").split())
        else:
            out.append(arg)
    return out


def whole_archive_flags(args: list[str], cwd: Path, build_root: Path) -> tuple[set[str], bool]:
    """(archives loaded by `-force_load`, whether `-all_load`/`-ObjC` loads
    every archive whole) from a link command line."""
    tokens: list[str] = []
    for arg in _expand_response_files(args, cwd):
        if arg.startswith("-Wl,"):
            tokens.extend(t for t in arg[4:].split(",") if t)
        elif arg != "-Xlinker":
            tokens.append(arg)
    forced: set[str] = set()
    everything = False
    for i, tok in enumerate(tokens):
        if tok in ("-all_load", "-ObjC"):
            everything = True
        elif tok == "-force_load" and i + 1 < len(tokens):
            forced.add(_normalise(tokens[i + 1], cwd, build_root))
    return forced, everything


def parse_link(objects_text: list[str], args: list[str], build_root: Path) -> tuple[str, dict]:
    """(executable, {"objects", "archives"}) for one recorded link."""
    cwd, path, inputs = parse_map(objects_text)
    if not path or not inputs:
        raise ValueError("no output path or no object files")
    base = Path(cwd) if cwd else build_root
    forced, everything = whole_archive_flags(args, base, build_root)
    found = members_of(inputs, base, build_root)
    return _normalise(path, base, build_root), {
        "objects": found["objects"],
        "archives": {a: {"members": ms, "whole": everything or a in forced}
                     for a, ms in found["archives"].items()},
    }


def collect(build_dir: Path) -> dict:
    """Every recorded link in the build directory; a record that cannot be
    read is counted, never guessed."""
    executables: dict[str, dict] = {}
    unreadable = 0
    for f in sorted((build_dir / MEMBERS_DIR).glob("*.objects")):
        try:
            args_file = f.with_suffix(".args")
            args = args_file.read_text(encoding="utf-8").splitlines() if args_file.is_file() else []
            with f.open("r", encoding="utf-8", errors="replace") as fh:
                exe, rec = parse_link(list(fh), args, build_dir)
            executables[exe] = rec
        except (OSError, ValueError):
            unreadable += 1
    return compact({"schema": SCHEMA, "executables": executables, "unreadable": unreadable})


def compact(doc: dict) -> dict:
    """Replace member names with indices into one sorted table per archive."""
    table: dict[str, set[str]] = {}
    for rec in doc["executables"].values():
        for a, info in rec["archives"].items():
            table.setdefault(a, set()).update(info["members"])
    members = {a: sorted(ms) for a, ms in sorted(table.items())}
    index = {a: {m: i for i, m in enumerate(ms)} for a, ms in members.items()}
    executables = {
        exe: {"objects": rec["objects"],
              "archives": {a: {"members": sorted(index[a][m] for m in info["members"]), "whole": info["whole"]}
                           for a, info in rec["archives"].items()}}
        for exe, rec in doc["executables"].items()}
    return {**doc, "members": members, "executables": executables}


def expand(doc: dict) -> dict[str, dict]:
    """executable -> {"objects", "archives": {archive: {"members": [names], "whole"}}}."""
    members = doc.get("members", {})
    return {exe: {"objects": rec["objects"],
                  "archives": {a: {"members": [members[a][i] for i in info["members"]], "whole": info["whole"]}
                               for a, info in rec["archives"].items()}}
            for exe, rec in doc["executables"].items()}


def cmd_collect(a: argparse.Namespace) -> int:
    doc = collect(Path(a.build_dir).resolve())
    Path(a.out).write_text(json.dumps(doc, sort_keys=True, separators=(",", ":")), encoding="utf-8")
    print(f"link-members: {len(doc['executables'])} executables, {doc['unreadable']} unreadable")
    return 0


def cmd_parse(a: argparse.Namespace) -> int:
    args = Path(a.args).read_text(encoding="utf-8").splitlines() if a.args else []
    with open(a.objects, encoding="utf-8", errors="replace") as fh:
        exe, rec = parse_link(list(fh), args, Path(a.build_root).resolve())
    print(json.dumps({exe: rec}, indent=1, sort_keys=True))
    return 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("collect")
    c.add_argument("--build-dir", required=True)
    c.add_argument("--out", required=True)
    c.set_defaults(func=cmd_collect)
    p = sub.add_parser("parse")
    p.add_argument("--objects", required=True)
    p.add_argument("--args")
    p.add_argument("--build-root", required=True)
    p.set_defaults(func=cmd_parse)
    a = ap.parse_args(argv[1:])
    return a.func(a)


if __name__ == "__main__":
    sys.exit(main(sys.argv))

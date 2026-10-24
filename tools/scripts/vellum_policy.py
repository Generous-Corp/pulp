#!/usr/bin/env python3
"""The one table of retained Pulp subsystems the extracted units must not reach.

tools/scripts/vellum_policy.json names each subsystem once. Every boundary
check derives its own form from it here, so a subsystem added to the table is
forbidden by all of them at once:

- `names()`              the subsystem names (`gpu_audio`, `sample_bank_manifest`);
- `include_areas()`      the `pulp/<area>/` include prefixes;
- `target_stems()`       the hyphenated target stems (`pulp-<stem>`, `pulp::<stem>`);
- `roots()`              the `core/<name>/` source roots;
- `include_dirs()`       extra include directories a subsystem installs into;
- `alternation()`        a regex alternation accepting `_` or `-` between words.

`--cmake-list` prints the alternation for CMake consumers, which read the JSON
directly with string(JSON).
"""

from __future__ import annotations

import argparse
import json
import re
from functools import lru_cache
from pathlib import Path

POLICY = Path(__file__).with_name("vellum_policy.json")
SCHEMA = "pulp.vellum.policy.v1"
_NAME_RE = re.compile(r"^[a-z][a-z0-9]*(?:_[a-z0-9]+)*$")


@lru_cache(maxsize=None)
def load(path: Path = POLICY) -> tuple[dict, ...]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    if payload.get("schema") != SCHEMA:
        raise SystemExit(f"{path}: schema must be {SCHEMA}")
    entries = payload.get("forbidden_subsystems")
    if not isinstance(entries, list) or not entries:
        raise SystemExit(f"{path}: forbidden_subsystems must be a non-empty list")
    seen: set[str] = set()
    for entry in entries:
        name = entry.get("name") if isinstance(entry, dict) else None
        if not isinstance(name, str) or not _NAME_RE.match(name):
            raise SystemExit(f"{path}: invalid subsystem entry {entry!r}")
        if name in seen:
            raise SystemExit(f"{path}: duplicate subsystem {name}")
        seen.add(name)
        dirs = entry.get("include_dirs", [])
        if not isinstance(dirs, list) or not all(isinstance(d, str) and d for d in dirs):
            raise SystemExit(f"{path}: {name}: include_dirs must be a list of paths")
    return tuple(entries)


def names(path: Path = POLICY) -> tuple[str, ...]:
    return tuple(entry["name"] for entry in load(path))


def include_areas(path: Path = POLICY) -> frozenset[str]:
    return frozenset(names(path))


def target_stems(path: Path = POLICY) -> frozenset[str]:
    return frozenset(name.replace("_", "-") for name in names(path))


def roots(path: Path = POLICY) -> tuple[str, ...]:
    return tuple(f"core/{name}/" for name in names(path))


def include_dirs(path: Path = POLICY) -> tuple[str, ...]:
    return tuple(d for entry in load(path) for d in entry.get("include_dirs", []))


def alternation(path: Path = POLICY) -> str:
    """`audio|content|gpu[-_]audio|...`: each name with `_` or `-` between words."""
    return "|".join(name.replace("_", "[-_]") for name in names(path))


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cmake-list", action="store_true",
                        help="print the subsystem names as a CMake list of target stems")
    args = parser.parse_args(argv)
    if args.cmake_list:
        print(";".join(sorted(target_stems())))
    else:
        print("\n".join(names()))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

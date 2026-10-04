#!/usr/bin/env python3
"""The executable-keyed selection: which tests a run would skip, which of
those it re-runs anyway as a sample, and what it builds and runs.

Input is the key manifest executable_keys.py wrote for the head configure,
plus that configure's codemodel digest and `ctest --show-only=json-v1`
listing. The lane derives the selection after its configure stage, and
Shipyard re-derives it from the same files to check the lane's answer, so
the output is a pure function of those inputs, the seed and the percent, and
it is written as canonical JSON (sorted keys, no whitespace): equal
selections are equal bytes.

    would_skip   executables whose base and head keys are equal, that carry
                 no always_run reason, and that no executable left to run
                 spawns (a rebuilt test must never run a stale tool)
    sampled_executables
                 ceil(|would_skip| * percent / 100) of them, in integer
                 arithmetic, the ones with the smallest sha256(seed NUL
                 artifact), re-run as the negative control on a wrong skip
    tests        every registration in the listing except those of a
                 would-skip executable that was not sampled
    build_targets
                 the codemodel targets of every executable left to run, and
                 of everything those executables spawn

    executable_selection.py --manifest M --head-codemodel C --ctest-json J \\
        --seed HEX --percent P --out S

The seed is bound by Shipyard and passed in; nothing here computes it.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

SCHEMA = "pulp-executable-selection/v1"
MANIFEST_SCHEMA = "pulp-executable-keys/v1"
BUILD = "<build>/"


class SelectionError(ValueError):
    pass


def would_skip(manifest: dict) -> list[str]:
    executables = manifest.get("executables") or {}
    skip = set()
    for artifact, entry in executables.items():
        key = entry.get("head_key")
        if entry.get("kind") == "executable" and entry.get("always_run") is None \
                and key is not None and key == entry.get("base_key"):
            skip.add(artifact)
    # Whatever an executable that runs spawns has to be built too; dropping
    # it from the set can expose what it spawns in turn.
    while True:
        spawned = {s for a, e in executables.items() if a not in skip for s in e.get("spawns") or []}
        if not skip & spawned:
            return sorted(skip)
        skip -= spawned


def sample(candidates: list[str], seed: str, percent: int) -> list[str]:
    if not isinstance(percent, int) or isinstance(percent, bool) or not 1 <= percent <= 100:
        raise SelectionError(f"sample percent {percent!r} is not an integer in [1, 100]")
    count = (len(candidates) * percent + 99) // 100
    rank = sorted(candidates, key=lambda a: hashlib.sha256(f"{seed}\0{a}".encode()).hexdigest())
    return sorted(rank[:count])


def select(manifest: dict, codemodel: dict, ctest: dict, seed: str, percent: int) -> dict:
    if manifest.get("schema") != MANIFEST_SCHEMA:
        raise SelectionError(f"key manifest schema is {manifest.get('schema')!r}, not {MANIFEST_SCHEMA}")
    if not seed:
        raise SelectionError("no sample seed")
    executables = manifest.get("executables") or {}
    skippable = would_skip(manifest)
    sampled = sample(skippable, seed, percent)
    skipped = set(skippable) - set(sampled)
    skipped_tests = {name for a in skipped for name in executables[a].get("registrations") or []}
    names = [t.get("name") for t in ctest.get("tests") or []]
    if any(not isinstance(n, str) or not n for n in names):
        raise SelectionError("a ctest listing entry has no name")
    tests = sorted(set(names) - skipped_tests)
    targets = {a.removeprefix(BUILD): n for n, t in (codemodel.get("targets") or {}).items()
               for a in t.get("artifacts") or []}
    running = [a for a, e in executables.items() if e.get("kind") == "executable" and a not in skipped]
    needed = set(running) | {s for a in running for s in executables[a].get("spawns") or []}
    missing = sorted(a for a in needed if a not in targets)
    if missing:
        raise SelectionError(f"no codemodel target builds {missing[0]}")
    return {"schema": SCHEMA, "sample_seed": seed, "sample_percent": percent,
            "would_skip": skippable, "sampled_executables": sampled, "tests": tests,
            "build_targets": sorted({targets[a] for a in needed})}


def canonical(selection: dict) -> bytes:
    return json.dumps(selection, sort_keys=True, separators=(",", ":")).encode() + b"\n"


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--manifest", required=True, type=Path)
    ap.add_argument("--head-codemodel", required=True, type=Path)
    ap.add_argument("--ctest-json", required=True, type=Path)
    ap.add_argument("--seed", required=True)
    ap.add_argument("--percent", required=True, type=int)
    ap.add_argument("--out", required=True, type=Path)
    a = ap.parse_args(argv[1:])
    try:
        docs = [json.loads(p.read_text(encoding="utf-8")) for p in (a.manifest, a.head_codemodel, a.ctest_json)]
        selection = select(*docs, a.seed, a.percent)
    except (OSError, json.JSONDecodeError, SelectionError) as error:
        print(f"executable-selection: {error}", file=sys.stderr)
        return 1
    a.out.write_bytes(canonical(selection))
    print(f"executable-selection: {len(selection['would_skip'])} would skip, "
          f"{len(selection['sampled_executables'])} sampled, {len(selection['tests'])} tests")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

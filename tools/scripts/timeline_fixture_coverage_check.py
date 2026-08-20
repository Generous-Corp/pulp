#!/usr/bin/env python3
"""Corpus-coverage gate: every persisted sequence schema version has fixtures.

The timeline fixture corpus (`test/fixtures/timeline/`) is versioned by
`pulp.timeline.sequence` schema version: directory `v<N>/` holds the fixtures
authored when the sequence schema was at version N. A schema bump that lands
without adding fixtures leaves a version the corpus can never exercise -- the
v5 gap (fixtures for v1..v4 and v6..v7, none for v5) stood for weeks as proof
that nothing enforced the convention. This script is that enforcement.

    timeline_fixture_coverage_check.py [--schema <path>] [--corpus <dir>]

Exit codes:
    0  every version in the readable range has at least one document fixture
    1  coverage gap (the message names each uncovered version)
    2  operational error (unreadable schema manifest or corpus index, bad usage)

What it reads, and why those are the right sources:

- The required version set comes from the generated schema manifest
  (`core/timeline/schema/timeline_schema.json`), which is the canonical,
  drift-checked projection of the registry. The range is the contiguous
  migration chain: from the first upgrade edge's `from` through
  `x-pulp-current-version`.
- Coverage comes from `corpus.index`, NOT from walking the tree. The corpus is
  deliberately index-enumerated (directory iteration is unreliable on the WASM
  and Android lanes, and an unindexed fixture is dead weight no gate can see),
  so a `v<N>/` directory whose fixtures are not indexed as `document` entries
  does not count as covered.

Scope: the corpus's only established version-dir convention is the sequence
one. Track, clip, project, and asset versions have no fixture-directory
convention today, and this gate does not invent one.

This script is standalone and has no third-party dependencies. It is registered
as the `timeline-fixture-coverage` ctest in test/cmake/timeline_tests.cmake and
runs as a job in .github/workflows/timeline-hardening.yml; the companion
`timeline-fixture-coverage-selftest` ctest proves the gate can go red.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

# Repo-relative defaults, resolved from this file's location
# (<repo>/tools/scripts/timeline_fixture_coverage_check.py). Gate scripts live
# under tools/scripts/ repo-wide.
_THIS = Path(__file__).resolve()
_REPO_ROOT = _THIS.parents[2]
DEFAULT_SCHEMA = _REPO_ROOT / "core" / "timeline" / "schema" / "timeline_schema.json"
DEFAULT_CORPUS = _REPO_ROOT / "test" / "fixtures" / "timeline"

SEQUENCE_TYPE = "pulp.timeline.sequence"


class OperationalError(Exception):
    """A missing or unreadable input: exit 2, never a coverage verdict."""


def required_versions(schema_path: Path) -> set[int]:
    """Versions the corpus must cover, from the schema manifest's own chain."""
    try:
        manifest = json.loads(schema_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise OperationalError(f"cannot read schema manifest {schema_path}: {exc}") from exc
    entry = manifest.get("$defs", {}).get(SEQUENCE_TYPE)
    if not isinstance(entry, dict):
        raise OperationalError(f"schema manifest {schema_path} has no $defs entry for {SEQUENCE_TYPE}")
    current = entry.get("x-pulp-current-version")
    if isinstance(current, bool) or not isinstance(current, int) or current < 1:
        raise OperationalError(
            f"schema manifest {schema_path}: {SEQUENCE_TYPE} has no usable x-pulp-current-version"
        )
    upgrades = entry.get("x-pulp-migrations", {}).get("upgrades", [])
    starts = [e.get("from") for e in upgrades if isinstance(e, dict) and isinstance(e.get("from"), int)]
    if not starts:
        # A v1-only schema legitimately has no edges; anything newer without
        # them is a manifest defect, not a smaller required set.
        if current != 1:
            raise OperationalError(
                f"schema manifest {schema_path}: {SEQUENCE_TYPE} is at v{current} but "
                "declares no migration edges"
            )
        return {1}
    oldest = min(starts)
    # Fail closed on a broken chain rather than weaken the required set: the
    # contiguous-migration invariant is asserted C++-side, so a manifest whose
    # edges do not span oldest..current is not a truth this gate may relax
    # against -- it is an operational defect, reported as exit 2.
    destinations = {e.get("to") for e in upgrades if isinstance(e, dict) and isinstance(e.get("to"), int)}
    if destinations != set(range(oldest + 1, current + 1)):
        raise OperationalError(
            f"schema manifest {schema_path}: {SEQUENCE_TYPE} migration chain does not "
            f"span v{oldest}..v{current} contiguously"
        )
    return set(range(oldest, current + 1))


def covered_versions(corpus_dir: Path) -> set[int]:
    """Versions with at least one indexed `document` fixture in v<N>/."""
    index = corpus_dir / "corpus.index"
    try:
        lines = index.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise OperationalError(f"cannot read corpus index {index}: {exc}") from exc
    covered: set[int] = set()
    for lineno, line in enumerate(lines, start=1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) != 2:
            raise OperationalError(f"{index}:{lineno}: expected '<kind> <relative path>', got: {line!r}")
        kind, rel = parts
        if kind != "document":
            continue
        segments = rel.split("/")
        if len(segments) == 2 and segments[0].startswith("v") and segments[0][1:].isdigit():
            covered.add(int(segments[0][1:]))
    return covered


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--schema", type=Path, default=DEFAULT_SCHEMA,
                        help="schema manifest to read the version chain from")
    parser.add_argument("--corpus", type=Path, default=DEFAULT_CORPUS,
                        help="corpus root containing corpus.index")
    args = parser.parse_args()

    try:
        required = required_versions(args.schema)
        covered = covered_versions(args.corpus)
    except OperationalError as exc:
        print(f"fixture-coverage: operational error: {exc}", file=sys.stderr)
        return 2

    missing = sorted(required - covered)
    if missing:
        print(
            "fixture-coverage: sequence schema versions lacking a corpus document "
            f"fixture: {', '.join(f'v{v}' for v in missing)}\n"
            "every persisted schema version must carry an indexed document "
            "fixture; add one under "
            f"{', '.join(f'test/fixtures/timeline/v{v}/' for v in missing)}",
            file=sys.stderr,
        )
        return 1
    print(
        f"fixture-coverage: all {len(required)} sequence schema versions "
        f"(v{min(required)}..v{max(required)}) carry indexed document fixtures"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())

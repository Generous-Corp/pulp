#!/usr/bin/env python3
"""Shared source-tree traversal for the repository's Python guard tests.

A walk rooted at the repository root does not measure the source tree. A working
checkout also carries build directories, dependency sources fetched under
`_deps`, node packages, and agent tool directories that hold complete checkouts
of *other branches* of this same repository. Every one of those contains files
that look exactly like Pulp source, so a scanner that reads them is slow and can
report a violation that belongs to a different tree - or miss one, when a stale
copy answers in place of the working tree.

Three rules cover the whole class, so a newly invented generated directory does
not need this file edited:

* a directory whose name starts with `.` is never Pulp source, which covers
  every agent or editor tool directory including the ones that nest checkouts;
* a directory whose name starts with `build` is build output, which covers the
  hyphen-suffixed variants (`build-cov`, `build-tsan`) that an exact-name skip
  list silently lets through;
* a small set of named directories holds vendored or submodule content.

Directories are pruned during the walk rather than filtered afterwards, so the
excluded trees are never descended into.
"""

from __future__ import annotations

import fnmatch
import os
from collections.abc import Iterable, Iterator, Sequence
from pathlib import Path

# Prefix matched, so every hyphen-suffixed build directory is covered.
GENERATED_DIR_PREFIXES = ("build",)

# Third-party, submodule, and package-manager content.
VENDORED_DIR_NAMES = frozenset({"external", "planning", "node_modules", "_deps"})


def is_scannable_dir(name: str, extra_excluded: Iterable[str] = ()) -> bool:
    """Whether a directory named `name` holds repository source."""
    if name.startswith("."):
        return False
    if name in VENDORED_DIR_NAMES or name in set(extra_excluded):
        return False
    return not any(name.startswith(prefix) for prefix in GENERATED_DIR_PREFIXES)


def iter_sources(
    root: Path,
    patterns: Sequence[str],
    extra_excluded: Iterable[str] = (),
) -> Iterator[Path]:
    """Yield files under `root` matching any glob in `patterns`, sorted.

    Excluded directories are pruned, so nothing inside them is visited.
    """
    excluded = frozenset(extra_excluded)
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = sorted(
            name for name in dirnames if is_scannable_dir(name, excluded)
        )
        directory = Path(dirpath)
        for name in sorted(filenames):
            if any(fnmatch.fnmatch(name, pattern) for pattern in patterns):
                yield directory / name

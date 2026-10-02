#!/usr/bin/env python3
"""Answer repeated, immutable Git reads once inside a test run.

The GPU acceptance selftests validate one receipt dozens of times, once per
planted negative, and every validation asks Git the same questions about the
same commits: `ls-tree <sha>`, `show <sha>:<path>`, `diff-tree <sha>`, one
process per revision of a history window that grows with main. Those repeats,
not the assertions, are most of the suites' run time.

`memoized_git_reads()` patches `subprocess.run` for the duration of a block
and serves a repeated Git call from memory only when its answer cannot change
during the run:

- the subcommand only reads the object database (`READ_ONLY_SUBCOMMANDS`;
  never `hash-object`, `grep`, `status` or `diff` against the working tree,
  which a test may edit on purpose);
- every revision it names is a full commit SHA, or a range of two, because a
  SHA names immutable content while `HEAD`, branches and short names can move
  (tests commit into scratch repositories);
- the call succeeded; failures are re-run every time so a planted failure is
  never answered from a cache;
- the key holds the argv, the working directory, the stdin bytes and the
  output mode, so two repositories never share an answer.

Everything else passes straight through. `stats` counts hits and misses so a
caller can assert the cache was actually consulted.
"""
from __future__ import annotations

import contextlib
import copy
import os
import re
import subprocess
from dataclasses import dataclass, field
from typing import Any, Iterator

READ_ONLY_SUBCOMMANDS = frozenset({
    "cat-file", "diff-tree", "log", "ls-tree", "merge-base", "rev-list", "rev-parse", "show",
})
_FULL_SHA = re.compile(r"[0-9a-f]{40}")
_SHA_RANGE = re.compile(r"[0-9a-f]{40}\.\.\.?[0-9a-f]{40}")
_SHA_PATH = re.compile(r"[0-9a-f]{40}[:^~].*")


@dataclass
class MemoStats:
    hits: int = 0
    misses: int = 0
    passthrough: int = 0
    keys: set = field(default_factory=set)


def _subcommand(argv: list[str]) -> tuple[str | None, list[str]]:
    """The Git subcommand and its arguments, skipping `-C <dir>` and `-c k=v`."""
    i = 1
    while i < len(argv) and argv[i].startswith("-"):
        i += 2 if argv[i] in ("-C", "-c") else 1
    if i >= len(argv):
        return None, []
    return argv[i], argv[i + 1:]


def cacheable(argv: Any) -> bool:
    """True when `argv` is a Git read whose answer is fixed by full SHAs."""
    if not isinstance(argv, (list, tuple)) or not argv:
        return False
    argv = [str(a) for a in argv]
    if os.path.basename(argv[0]) != "git":
        return False
    sub, rest = _subcommand(argv)
    if sub not in READ_ONLY_SUBCOMMANDS:
        return False
    revisions = []
    for arg in rest:
        if arg == "--":
            break
        if arg.startswith("-"):
            continue
        revisions.append(arg)
    if not revisions:
        return False
    return all(
        _FULL_SHA.fullmatch(r) or _SHA_RANGE.fullmatch(r) or _SHA_PATH.fullmatch(r)
        for r in revisions
    )


@contextlib.contextmanager
def memoized_git_reads() -> Iterator[MemoStats]:
    original = subprocess.run
    cache: dict[tuple, subprocess.CompletedProcess] = {}
    stats = MemoStats()

    def run(*args: Any, **kwargs: Any) -> subprocess.CompletedProcess:
        argv = args[0] if args else kwargs.get("args")
        if not cacheable(argv):
            stats.passthrough += 1
            return original(*args, **kwargs)
        key = (
            tuple(str(a) for a in argv),
            os.path.realpath(str(kwargs.get("cwd") or os.getcwd())),
            kwargs.get("input"),
            bool(kwargs.get("text") or kwargs.get("universal_newlines") or kwargs.get("encoding")),
            bool(kwargs.get("capture_output")),
            repr(kwargs.get("stdout")), repr(kwargs.get("stderr")),
        )
        if key in cache:
            stats.hits += 1
            return copy.copy(cache[key])
        stats.misses += 1
        result = original(*args, **kwargs)
        if result.returncode == 0:
            cache[key] = copy.copy(result)
            stats.keys.add(key[0][:3])
        return result

    subprocess.run = run
    try:
        yield stats
    finally:
        subprocess.run = original

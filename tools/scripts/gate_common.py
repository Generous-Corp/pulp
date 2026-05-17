"""Shared substrate for repo gate scripts.

`version_bump_check.py`, `compat_sync_check.py`, and `skill_sync_check.py`
all rewrote the same handful of helpers — git range queries, trailer
collection, glob-to-regex translation — and the code carried explicit
"mirrors the other gate" comments that admitted the drift risk.

This module consolidates those helpers. Each gate now imports from
here instead of re-implementing. Pure stdlib; no third-party deps.

Behavior contracts preserved verbatim from the previous in-script
copies:

* ``git_range_trailers(base, head)`` walks every commit in the range
  (not just HEAD) because CI checks out a synthetic merge commit, so
  a bypass trailer on the branch tip would be invisible to a HEAD-only
  scan.
* ``_glob_to_regex`` follows the post-#554 rules: ``**`` matches zero
  or more *segments*, ``*`` stays single-segment, slash boundaries are
  preserved around ``**`` so zero-segment matches don't collapse
  (``tools/cli/**/*.cpp`` does not match ``tools/clicmd.cpp``).
* ``strip_meta`` drops top-level keys starting with ``_`` and the
  ``$schema`` key — used to keep in-memory config tidy without forcing
  schema-aware callers.
"""

from __future__ import annotations

import os
import re
import subprocess
from functools import lru_cache
from pathlib import Path
from typing import Iterable


# ── Git helpers ─────────────────────────────────────────────────────────


def repo_root() -> Path:
    out = subprocess.run(
        ["git", "rev-parse", "--show-toplevel"],
        check=True, capture_output=True, text=True,
    )
    return Path(out.stdout.strip())


def git_diff_names(base: str, head: str) -> list[str]:
    out = subprocess.run(
        ["git", "diff", "--name-only", f"{base}..{head}"],
        check=True, capture_output=True, text=True,
    )
    return [line for line in out.stdout.splitlines() if line.strip()]


def _parse_trailer_block(body: str) -> dict[str, list[str]]:
    trailers = subprocess.run(
        ["git", "interpret-trailers", "--parse"],
        input=body, capture_output=True, text=True,
    )
    result: dict[str, list[str]] = {}
    for line in trailers.stdout.splitlines():
        if ":" not in line:
            continue
        key, _, value = line.partition(":")
        result.setdefault(key.strip().lower(), []).append(value.strip())
    return result


def git_range_trailers(base: str, head: str) -> dict[str, list[str]]:
    """Collect trailers from every commit in ``base..head``, merged.

    CI checks out a synthetic merge commit as HEAD, so a bypass trailer
    on the branch's tip commit wouldn't be seen if we only looked at
    HEAD. Walk the whole range — any commit in the range carries the
    bypass.
    """
    try:
        body = subprocess.run(
            ["git", "log", "--format=%B%x00", f"{base}..{head}"],
            check=True, capture_output=True, text=True,
        ).stdout
    except subprocess.CalledProcessError:
        return {}

    result: dict[str, list[str]] = {}
    for chunk in body.split("\x00"):
        if not chunk.strip():
            continue
        for key, values in _parse_trailer_block(chunk).items():
            result.setdefault(key, []).extend(values)
    return result


def git_commit_trailers(ref: str) -> dict[str, list[str]]:
    """Trailers on the single commit ``ref``. Kept for callers that
    deliberately want only the tip (rare; most gates should use
    ``git_range_trailers`` instead)."""
    try:
        body = subprocess.run(
            ["git", "log", "-1", "--format=%B", ref],
            check=True, capture_output=True, text=True,
        ).stdout
    except subprocess.CalledProcessError:
        return {}
    return _parse_trailer_block(body)


# ── Config helpers ──────────────────────────────────────────────────────


def strip_meta(data):
    """Drop top-level ``_*`` keys and ``$schema`` from a config dict.

    Non-dict input passes through unchanged so callers can chain this
    onto ``json.loads(...)`` without type-guarding first.
    """
    if isinstance(data, dict):
        return {
            k: v for k, v in data.items()
            if not k.startswith("_") and k != "$schema"
        }
    return data


# ── Glob matching ───────────────────────────────────────────────────────


@lru_cache(maxsize=None)
def glob_to_regex(pattern: str) -> "re.Pattern[str]":
    """Translate a gitignore-style glob into an anchored regex.

    Semantics:
        * ``**`` matches zero or more path segments (including zero).
        * ``*``  matches zero or more characters within a single segment.
        * ``?``  matches exactly one character within a single segment.
        * Patterns are anchored at both ends.

    Slash handling around ``**`` (post-#554 Codex review): when ``**``
    spans zero segments at the join point, the surrounding slashes
    collapse so ``tools/cli/**/*.cpp`` does NOT match
    ``tools/clicmd.cpp``.
    """
    parts = pattern.split("/")
    n = len(parts)

    STARSTAR = object()
    tokens: list = []
    for part in parts:
        if part == "**":
            tokens.append(STARSTAR)
            continue
        seg = ""
        for c in part:
            if c == "*":
                seg += "[^/]*"
            elif c == "?":
                seg += "[^/]"
            else:
                seg += re.escape(c)
        tokens.append(seg)

    out = ""
    for i, tok in enumerate(tokens):
        is_first = i == 0
        is_last = i == n - 1
        if tok is STARSTAR:
            if is_first and is_last:
                out += ".*"
            elif is_first:
                out += "(?:[^/]+/)*"
            elif is_last:
                if out.endswith("/"):
                    out = out[:-1]
                out += "(?:/.*)?"
            else:
                if not out.endswith("/"):
                    out += "/"
                out += "(?:[^/]+/)*"
        else:
            if not is_first:
                if (not out.endswith("/")
                        and not out.endswith(")?")
                        and not out.endswith(")*")):
                    out += "/"
            out += tok

    return re.compile("^" + out + "$")


def glob_match(path: str, pattern: str) -> bool:
    return glob_to_regex(pattern).match(path) is not None


def matches_any(path: str, patterns: Iterable[str]) -> bool:
    p = path.replace(os.sep, "/")
    for pat in patterns:
        if glob_match(p, pat):
            return True
    return False

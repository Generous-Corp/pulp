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

import json
import os
import re
import subprocess
from dataclasses import dataclass, replace
from functools import lru_cache
from pathlib import Path
from typing import Iterable, Literal


# ── Git helpers ─────────────────────────────────────────────────────────


GitComparisonStatus = Literal[
    "available", "history_unavailable", "path_absent", "command_failed"
]
GitComparisonMode = Literal["base_tip", "merge_base", "exact_base", "unresolved"]


@dataclass(frozen=True)
class GitComparisonProvenance:
    """Immutable receipt for a read-only comparison against Git history."""

    requested_base: str
    requested_head: str
    source: str
    resolved_base_tip: str | None = None
    resolved_head: str | None = None
    comparison_anchor: str | None = None
    comparison_mode: GitComparisonMode = "unresolved"
    status: GitComparisonStatus = "history_unavailable"
    stderr: str = ""
    path: str | None = None
    content: str | None = None


def _bounded_git_stderr(value: str, limit: int = 512) -> str:
    compact = " ".join(value.strip().split())
    return compact if len(compact) <= limit else compact[: limit - 3] + "..."


def git_comparison_receipt(result: GitComparisonProvenance) -> str:
    """Stable single-line receipt; content is deliberately excluded."""
    material = {
        "requested_base": result.requested_base,
        "requested_head": result.requested_head,
        "source": result.source,
        "resolved_base_tip": result.resolved_base_tip,
        "resolved_head": result.resolved_head,
        "comparison_anchor": result.comparison_anchor,
        "comparison_mode": result.comparison_mode,
        "status": result.status,
        "stderr": result.stderr,
    }
    if result.path is not None:
        material["path"] = result.path
    return json.dumps(material, sort_keys=True, separators=(",", ":"))


def git_comparison_command_failed(
    result: GitComparisonProvenance, stderr: str
) -> GitComparisonProvenance:
    """Return a consistent immutable failure receipt after resolution."""
    return replace(
        result,
        status="command_failed",
        stderr=_bounded_git_stderr(stderr),
        content=None,
    )


def git_comparison_history_unavailable(
    result: GitComparisonProvenance, stderr: str
) -> GitComparisonProvenance:
    """Return a bounded immutable receipt for an incomplete history graph."""
    return replace(
        result,
        status="history_unavailable",
        stderr=_bounded_git_stderr(stderr),
        content=None,
    )


def git_comparison_authority_unavailable(
    result: GitComparisonProvenance, stderr: str
) -> GitComparisonProvenance:
    """Invalidate the requested authority while retaining independently proven HEAD."""
    return replace(
        result,
        resolved_base_tip=None,
        comparison_anchor=None,
        comparison_mode="unresolved",
        status="history_unavailable",
        stderr=_bounded_git_stderr(stderr),
        content=None,
    )


def _git_probe(root: "str | Path", arguments: list[str]) -> subprocess.CompletedProcess[str] | None:
    try:
        return subprocess.run(
            ["git", *arguments], cwd=str(root), text=True, capture_output=True
        )
    except OSError:
        return None


def resolve_git_comparison(
    root: "str | Path", requested_base: str, requested_head: str = "HEAD", *,
    source: str = "local_ref",
) -> GitComparisonProvenance:
    """Resolve a reproducible base-to-head anchor without mutating the repo."""
    receipt = GitComparisonProvenance(requested_base, requested_head, source)
    head = _git_probe(root, ["rev-parse", "--verify", f"{requested_head}^{{commit}}"])
    if head is None:
        return replace(receipt, status="command_failed", stderr="could not execute git")
    if head.returncode != 0:
        return replace(
            receipt, status="history_unavailable",
            stderr=_bounded_git_stderr(head.stderr),
        )
    head_tip = head.stdout.strip()
    receipt = replace(receipt, resolved_head=head_tip)

    base = _git_probe(root, ["rev-parse", "--verify", f"{requested_base}^{{commit}}"])
    if base is None:
        return replace(receipt, status="command_failed", stderr="could not execute git")
    if base.returncode != 0:
        return replace(
            receipt, status="history_unavailable",
            stderr=_bounded_git_stderr(base.stderr),
        )
    base_tip = base.stdout.strip()
    receipt = replace(receipt, resolved_base_tip=base_tip)

    ancestry = _git_probe(root, ["merge-base", "--is-ancestor", base_tip, head_tip])
    if ancestry is None:
        return replace(receipt, status="command_failed", stderr="could not execute git")
    if ancestry.returncode == 0:
        return replace(
            receipt, comparison_anchor=base_tip, comparison_mode="base_tip",
            status="available", stderr=_bounded_git_stderr(ancestry.stderr),
        )
    if ancestry.returncode != 1:
        return replace(
            receipt, status="command_failed",
            stderr=_bounded_git_stderr(ancestry.stderr),
        )

    merge_base = _git_probe(root, ["merge-base", head_tip, base_tip])
    if merge_base is None:
        return replace(receipt, status="command_failed", stderr="could not execute git")
    anchor = merge_base.stdout.strip()
    if merge_base.returncode != 0 or not anchor:
        return replace(
            receipt, status="history_unavailable",
            stderr=_bounded_git_stderr(merge_base.stderr) or "merge-base unavailable",
        )
    return replace(
        receipt, comparison_anchor=anchor, comparison_mode="merge_base",
        status="available", stderr=_bounded_git_stderr(merge_base.stderr),
    )


def read_git_path(
    root: "str | Path", comparison: GitComparisonProvenance, path: "str | Path"
) -> GitComparisonProvenance:
    """Read a path at a proven anchor, distinguishing absence from bad history."""
    name = Path(path).as_posix()
    result = replace(comparison, path=name, content=None)
    if comparison.status != "available" or comparison.comparison_anchor is None:
        return result
    listed = _git_probe(root, ["ls-tree", "-z", comparison.comparison_anchor, "--", name])
    if listed is None:
        return replace(result, status="command_failed", stderr="could not execute git")
    if listed.returncode != 0:
        return replace(result, status="command_failed", stderr=_bounded_git_stderr(listed.stderr))
    if not listed.stdout:
        return replace(result, status="path_absent", stderr="")
    shown = _git_probe(root, ["show", f"{comparison.comparison_anchor}:{name}"])
    if shown is None:
        return replace(result, status="command_failed", stderr="could not execute git")
    if shown.returncode != 0:
        return replace(result, status="command_failed", stderr=_bounded_git_stderr(shown.stderr))
    return replace(result, status="available", stderr="", content=shown.stdout)


def repo_root() -> Path:
    out = subprocess.run(
        ["git", "rev-parse", "--show-toplevel"],
        check=True, capture_output=True, text=True,
    )
    return Path(out.stdout.strip())


def git_diff_names(base: str, head: str) -> list[str]:
    """Files this branch actually changed, via a THREE-dot (merge-base) diff.

    Two-dot ``base..head`` diffs the two *trees*, so it also reports files the branch
    is merely BEHIND ``base`` on (e.g. a long-lived PR whose branch predates other
    merges to main) — falsely attributing unrelated files to the PR and tripping the
    skill-sync / version-bump / compat gates on a busy main. Three-dot ``base...head``
    diffs from ``merge-base(base, head)`` to ``head``, i.e. only what this branch added
    since it diverged. That is what every gate means by "what did this PR touch".
    """
    out = subprocess.run(
        ["git", "diff", "--name-only", f"{base}...{head}"],
        check=True, capture_output=True, text=True,
    )
    return [line for line in out.stdout.splitlines() if line.strip()]


# git interpret-trailers --parse only returns a message's FINAL trailer block.
# GitHub's merge-queue `SQUASH + COMMIT_MESSAGES` concatenates every branch commit
# body into one squash body and appends a `--------- / Co-authored-by:` block, so
# any bypass trailer (Skill-Update / Version-Bump / Compat-Update / Config-Doc /
# Reference-Lineage / Release / Custom-Bump / Docs-Update / …) that lands mid-body
# is silently dropped in `merge_group` context — green at PR level (intact
# trailers), failing on the queue candidate, so the PR is added then evicted in a
# loop and never merges.
#
# Rescue is deliberately KEY-AGNOSTIC: match every trailer-shaped line in the
# whole body rather than an allowlist. Every gate reads
# `trailers.get("<specific-key>")` and none enumerates the dict, so surfacing
# extra keys is harmless — and, crucially, no bypass trailer (present OR future)
# can ever be silently voided by the squash again. The `^`-anchor + a mandatory
# space after the colon means only genuine trailer lines match: not `http://…`
# URLs, not `## Heading`, not mid-sentence prose.
_TRAILER_LINE_RE = re.compile(
    r"^(?P<key>[A-Za-z][A-Za-z0-9]*(?:-[A-Za-z0-9]+)*):[ \t]+(?P<val>\S.*?)[ \t]*$",
    re.MULTILINE,
)


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
    # Rescue trailer-shaped lines a merge-queue squash buried mid-body (see
    # _TRAILER_LINE_RE). Only add values interpret-trailers didn't already
    # capture, so a trailer in the final block isn't double-counted.
    for match in _TRAILER_LINE_RE.finditer(body):
        key = match.group("key").strip().lower()
        value = match.group("val").strip()
        values = result.setdefault(key, [])
        if value not in values:
            values.append(value)
    return result


def git_range_trailers(
    base: str, head: str, *, no_merges: bool = False, cwd: "str | Path | None" = None
) -> dict[str, list[str]]:
    """Collect trailers from every commit in ``base..head``, merged.

    CI checks out a synthetic merge commit as HEAD, so a bypass trailer
    on the branch's tip commit wouldn't be seen if we only looked at
    HEAD. Walk the whole range — any commit in the range carries the
    bypass.

    ``no_merges`` (opt-in, default off to preserve every existing caller
    verbatim) drops merge commits from the walk. A caller that treats a
    ``Version-Bump: <surface>=<level>`` value as an AUTHORED release
    *intent* must use it: a "Merge origin/main into <branch>" re-sync
    commit (2+ parents) can carry a stale intent trailer that was never
    meant to declare this range's release, and honoring it would silently
    escalate the version. A PR's real intent lives on its own non-merge
    commits (or the squash commit, which has a single parent), so
    ``--no-merges`` keeps every genuine declaration while excluding the
    re-sync noise.

    ``cwd`` runs the walk in that directory (default: the process cwd), so a
    caller operating on a repo other than its own working directory reads the
    right history.
    """
    cmd = ["git", "log", "--format=%B%x00"]
    if no_merges:
        cmd.append("--no-merges")
    cmd.append(f"{base}..{head}")
    try:
        body = subprocess.run(
            cmd, check=True, capture_output=True, text=True,
            cwd=str(cwd) if cwd is not None else None,
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

    Slash handling around ``**`` (post-#554): when ``**``
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
    p = path.replace("\\", "/").replace(os.sep, "/")
    for pat in patterns:
        if glob_match(p, pat):
            return True
    return False

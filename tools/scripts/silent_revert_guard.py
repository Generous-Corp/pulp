#!/usr/bin/env python3
"""Silent-revert guard — refuse a push that byte-exactly undoes a recent landing.

The shape this catches: a branch whose diff restores the EXACT pre-landing bytes
of every file a recently-landed commit changed, with no stated revert intent. A
stale worktree, a mis-resolved merge, or a `git add -A` over old content can all
produce it, and the result reads as ordinary work in review — the diff looks like
a plausible edit, not an undo. It lands and silently erases the earlier change.

The guard is deliberately:

  * FAST and ZERO-CI-COST — pure blob-sha comparison. No build, no network, no
    model. It runs as a local pre-push check, not a CI job, and is sub-second on
    a normal diff.
  * NARROW — it fires ONLY on a byte-exact WHOLESALE revert of a RECENT landing
    that carries no revert intent. New bytes, unrelated files, partial reverts,
    an explicit `Revert "..."`, and old history are all clean. A backstop that
    fires on honest work is worse than the bug it prevents, so the predicate is
    exact rather than heuristic.
  * SELF-CONTAINED — plain git plus the standard library. No external service is
    consulted and none is required.

Intent is what separates a silent revert from a legitimate one. `git revert`
produces the same bytes on purpose; that is a normal, necessary operation and
must stay unblocked. So a range that states its intent — a `Revert "..."`
subject, a `Revert-Of:` trailer, or the explicit skip trailer — passes.

Usage:
    python3 tools/scripts/silent_revert_guard.py --base origin/main --mode=report

Exit codes:
    0 — clean, or hint mode (which always exits 0 but still reports loudly)
    1 — blocked: the push byte-exactly reverts a recent landing (report mode)
    2 — Git history unavailable; no verdict was produced (report mode only)
"""
from __future__ import annotations

import os
import re
import subprocess
from dataclasses import dataclass, field
from datetime import datetime, timedelta, timezone
from typing import Optional

from gate_common import (
    GitComparisonProvenance,
    git_comparison_command_failed,
    git_comparison_history_unavailable,
    git_comparison_receipt,
    resolve_git_comparison,
)

# A range that says what it is doing is not a SILENT revert. `git revert` writes
# the first form; the trailers are the deliberate, auditable escape hatches.
_REVERT_SUBJECT = re.compile(r'^Revert\s+"', re.MULTILINE)
_REVERT_TRAILER = re.compile(r"^Revert-Of:\s*\S+", re.MULTILINE)
_SKIP_TRAILER = re.compile(r'^Silent-Revert:\s*skip\s+reason="[^"]+"', re.MULTILINE)


# ===========================================================================
# DATA
# ===========================================================================
@dataclass
class MergeRecord:
    """A recently-landed commit, reduced to what the guard needs: for every path
    it changed, the blob sha BEFORE it (pre) and AFTER it (post)."""

    merge_id: str
    merged_at: datetime
    # path -> {"pre": <sha or None if added>, "post": <sha>}
    changes: dict

    def touched_paths(self) -> set[str]:
        return set(self.changes.keys())


@dataclass
class GuardVerdict:
    blocked: bool
    reason: str = ""
    merge_id: str = ""
    reverted_paths: list[str] = field(default_factory=list)
    history_status: str = "available"
    provenance: Optional[GitComparisonProvenance] = None


# ===========================================================================
# THE PURE PREDICATE — the whole property lives here.
# ===========================================================================
def is_byte_exact_revert(merge: MergeRecord, proposed: dict) -> bool:
    """True iff `proposed` (path -> new blob sha) byte-exactly reverts `merge`.

    A silent revert is WHOLESALE: every path the landing changed is set back to
    its pre-landing blob, and the landing genuinely changed something. Require:

      1. The landing actually changed at least one path (post != pre somewhere) —
         a no-op cannot be reverted.
      2. Every path it touched appears in the proposed change AND is set back to
         EXACTLY its pre-landing sha (proposed[path] == pre).
         * A path the landing ADDED (pre is None) is reverted by DELETING it —
           represented as proposed[path] == None (a tombstone).
      3. At least one of those paths actually moves OFF the post sha, so this is
         a revert rather than a coincidental no-change.

    A partial revert (some but not all of the landing's paths restored) does NOT
    fire — the guard is a backstop for the exact accident, and partial changes
    are legitimate work.
    """
    touched = merge.touched_paths()
    if not touched:
        return False

    merge_changed_something = any(
        c.get("pre") != c.get("post") for c in merge.changes.values()
    )
    if not merge_changed_something:
        return False

    moved_off_post = False
    for path in touched:
        pre = merge.changes[path].get("pre")
        post = merge.changes[path].get("post")
        # The proposed change must restore this path to its pre-landing state.
        if path not in proposed:
            return False
        new = proposed[path]
        if new != pre:
            return False
        if post != pre:
            moved_off_post = True

    return moved_off_post


# ===========================================================================
# THE BACKSTOP
# ===========================================================================
class GuardBackstop:
    def __init__(self, recent_window_hours: float = 72.0):
        # "Recent" = landed within this window. Reverting an ancient commit is a
        # deliberate historical operation, not the accident this guards.
        self.recent_window = timedelta(hours=recent_window_hours)

    def check(
        self,
        proposed: dict,
        recent_merges: list[MergeRecord],
        now: Optional[datetime] = None,
    ) -> GuardVerdict:
        """Classify a proposed change against recent landings. Returns the first
        recent landing the proposed change byte-exactly reverts, or a clean pass.
        Pure hash comparison — fast, no CI, no model."""
        now = now or datetime.now(timezone.utc)
        for merge in recent_merges:
            if now - merge.merged_at > self.recent_window:
                continue  # not recent — deliberate history op, not the accident
            if is_byte_exact_revert(merge, proposed):
                return GuardVerdict(
                    blocked=True,
                    reason=(
                        "byte-exact wholesale revert of recently-landed "
                        f"{merge.merge_id[:12]} with no stated revert intent"
                    ),
                    merge_id=merge.merge_id,
                    reverted_paths=sorted(merge.touched_paths()),
                )
        return GuardVerdict(
            blocked=False, reason="no byte-exact revert of a recent landing"
        )


# ===========================================================================
# GIT ADAPTER — real refs in production; tests inject snapshots for the predicate.
# ===========================================================================
def _git(
    args: list[str], cwd: Optional[str] = None,
    env: Optional[dict[str, str]] = None,
) -> str:
    return subprocess.run(
        ["git", *args], cwd=cwd, capture_output=True, text=True, check=True,
        env=env,
    ).stdout


def _blob_sha(repo: str, rev: str, path: str) -> Optional[str]:
    try:
        return _git(["rev-parse", f"{rev}:{path}"], cwd=repo).strip() or None
    except Exception:
        return None  # path did not exist at that rev (added/deleted)


def _blob_sha_checked(
    repo: str, rev: str, path: str,
    env: Optional[dict[str, str]] = None,
) -> tuple[Optional[str], str]:
    try:
        listed = subprocess.run(
            ["git", "ls-tree", "-z", rev, "--", path], cwd=repo,
            capture_output=True, text=True, env=env,
        )
    except OSError as error:
        return None, str(error)[:512]
    if listed.returncode != 0:
        return None, " ".join(listed.stderr.strip().split())[:512]
    if not listed.stdout:
        return None, ""
    try:
        return _git(
            ["rev-parse", f"{rev}:{path}"], cwd=repo, env=env
        ).strip(), ""
    except Exception as error:
        return None, str(error)[:512]


def _since_arg(since_hours: float, now: Optional[datetime] = None) -> str:
    """An explicit ISO-8601 cutoff for --since.

    NOT a `"<n> hours ago"` string. git's approxidate parser accepts a float
    duration and then silently means something else entirely: measured against
    this repo, `--since="72.0 hours ago"` matched all 4367 commits (i.e. no
    filter at all) while `--since="120.0 hours ago"` matched zero. It never
    errors, so a float window fails silently in EITHER direction — returning
    everything, or returning nothing and taking the guard quietly offline. Only
    the integer form (`"72 hours ago"` -> 47) parses as intended.

    An absolute timestamp sidesteps approxidate entirely and keeps a fractional
    window honest.
    """
    now = now or datetime.now(timezone.utc)
    return (now - timedelta(hours=since_hours)).isoformat()


def recent_landings(
    repo: str,
    base_ref: str = "origin/main",
    since_hours: float = 72.0,
    paths: Optional[list[str]] = None,
    now: Optional[datetime] = None,
) -> list[MergeRecord]:
    """The base branch's recent landings, as MergeRecords.

    Walks --first-parent, NOT --merges. Every commit on the first-parent line is
    a landing however it got there, and a squash-merged PR is a SINGLE-parent
    commit that a --merges filter cannot see. Squash is how most PRs land here,
    so filtering to merge commits would make the guard blind to the exact shape
    it exists to catch.

    `paths` narrows the walk to landings that touched at least one path the push
    modifies. That is a superset of what can possibly fire — a wholesale revert
    must restore EVERY path its landing touched, so a landing sharing no path
    with the push can never match — and it keeps the guard fast: without it,
    every landing in the window costs a diff-tree plus two rev-parse per file.

    Best-effort: an unreadable repo or ref returns [], degrading to a pass.
    """
    out: list[MergeRecord] = []
    argv = [
        "log",
        "--first-parent",
        f"--since={_since_arg(since_hours, now)}",
        "--format=%H %cI",
        base_ref,
    ]
    if paths:
        argv.append("--")
        argv.extend(paths)
    try:
        log = _git(argv, cwd=repo)
    except Exception:
        return out
    for line in log.splitlines():
        parts = line.split(None, 1)
        if len(parts) != 2:
            continue
        sha, when = parts
        try:
            merged_at = datetime.fromisoformat(when.replace("Z", "+00:00"))
        except Exception:
            continue
        record = landing_record(
            repo, sha, merged_at, only_within=set(paths) if paths else None
        )
        if record is not None:
            out.append(record)
    return out


def landing_record(
    repo: str,
    sha: str,
    merged_at: Optional[datetime] = None,
    only_within: Optional[set[str]] = None,
) -> Optional[MergeRecord]:
    """Build a MergeRecord for one landed commit: its pre/post blob per path.

    `only_within` skips a landing whose touched paths are not all present in that
    set, returning None WITHOUT reading a single blob. This is a pure cost
    optimization with no effect on the verdict: a wholesale revert must restore
    EVERY path its landing touched, so `is_byte_exact_revert` already returns
    False the moment one is missing from the proposed change.

    It matters because the blob reads are the expensive part — two per file — and
    the landings that share a path with a push are often large. Measured on a
    5-file push here: 2 candidate landings touching 561 files between them, i.e.
    ~1124 git invocations and ~18s, versus one name listing each and ~0.2s.
    """
    if merged_at is None:
        try:
            when = _git(["log", "-1", "--format=%cI", sha], cwd=repo).strip()
            merged_at = datetime.fromisoformat(when.replace("Z", "+00:00"))
        except Exception:
            return None
    try:
        parent = _first_parent(repo, sha)
        revisions = [sha] if parent is None else [parent, sha]
        names = _git(
            [
                "diff-tree", "--root", "--no-commit-id", "--name-only", "-z",
                "-r", *revisions,
            ],
            cwd=repo,
        )
    except Exception:
        return None
    touched = [path for path in names.split("\0") if path]
    if only_within is not None and not set(touched).issubset(only_within):
        return None
    changes: dict = {}
    for path in touched:
        changes[path] = {
            "pre": _blob_sha(repo, f"{sha}^", path),
            "post": _blob_sha(repo, sha, path),
        }
    return MergeRecord(merge_id=sha, merged_at=merged_at, changes=changes)


def _first_parent(
    repo: str, sha: str, env: Optional[dict[str, str]] = None
) -> Optional[str]:
    """Return the exact first parent, or None only for a proven root commit."""
    line = _git(["rev-list", "--parents", "-n", "1", sha], cwd=repo, env=env)
    parts = line.strip().split()
    if not parts or parts[0] != sha:
        raise RuntimeError(f"malformed parent record for {sha}: {line!r}")
    return parts[1] if len(parts) > 1 else None


def proposed_from_git(repo: str, base_ref: str, head_ref: str = "HEAD") -> dict:
    """The change this push proposes: path -> blob sha at head (None = deleted).

    Scoped to paths the branch ITSELF modified, measured from its merge-base with
    the base branch — NOT a tip-vs-tip diff.

    The distinction is the guard's whole false-positive story. A branch that is
    merely BEHIND the base differs from the base's tip on every path landed since
    it was cut, and at those paths its blobs ARE the pre-landing bytes. Tip-vs-tip
    would therefore flag every out-of-date branch as a revert of everything that
    landed while it was open — the common case, and always wrong: merging simply
    keeps the base's side for paths the branch never touched.

    Measured from the merge-base, an untouched path is absent from the diff and
    cannot fire. Only a branch that ACTIVELY rewrites a landed file back to its
    old bytes appears here — which is exactly the accident.
    """
    try:
        base_sha = _git(["merge-base", base_ref, head_ref], cwd=repo).strip()
    except Exception:
        return {}
    try:
        names = _git(
            ["diff", "--name-only", "-z", base_sha, head_ref], cwd=repo
        )
    except Exception:
        return {}
    proposed: dict = {}
    for path in names.split("\0"):
        if path:
            proposed[path] = _blob_sha(repo, head_ref, path)
    return proposed


def _proposed_from_comparison(
    repo: str, comparison: GitComparisonProvenance
) -> tuple[dict, str]:
    if comparison.comparison_anchor is None or comparison.resolved_head is None:
        return {}, "comparison anchor is unavailable"
    try:
        raw = _git(
            ["diff", "--raw", "-z", "--no-abbrev", "--no-renames",
             comparison.comparison_anchor,
             comparison.resolved_head], cwd=repo
        )
    except Exception as error:
        return {}, str(error)[:512]
    changes, error = _parse_raw_changes(raw)
    if error:
        return {}, error
    return {path: post for path, (_, post) in changes.items()}, ""


def _parse_raw_changes(raw: str) -> tuple[dict[str, tuple[Optional[str], Optional[str]]], str]:
    """Parse --raw -z --no-renames output into exact path/blob pairs."""
    changes: dict[str, tuple[Optional[str], Optional[str]]] = {}
    fields = raw.split("\0")
    if fields and fields[-1] == "":
        fields.pop()
    if len(fields) % 2:
        return {}, f"malformed NUL-delimited raw Git diff: {raw[:420]!r}"[:512]
    for metadata, path in zip(fields[0::2], fields[1::2]):
        try:
            record = metadata.split()
            if len(record) != 5 or not record[0].startswith(":") or not path:
                raise ValueError
            old_blob, new_blob = record[2], record[3]
        except ValueError:
            return {}, f"malformed raw Git diff record: {(metadata, path)!r}"[:512]
        changes[path] = (
            None if not old_blob.strip("0") else old_blob,
            None if not new_blob.strip("0") else new_blob,
        )
    return changes, ""


def _has_revert_intent_checked(
    repo: str, comparison: GitComparisonProvenance
) -> tuple[bool, str]:
    if comparison.comparison_anchor is None or comparison.resolved_head is None:
        return False, "comparison anchor is unavailable"
    try:
        body = _git(
            ["log", "--format=%B",
             f"{comparison.comparison_anchor}..{comparison.resolved_head}"], cwd=repo
        )
    except Exception as error:
        return False, str(error)[:512]
    return bool(
        _REVERT_SUBJECT.search(body)
        or _REVERT_TRAILER.search(body)
        or _SKIP_TRAILER.search(body)
    ), ""


def _recent_landings_checked(
    repo: str, base_ref: str, since_hours: float, paths: list[str],
    now: Optional[datetime],
) -> tuple[list[MergeRecord], str, str]:
    try:
        shallow = _git(
            ["rev-parse", "--is-shallow-repository"], cwd=repo
        ).strip().lower()
    except Exception as error:
        return [], str(error)[:512], "command_failed"
    if shallow not in {"true", "false"}:
        return [], (
            f"unexpected --is-shallow-repository output: {shallow!r}"
        ), "command_failed"
    history_env: Optional[dict[str, str]] = None
    if shallow == "true":
        history_env = dict(os.environ)
        history_env["GIT_SHALLOW_FILE"] = os.devnull
    # Prove the complete first-parent ancestry before applying a path filter.
    # Git's date traversal is not monotonic, so --since cannot safely bound this
    # walk: an old-dated commit may have a newer-dated parent behind it.
    argv = ["log", "--first-parent", "--format=%H %cI", base_ref]
    try:
        log = _git(argv, cwd=repo, env=history_env)
    except Exception as error:
        status = "history_unavailable" if shallow == "true" else "command_failed"
        detail = str(error)[:420]
        if shallow == "true":
            detail = "shallow history has missing hidden objects: " + detail
        return [], detail, status
    cutoff = (now or datetime.now(timezone.utc)) - timedelta(hours=since_hours)
    history: list[tuple[str, datetime]] = []
    for line in log.splitlines():
        parts = line.split(None, 1)
        if len(parts) != 2:
            return [], f"malformed git log record: {line!r}"[:512], "command_failed"
        sha, when = parts
        try:
            merged_at = datetime.fromisoformat(when.replace("Z", "+00:00"))
        except Exception as error:
            return [], str(error)[:512], "command_failed"
        history.append((sha, merged_at))

    recent_indexes = [
        index for index, (_, merged_at) in enumerate(history)
        if merged_at >= cutoff
    ]
    if not recent_indexes:
        return [], "", "available"

    furthest_recent = max(recent_indexes)
    if furthest_recent + 1 < len(history):
        # Exclude the first parent immediately older than the required prefix.
        path_range = f"{history[furthest_recent + 1][0]}..{base_ref}"
    else:
        path_range = base_ref
    try:
        path_log = _git(
            ["log", "--first-parent", "--format=%H", path_range, "--", *paths],
            cwd=repo, env=history_env,
        )
    except Exception as error:
        status = "history_unavailable" if shallow == "true" else "command_failed"
        return [], str(error)[:512], status

    timestamps = dict(history)
    records: list[MergeRecord] = []
    for sha in (line.strip() for line in path_log.splitlines()):
        if not sha:
            continue
        merged_at = timestamps.get(sha)
        if merged_at is None:
            return [], (
                f"path-limited history returned unknown commit {sha}"
            ), "command_failed"
        # Keep filtering after the bounded path walk: commit dates within the
        # exact prefix can still move backwards and forwards.
        if merged_at < cutoff:
            continue
        try:
            parent = _first_parent(repo, sha, history_env)
            revisions = [sha] if parent is None else [parent, sha]
            raw = _git(
                [
                    "diff-tree", "--root", "--no-commit-id", "--raw", "-z",
                    "--no-abbrev", "--no-renames", "-r", *revisions,
                ],
                cwd=repo, env=history_env,
            )
        except Exception as error:
            status = "history_unavailable" if shallow == "true" else "command_failed"
            return [], str(error)[:512], status
        changes, error = _parse_raw_changes(raw)
        if error:
            return [], error, "command_failed"
        touched = list(changes)
        if not set(touched).issubset(paths):
            continue
        records.append(MergeRecord(
            sha, merged_at,
            {path: {"pre": pre, "post": post}
             for path, (pre, post) in changes.items()},
        ))
    return records, "", "available"


def has_revert_intent(repo: str, base_ref: str, head_ref: str = "HEAD") -> bool:
    """True if any commit in the range states that it is a revert.

    An intentional revert is not a SILENT revert. `git revert` writes a
    `Revert "..."` subject; `Revert-Of:` and the skip trailer are the deliberate,
    auditable escapes for a hand-built one. Any of them exempts the range.
    """
    try:
        body = _git(["log", "--format=%B", f"{base_ref}..{head_ref}"], cwd=repo)
    except Exception:
        return False
    return bool(
        _REVERT_SUBJECT.search(body)
        or _REVERT_TRAILER.search(body)
        or _SKIP_TRAILER.search(body)
    )


def check_push(
    repo: str,
    base_ref: str = "origin/main",
    head_ref: str = "HEAD",
    since_hours: float = 72.0,
    now: Optional[datetime] = None,
) -> GuardVerdict:
    """End-to-end: classify what this branch would do to the base branch.

    `now` anchors the recency window; it exists so a historical incident can be
    replayed at the moment it actually happened rather than against wall-clock
    time, which would put it outside the window and pass for the wrong reason.
    """
    comparison = resolve_git_comparison(
        repo, base_ref, head_ref, source="silent_revert_guard"
    )
    if comparison.status != "available":
        return GuardVerdict(
            blocked=True, reason="Git history unavailable; no verdict produced",
            history_status=comparison.status, provenance=comparison,
        )
    has_intent, error = _has_revert_intent_checked(repo, comparison)
    if error:
        failed = git_comparison_command_failed(comparison, error)
        return GuardVerdict(
            blocked=True, reason=failed.stderr, history_status=failed.status,
            provenance=failed,
        )
    if has_intent:
        return GuardVerdict(blocked=False, reason="explicit revert intent stated",
                            provenance=comparison)
    proposed, error = _proposed_from_comparison(repo, comparison)
    if error:
        failed = git_comparison_command_failed(comparison, error)
        return GuardVerdict(
            blocked=True, reason=failed.stderr, history_status=failed.status,
            provenance=failed,
        )
    if not proposed:
        return GuardVerdict(blocked=False, reason="no changes proposed",
                            provenance=comparison)
    landings, error, history_status = _recent_landings_checked(
        repo, comparison.resolved_base_tip or base_ref, since_hours,
        sorted(proposed.keys()), now,
    )
    if error:
        failed = (
            git_comparison_history_unavailable(comparison, error)
            if history_status == "history_unavailable"
            else git_comparison_command_failed(comparison, error)
        )
        return GuardVerdict(
            blocked=True, reason=failed.stderr, history_status=failed.status,
            provenance=failed,
        )
    verdict = GuardBackstop(since_hours).check(proposed, landings, now=now)
    verdict.provenance = comparison
    return verdict


def main(argv: Optional[list[str]] = None) -> int:
    import argparse

    ap = argparse.ArgumentParser(description="Silent-revert guard")
    ap.add_argument("--repo", default=".")
    ap.add_argument("--base", default="origin/main")
    ap.add_argument("--head", default="HEAD")
    ap.add_argument("--since-hours", type=float, default=72.0)
    ap.add_argument(
        "--mode",
        choices=["report", "hint"],
        default="report",
        help=(
            "report exits 1 on a content block and 2 on unavailable history; "
            "hint always exits 0 but still reports either condition loudly"
        ),
    )
    args = ap.parse_args(argv)

    verdict = check_push(args.repo, args.base, args.head, args.since_hours)
    if verdict.history_status != "available":
        print("  silent-revert guard: HISTORY UNAVAILABLE")
        print(f"    {verdict.reason}")
        if verdict.provenance is not None:
            print("    comparison receipt=" + git_comparison_receipt(verdict.provenance))
        return 2 if args.mode == "report" else 0
    if not verdict.blocked:
        print(f"  silent-revert guard: ok ({verdict.reason})")
        if verdict.provenance is not None:
            print("    comparison receipt=" + git_comparison_receipt(verdict.provenance))
        return 0

    print("  silent-revert guard: BLOCKED")
    print(f"    {verdict.reason}")
    if verdict.provenance is not None:
        print("    comparison receipt=" + git_comparison_receipt(verdict.provenance))
    print(f"    landing: {verdict.merge_id}")
    print("    paths restored to their pre-landing bytes:")
    for path in verdict.reverted_paths:
        print(f"      {path}")
    print("")
    print("    This push would erase that commit's changes. If it is accidental —")
    print("    a stale worktree, a mis-resolved merge, `git add -A` over old")
    print("    content — rebase onto the current base and re-apply your work.")
    print("    If the revert is DELIBERATE, state the intent:")
    print('      Silent-Revert: skip reason="..."')
    return 1 if args.mode == "report" else 0


if __name__ == "__main__":
    raise SystemExit(main())

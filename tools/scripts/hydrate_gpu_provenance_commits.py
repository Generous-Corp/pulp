#!/usr/bin/env python3
"""Fetch only the Pulp commits named by checked-in GPU provenance evidence."""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import os
import subprocess
import sys
from typing import Any


SHA = re.compile(r"[0-9a-f]{40}")
MAX_COMMITS = 128


class HydrationError(RuntimeError):
    """Checked-in provenance cannot be materialized safely."""


def load_object(path: pathlib.Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise HydrationError(f"cannot read provenance document {path}: {error}") from error
    if not isinstance(value, dict):
        raise HydrationError(f"provenance document is not an object: {path}")
    return value


def required_commits(root: pathlib.Path) -> list[str]:
    handoff = load_object(root / "docs/status/gpu-vellum-handoff.yaml")
    receipt = load_object(
        root / "docs/validation/gpu-probes/m3-a2-real-probes-20260828/receipt.json"
    )
    revisions: set[str] = set()
    for entry in handoff.get("entries", []):
        if not isinstance(entry, dict):
            raise HydrationError("GPU handoff contains a non-object entry")
        for row in entry.get("pulp_paths", []):
            if not isinstance(row, dict):
                raise HydrationError("GPU handoff contains a non-object Pulp path")
            if row.get("repo") != "Generous-Corp/pulp":
                raise HydrationError("GPU handoff Pulp path names a non-Pulp repository")
            revision = row.get("revision")
            if not isinstance(revision, str) or SHA.fullmatch(revision) is None:
                raise HydrationError("GPU handoff Pulp path has an invalid revision")
            revisions.add(revision)
    equivalent_head = receipt.get("verification_equivalent_head")
    if not isinstance(equivalent_head, str) or SHA.fullmatch(equivalent_head) is None:
        raise HydrationError("GPU probe receipt has an invalid verification_equivalent_head")
    revisions.add(equivalent_head)
    if not revisions or len(revisions) > MAX_COMMITS:
        raise HydrationError(
            f"GPU provenance requests {len(revisions)} commits; allowed range is 1..{MAX_COMMITS}"
        )
    return sorted(revisions)


def is_commit(root: pathlib.Path, revision: str) -> bool:
    return subprocess.run(
        ["git", "cat-file", "-e", f"{revision}^{{commit}}"], cwd=root,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False,
    ).returncode == 0


def event_ref_candidates(event_ref: str, event_sha: str = "") -> list[str]:
    """Fetch sources to try, most exact first, to reconnect this event's history.

    A `pull_request` event names `refs/pull/<n>/merge`, which exists only while
    GitHub holds a computed merge commit for an open, non-conflicting pull
    request. The pull request's `head` ref has no such lifetime and reaches the
    same provenance commits, so it is the fallback rather than a second guess.

    A `merge_group` event names `refs/heads/gh-readonly-queue/<base>/pr-<n>-<sha>`,
    whose lifetime is shorter still: the queue deletes that branch the moment it
    re-forms the batch onto a newer base, while the run holding the old name runs
    on. So the event's head object id is the last candidate. It names the same
    history the ref did, and it stays fetchable once no ref points at it, which
    is why `actions/checkout` fetches the event by object id rather than by name.
    A commit id is only ever appended, never substituted for an exact ref, so a
    reachable ref still decides the fetch.
    """
    candidates = [event_ref]
    merge_ref = re.fullmatch(r"refs/pull/(\d+)/merge", event_ref)
    if merge_ref:
        candidates.append(f"refs/pull/{merge_ref.group(1)}/head")
    if SHA.fullmatch(event_sha) and event_sha not in candidates:
        candidates.append(event_sha)
    return candidates


def offline_git_env() -> dict[str, str]:
    """Git environment that answers from local objects or not at all.

    A blobless partial clone resolves a missing object over the network, so a
    presence probe against an absent commit pays seconds of transport before it
    can report the absence. Verification is a statement about what this
    checkout already holds, so the lazy fetch is refused outright.
    """
    env = dict(os.environ)
    env["GIT_NO_LAZY_FETCH"] = "1"
    return env


def present_commits(root: pathlib.Path, revisions: list[str]) -> set[str]:
    """The subset of `revisions` this checkout already holds as commits.

    One batch call for the whole set: a spawn per revision buys the same answer
    for tens of times the cost, and this runs on every push.
    """
    if not revisions:
        return set()
    completed = subprocess.run(
        ["git", "cat-file", "--batch-check=%(objectname) %(objecttype)"],
        cwd=root, input="\n".join(revisions) + "\n",
        text=True, capture_output=True, check=False, env=offline_git_env(),
    )
    present = set()
    for line in completed.stdout.splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[1] == "commit":
            present.add(parts[0])
    return present


def committer_dates(root: pathlib.Path, revisions: list[str]) -> dict[str, int]:
    """Committer timestamps for revisions this checkout can already read.

    Pass only revisions known to be present: one absent argument makes Git
    reject the whole invocation, which would cost every other row its date.
    """
    if not revisions:
        return {}
    completed = subprocess.run(
        ["git", "log", "--no-walk", "--format=%H %ct", *sorted(revisions)],
        cwd=root, text=True, capture_output=True, check=False,
        env=offline_git_env(),
    )
    dates: dict[str, int] = {}
    for line in completed.stdout.splitlines():
        parts = line.split()
        if len(parts) == 2 and SHA.fullmatch(parts[0]) and parts[1].isdigit():
            dates[parts[0]] = int(parts[1])
    return dates


def shallow_boundaries(root: pathlib.Path) -> list[str]:
    """Commits this checkout treats as parentless grafts.

    The file lives in the COMMON Git directory, so every linked worktree of a
    truncated clone shares one horizon rather than carrying its own.
    """
    completed = subprocess.run(
        ["git", "rev-parse", "--git-common-dir"], cwd=root,
        text=True, capture_output=True, check=False,
    )
    if completed.returncode != 0:
        return []
    common = pathlib.Path(completed.stdout.strip())
    if not common.is_absolute():
        common = root / common
    try:
        content = (common / "shallow").read_text(encoding="utf-8")
    except OSError:
        return []
    return [token for token in content.split() if SHA.fullmatch(token)]


def reachable_commits(root: pathlib.Path) -> set[str]:
    """Every commit reachable from HEAD or from any remote-tracking branch.

    Ancestry against HEAD alone is the wrong question for a push-time check: a
    developer working on a branch cut before a pin landed fails it for every
    such pin, which is ordinary work rather than a defect. What a push needs to
    know is whether a commit will exist on the remote once it lands. HEAD
    covers what is being pushed, the remote-tracking refs cover what is already
    published, and a commit rewritten away before its first push is reachable
    from neither.
    """
    completed = subprocess.run(
        ["git", "rev-list", "HEAD", "--remotes"], cwd=root,
        text=True, capture_output=True, check=False, env=offline_git_env(),
    )
    if completed.returncode != 0:
        detail = (completed.stderr or completed.stdout).strip()
        raise HydrationError(f"cannot enumerate reachable commits: {detail}")
    return set(completed.stdout.split())


def verify(root: pathlib.Path) -> tuple[list[str], list[str], int | None, int | None]:
    """Adjudicate every pinned commit against what this checkout can prove.

    Returns the unpublishable rows, the rows a truncated checkout cannot answer
    for, the graft horizon, and the oldest pinned timestamp. A complete clone
    reports no undecidable rows; a truncated one refuses to call a row broken
    when its own missing history is an equally good explanation.
    """
    revisions = required_commits(root)
    boundaries = shallow_boundaries(root)
    present = present_commits(root, sorted(set(revisions) | set(boundaries)))
    reachable = reachable_commits(root)
    dates = committer_dates(root, sorted(present))
    horizon_dates = [dates[sha] for sha in boundaries if sha in dates]
    horizon = max(horizon_dates) if horizon_dates else None
    pinned_dates = [dates[rev] for rev in revisions if rev in dates]
    oldest = min(pinned_dates) if pinned_dates else None

    suspect = [rev for rev in revisions if rev not in present or rev not in reachable]
    if not suspect:
        return [], [], horizon, oldest
    if not boundaries:
        return suspect, [], horizon, oldest
    if horizon is None:
        # Truncated, with no readable horizon to adjudicate against. Reporting
        # these as broken would invent a finding out of a missing measurement.
        return [], suspect, horizon, oldest
    broken, undecidable = [], []
    for revision in suspect:
        when = dates.get(revision)
        if when is not None and when > horizon:
            broken.append(revision)
        else:
            undecidable.append(revision)
    return broken, undecidable, horizon, oldest


def command_verify(root: pathlib.Path) -> int:
    try:
        broken, undecidable, horizon, oldest = verify(root)
    except HydrationError as error:
        print(f"gpu-provenance-verify: FAIL: {error}", file=sys.stderr)
        return 1
    for revision in undecidable:
        print(
            f"gpu-provenance-verify: SKIP: {revision} is unreachable, and this "
            "checkout is truncated at or after that commit, so a rewritten pin "
            "and history this clone never fetched look identical here",
            file=sys.stderr,
        )
    if horizon is not None and oldest is not None:
        margin = (oldest - horizon) // 86400
        print(
            f"gpu-provenance-verify: horizon margin {margin}d "
            "(oldest pinned commit against this clone's graft boundary)",
            file=sys.stderr,
        )
    if broken:
        for revision in broken:
            print(
                f"gpu-provenance-verify: FAIL: {revision} is pinned by checked-in "
                "GPU provenance but is reachable from neither HEAD nor any "
                "remote-tracking branch, so it will not exist on the remote after "
                "this push; regenerate the ledger against the commit that "
                "replaced it",
                file=sys.stderr,
            )
        return 1
    print(
        f"gpu-provenance-verify: PASS skipped={len(undecidable)} cap={MAX_COMMITS}"
    )
    return 0


def hydrate(root: pathlib.Path, remote: str) -> tuple[int, int]:
    revisions = required_commits(root)
    missing = [revision for revision in revisions if not is_commit(root, revision)]
    shallow = subprocess.run(
        ["git", "rev-parse", "--is-shallow-repository"], cwd=root,
        text=True, capture_output=True, check=True,
    ).stdout.strip() == "true"
    # A warm self-hosted checkout can retain every object while checkout@v5
    # rewrites the shallow boundary. Object presence therefore does not prove
    # ancestry; always reconnect the exact event history when Git says the
    # repository is shallow.
    if shallow:
        event_ref = os.environ.get("GITHUB_REF", "")
        if not event_ref.startswith("refs/"):
            raise HydrationError("shallow checkout lacks an exact GITHUB_REF to hydrate")
        failures = []
        event_sha = os.environ.get("GITHUB_SHA", "")
        for candidate in event_ref_candidates(event_ref, event_sha):
            completed = subprocess.run(
                [
                    "git", "fetch", "--no-tags", "--unshallow", remote,
                    f"+{candidate}:refs/pulp-ci/gpu-provenance/event",
                ],
                cwd=root, text=True, capture_output=True, check=False,
            )
            if completed.returncode == 0:
                break
            failures.append(f"{candidate}: {(completed.stderr or completed.stdout).strip()}")
        else:
            # Every candidate was unfetchable. That is a statement about
            # AVAILABILITY, not about the provenance: GitHub deletes
            # refs/pull/<n>/merge the moment a pull request closes, and leaves it
            # absent while it recomputes mergeability after a base-branch push, so
            # a rerun in either window fails here on a repository whose history is
            # otherwise fine. The two checks below are the actual invariants and
            # are themselves fail-closed -- a still-shallow repository cannot walk
            # past its graft, so `merge-base --is-ancestor` reports NOT-an-ancestor
            # rather than a false yes. Report the transport failure and let those
            # adjudicate.
            for failure in failures:
                print(
                    f"gpu-provenance-hydration: WARN: bounded fetch failed: {failure}",
                    file=sys.stderr,
                )
    unresolved = [revision for revision in revisions if not is_commit(root, revision)]
    if unresolved:
        raise HydrationError(f"GPU provenance commits remain unresolved: {unresolved}")
    non_ancestors = [
        revision for revision in revisions
        if subprocess.run(
            ["git", "merge-base", "--is-ancestor", revision, "HEAD"], cwd=root,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False,
        ).returncode != 0
    ]
    if non_ancestors:
        raise HydrationError(f"GPU provenance commits are not ancestors of HEAD: {non_ancestors}")
    return len(revisions), len(missing)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=pathlib.Path, default=pathlib.Path(__file__).parents[2])
    parser.add_argument("--remote", default="origin")
    parser.add_argument(
        "--verify-only",
        action="store_true",
        help="adjudicate the pinned commits against local objects and refs; never fetch",
    )
    args = parser.parse_args(argv)
    if args.verify_only:
        return command_verify(args.root.resolve())
    try:
        total, fetched = hydrate(args.root.resolve(), args.remote)
    except HydrationError as error:
        print(f"gpu-provenance-hydration: FAIL: {error}", file=sys.stderr)
        return 1
    print(f"gpu-provenance-hydration: PASS total={total} fetched={fetched} cap={MAX_COMMITS}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

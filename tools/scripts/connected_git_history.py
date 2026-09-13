#!/usr/bin/env python3
"""Tell a truncated checkout apart from a genuinely missing path.

Several GPU provenance tools ask Git which commit last owned a path::

    git log -1 --format=%H <commit> -- <path>

On a checkout whose history has been truncated (``git clone --depth=N``) that
command does not fail. It returns the *shallow graft boundary* commit — the
oldest commit the checkout has — because Git cannot see past it to the real
owning commit. Exit status is 0 and the output is a well-formed SHA, so every
caller downstream treats a boundary SHA as a real answer: the handoff ledger
gets rewritten with wrong revisions, and identity comparisons report "absent
from its pinned revision" when the path is right there on disk.

The predicate this module exports is *membership of the resolved revision in
the shallow-boundary set*, not ``git rev-parse --is-shallow-repository``. Those
are different questions and only the first one discriminates:

    checkout                is-shallow   commits   pinned rows on a boundary
    a fetched agent worktree  true          9878         0 / 102
    git clone --depth=1       true             1       102 / 102

A repository can be shallow-*marked* and still carry enough history to answer
every per-path ownership question asked of it. Keying a failure on
``--is-shallow-repository`` would turn such a checkout red for no defect, which
includes the CI runner checkouts that produce the required ``macos`` gate.
``--is-shallow-repository`` remains the right trigger for the *remedy* — see
``hydrate_gpu_provenance_commits.py`` — because unshallowing an already-deep
repository is wasted work, never a false failure.

A broken instrument is not a shallow finding. If Git is missing, the directory
is not a repository, or a query exits nonzero for any other reason, this module
reports "no boundaries" and lets the caller proceed with whatever answer it
would otherwise have reached. Reporting truncation because the measurement
itself failed would manufacture exactly the kind of misattributed diagnosis this
module exists to prevent.
"""

from __future__ import annotations

import json
import pathlib
import subprocess


GIT_TIMEOUT_SECONDS = 30

# ``require_connected_history`` is a coarse gate run at tool startup, and one
# ``git log`` per probe costs ~65ms, so resolving all 102 pinned rows would add
# ~7s to every invocation. The measured control pair is all-or-nothing — 0/102
# on a fetched worktree, 102/102 on a ``--depth=1`` clone — so a small prefix
# discriminates the shapes this gate exists to separate. A partially truncated
# checkout whose sampled rows all resolve is a missed finding here; the
# exhaustive per-row check lives in the handoff validator, which reads every row.
PROBE_LIMIT = 8

REMEDY = (
    "run `git fetch --unshallow` (or tools/scripts/hydrate_gpu_provenance_commits.py "
    "in CI), then re-run"
)


class ShallowCheckoutError(RuntimeError):
    """The checkout cannot answer per-path ownership questions."""


def _git_stdout(root: pathlib.Path, arguments: list[str]) -> str | None:
    """Return stripped stdout, or ``None`` when the query could not be run.

    ``None`` means "cannot determine" and is deliberately distinct from an empty
    string, which means "Git answered, and the answer was nothing".
    """

    try:
        completed = subprocess.run(
            ["git", *arguments],
            cwd=root,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            timeout=GIT_TIMEOUT_SECONDS,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if completed.returncode != 0:
        return None
    return completed.stdout.strip()


def shallow_boundaries(root: pathlib.Path) -> set[str]:
    """Return the graft-boundary commits this checkout's history stops at.

    The boundary list lives in the *common* Git directory, not the per-worktree
    one: linked worktrees share a single object store and therefore a single
    truncation point, and ``--git-dir`` in a worktree points at the private
    ``worktrees/<name>`` directory where no ``shallow`` file exists.

    An empty set means either "not truncated" or "could not ask", and callers
    must treat both the same way — as no shallow finding.
    """

    common = _git_stdout(root, ["rev-parse", "--git-common-dir"])
    if not common:
        return set()
    directory = pathlib.Path(common)
    if not directory.is_absolute():
        directory = pathlib.Path(root) / directory
    shallow = directory / "shallow"
    try:
        contents = shallow.read_text(encoding="utf-8")
    except OSError:
        # Missing file is the ordinary "history is complete" case; an unreadable
        # one is a broken instrument. Neither is evidence of truncation.
        return set()
    return {line.strip() for line in contents.split() if line.strip()}


def resolves_to_boundary(
    root: pathlib.Path, commit: str, path: str, boundaries: set[str]
) -> bool:
    """Is this row's last-owner query answered by the truncation point?

    True when ``git log -1 --format=%H <commit> -- <path>`` produces nothing —
    which is what a depth-1 clone does when ``commit`` is not in its object
    store — or produces one of the graft boundaries, which is what a partially
    truncated clone does when the real owner lies beyond its horizon.

    Callers must only act on this when ``boundaries`` is non-empty. On a
    complete checkout an empty answer means the path genuinely has no history at
    that commit, and calling that truncation would be a misdiagnosis.
    """

    revision = _git_stdout(root, ["log", "-1", "--format=%H", commit, "--", path])
    if revision is None:
        # Git refused to answer. That is only reported as truncation when the
        # checkout is already known to carry a graft boundary; on a complete
        # checkout a failed query means a bad argument, not a short history.
        return bool(boundaries)
    if not revision:
        return True
    return revision in boundaries


def _ledger_probes(root: pathlib.Path) -> list[tuple[str, str]]:
    """Pinned ``(revision, path)`` rows to use as ownership probes.

    The handoff ledger is this repository's canonical inventory of per-path
    ownership questions, which makes it the measured control for the predicate:
    0 of its 102 rows resolve to a boundary on a fetched agent worktree, and all
    102 do on a ``--depth=1`` clone. An unreadable ledger yields no probes and
    the caller falls back to the weaker HEAD probe below.
    """

    ledger = pathlib.Path(root) / "docs/status/gpu-vellum-handoff.yaml"
    try:
        document = json.loads(ledger.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return []
    if not isinstance(document, dict):
        return []
    probes: list[tuple[str, str]] = []
    for entry in document.get("entries") or []:
        if not isinstance(entry, dict):
            continue
        for row in entry.get("pulp_paths") or []:
            if not isinstance(row, dict):
                continue
            revision = row.get("revision")
            path = row.get("path")
            if isinstance(revision, str) and isinstance(path, str) and revision and path:
                probes.append((revision, path))
            if len(probes) >= PROBE_LIMIT:
                return probes
    return probes


def require_connected_history(
    root: pathlib.Path, what: str, probes: list[tuple[str, str]] | None = None
) -> None:
    """Raise when ``what`` cannot be answered by this checkout's history.

    The gate is the discriminating predicate, not the presence of a graft
    boundary: a shallow-*marked* checkout whose ownership questions all resolve
    to real commits can answer everything asked of it and must stay silent.

    ``probes`` are ``(commit, path)`` pairs whose last-owner resolution stands in
    for the caller's own history questions. The default set is the pinned rows of
    the GPU handoff ledger. When the ledger cannot be read, the fallback asks
    whether ``HEAD`` itself is a graft boundary — the ``--depth=1`` signature.
    That fallback is deliberately weaker: it can miss a partially truncated
    checkout, which is a missed finding rather than a manufactured one.
    """

    boundaries = shallow_boundaries(root)
    if not boundaries:
        return
    if probes is None:
        probes = _ledger_probes(root)
    if probes:
        truncated = any(
            resolves_to_boundary(root, commit, path, boundaries)
            for commit, path in probes
        )
    else:
        head = _git_stdout(root, ["rev-parse", "HEAD"])
        truncated = bool(head) and head in boundaries
    if truncated:
        raise ShallowCheckoutError(
            f"{what} needs connected Git history, but this checkout's history is "
            f"truncated; {REMEDY}"
        )

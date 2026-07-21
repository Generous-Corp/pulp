#!/usr/bin/env python3
"""version_at_land.py — single-writer version assignment on main (T1.1).

The problem: every `fix:`/`feat:` PR must hand-write a version number ABOVE
main's current value into `CMakeLists.txt` / `.claude-plugin/*.json`. That is a
shared counter — only one PR can own `main+1` — so N parallel PRs endlessly
re-bump and re-conflict on the VERSION line (the biggest single source of
merge-land thrash; see planning/2026-07-07-parallel-merge-land-coordination.md).

The fix: PRs stop hand-bumping. This bot — the single writer, running post-merge
on `main` — assigns the explicit semver once, in commit order, when it is the
sole writer. No two PRs ever contend for the same number.

To assign the RIGHT number the bot must reproduce exactly what the hand-bump
model wrote. That model does NOT read positive `Version-Bump:` trailers as its
signal — those are `skip`/override escapes used only a handful of times. It
derives the level from a PATH + CONVENTIONAL-COMMIT heuristic
(`version_bump_heuristics.assess_surfaces`: public-API paths → minor, other
touched source → patch, `feat:` → minor, `fix:`/`perf:` → patch, `BREAKING`/`!`
→ major), honoring `Version-Bump: <surface>=skip` and explicit
`<surface>=<level>` trailers as overrides. So this bot calls the SAME
`assess_surfaces` the gate and the `--mode=apply` writer call — there is one
heuristic, imported, never duplicated — and bumps each surface's base version by
the level it returns.

Rollout is staged and this script is safe at every stage:
  --mode dry-run (default): compute and print what it WOULD assign; write
    nothing. Run this while PRs still hand-bump — it changes nothing about
    releases.
  --mode apply: write the configured version files in place (the historical
    "caller commits" shape, kept for callers that stage the commit themselves).
  --push: the hardened single-writer transaction — recompute from a fresh
    `origin/main`, write the files, commit with a `Version-Bump-Applied:`
    marker, and push with `--ff-only`, retrying (each retry recomputes from the
    new origin tip) so two near-simultaneous drains never lose or duplicate a
    version. This is what the workflow uses once flipped off dry-run.

It reuses the existing version machinery (versioning.json surfaces, the
`assess_surfaces` heuristic, version_at_base, write_version, bump_version) so
there is ONE source of truth for what a version file is, how a level is derived,
and how a level maps to a number.
"""

from __future__ import annotations

import argparse
import contextlib
import json
import os
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterator

from gate_common import git_diff_names
from version_bump_surfaces import Config, Surface, load_config, version_at_base, write_version
from version_bump_heuristics import assess_surfaces, filter_generated
from version_bump_apply import bump_version

# The next drain starts after the commit carrying this marker, so an applied
# bump is never re-assigned.
APPLIED_MARKER = "Version-Bump-Applied"
BUMP_SUBJECT = "chore: bump versions"

# The dedicated branch the PR-route bumper (`--route pr`) pushes the bump to.
# A FIXED name (not per-run) is deliberate: it is the concurrency lock. Two
# near-simultaneous drains both target this one branch, and GitHub permits only
# one open PR per head branch, so the second drain's `pr create` fails and the
# run reports "pending" instead of opening a rival bump PR (the debounce
# concern). The bot solely owns this branch, so a force-push is safe.
BUMP_BRANCH = "release/version-bump"


@dataclass(frozen=True)
class Assignment:
    surface: str
    level: str
    current: str
    assigned: str


def _current_at_base(base: str, surface: Surface) -> str | None:
    """The surface's version at `base` — the value the assignment bumps FROM.

    Reads the drain-window base (the last processed commit), NOT HEAD. Reading
    HEAD would be self-referential: any version-file edit already inside the
    range (a hand-bump during the transition, or the bot's own prior partial
    apply) would be read back as the "current" value and bumped a second time.
    The base is the last state the bot already accounted for, so bumping from it
    is idempotent. Mirrors `version_bump_apply.apply_bumps`, which computes its
    target from `version_at_base` for the same reason.
    """
    for vf in surface.version_files:
        cur = version_at_base(base, vf)
        if cur:
            return cur
    return None


def plan_assignments(config: Config, changed: list[str], base: str, head: str,
                     repo: Path) -> list[Assignment]:
    """For each surface the heuristic says needs a bump, what version to assign.

    Delegates the level decision to `assess_surfaces` — the identical pipeline
    the version-bump gate and the `--mode=apply` writer use — so the bot's
    assignment cannot drift from what a hand-bump would have written. A surface
    whose `final_level` is `none` (no meaningful touched source, or an explicit
    `Version-Bump: <surface>=skip`) gets no assignment.
    """
    verdicts = assess_surfaces(config, changed, base, head, repo)
    out: list[Assignment] = []
    for v in verdicts:
        if v.final_level == "none":
            continue
        # Bump FROM the base version, not the HEAD read that assess_surfaces
        # stored on the verdict (see _current_at_base). Fall back to the HEAD
        # read only when the file did not exist at base.
        current = _current_at_base(base, v.surface)
        if current is None:
            current = v.current_version
        if not current:
            continue
        out.append(Assignment(v.surface.name, v.final_level, current,
                              bump_version(current, v.final_level)))
    return out


# ── Git helpers ────────────────────────────────────────────────────────────

def _git(repo: Path, *args: str, check: bool = True) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["git", "-C", str(repo), *args],
        check=check, capture_output=True, text=True,
    )


def _rev_parse(repo: Path, ref: str) -> str:
    return _git(repo, "rev-parse", ref).stdout.strip()


@contextlib.contextmanager
def _in_repo(repo: Path) -> Iterator[None]:
    """Run the block with the process cwd set to `repo`, restoring it after.

    The level derivation (`git_diff_names`, `assess_surfaces`, `version_at_base`)
    runs git in the process cwd — correct for the CLI, whose cwd is the repo,
    but `apply_and_push` drives an arbitrary clone dir, so the plan must be
    computed there. Scoped narrowly around plan computation only."""
    prev = os.getcwd()
    os.chdir(repo)
    try:
        yield
    finally:
        os.chdir(prev)


def last_applied_marker(repo: Path, ref: str) -> str | None:
    """Newest commit reachable from `ref` carrying a `Version-Bump-Applied`
    marker — the last state this bot already processed. None if none exists."""
    # Anchor to the trailer form (`Version-Bump-Applied:` at line start) so a
    # stray mention of the string in some other commit body is not mistaken for
    # a marker.
    out = _git(repo, "log", "-E", "--grep", f"^{APPLIED_MARKER}:",
               "-1", "--format=%H", ref, check=False)
    sha = out.stdout.strip()
    return sha or None


def drain_base(repo: Path, head: str, fallback: str | None = None) -> str:
    """The exclusive start of the drain range: the last applied marker if one
    exists (the authoritative "already processed up to here"); otherwise the
    caller's `fallback` (e.g. a push event's `before` SHA) when it is a valid,
    reachable ancestor; otherwise the commit before `head`. Bounded, non-empty."""
    marker = last_applied_marker(repo, head)
    if marker:
        return marker
    if fallback:
        ok = _git(repo, "merge-base", "--is-ancestor", fallback, head, check=False)
        if ok.returncode == 0:
            return fallback
    return f"{head}~1"


def plan_for_range(repo: Path, config: Config, base: str, head: str) -> list[Assignment]:
    """Concrete plan over a drain window: the changed-file set across
    `base..head` (three-dot, generated files filtered) fed to the live
    `assess_surfaces` heuristic, bumping each surface FROM its version at
    `base` (see `plan_assignments` / `_current_at_base`).

    The drain range's `base` is supplied by `drain_base` (the last
    `Version-Bump-Applied` marker, or the caller's fallback), so the window is
    exactly what this bot has not yet accounted for."""
    changed = filter_generated(git_diff_names(base, head), config.generated_globs)
    return plan_assignments(config, changed, base, head, repo)


def _write_plan(repo: Path, config: Config, plan: list[Assignment]) -> list[str]:
    surfaces_by_name: dict[str, Surface] = {s.name: s for s in config.surfaces}
    edited: list[str] = []
    for a in plan:
        for vf in surfaces_by_name[a.surface].version_files:
            if write_version(repo, vf, a.assigned):
                edited.append(vf.path)
    return edited


# ── Hardened single-writer push transaction ─────────────────────────────────

def apply_and_push(
    repo: Path,
    config: Config,
    remote: str = "origin",
    branch: str = "main",
    max_retries: int = 3,
    fallback_base: str | None = None,
    on_before_push: Callable[[int], None] | None = None,
) -> tuple[str, list[Assignment]]:
    """Recompute from a fresh `remote/branch`, write + commit the bump with a
    `Version-Bump-Applied` marker, and push `--ff-only`; retry on rejection.

    Returns `(status, plan)` where status is one of:
      "noop"    — no intent in the drained range; nothing pushed.
      "applied" — a bump commit was pushed to `remote/branch`.
      "exhausted" — every attempt lost the `--ff-only` race (should not happen
                    in practice; surfaced so the caller can fail loudly).

    Correctness under concurrency rests on three things:
      1. `--ff-only` — a second writer that advanced the branch between our
         recompute and our push is rejected, never silently clobbered (the
         `git push origin HEAD:main` bug in the deleted intent-bump workflow).
      2. recompute-per-attempt — each retry re-syncs to the new tip, so the
         drain range now starts AFTER the winner's marker and collapses to
         empty → we no-op instead of double-bumping.
      3. the `Version-Bump-Applied` marker — the next drain (and our own reruns)
         start after it, so an already-assigned range is never reprocessed.

    `on_before_push` is a test seam (advance the remote to force a non-ff race);
    production leaves it None.
    """
    for attempt in range(max_retries + 1):
        _git(repo, "fetch", "--quiet", remote, branch)
        head = _rev_parse(repo, f"{remote}/{branch}")
        # Hard-sync the working tree to the fetched tip so read_version sees the
        # authoritative current numbers. Ephemeral CI checkout — safe to reset.
        _git(repo, "reset", "--hard", head)

        base = drain_base(repo, head, fallback_base)
        # plan_for_range's level derivation runs git in the process cwd; drive
        # it in the clone dir this transaction owns.
        with _in_repo(repo):
            plan = plan_for_range(repo, config, base, head)
        if not plan:
            return "noop", []

        edited = _write_plan(repo, config, plan)
        if not edited:
            # Intent declared but files already at/above target (a prior run
            # committed but the marker walk missed it). Treat as done, not a
            # spurious empty commit.
            return "noop", plan

        # Stage ONLY the version files we edited (never `git add -A` — this repo
        # carries a submodule gitlink that a blanket add would re-stage).
        _git(repo, "add", "--", *dict.fromkeys(edited))
        summary = "; ".join(f"{a.surface} {a.current}->{a.assigned} ({a.level})"
                            for a in plan)
        message = (
            f"{BUMP_SUBJECT}\n\n"
            f"Assigned from Version-Bump intent on {base[:12]}..{head[:12]}.\n"
            f"{summary}\n\n"
            f"{APPLIED_MARKER}: {head}\n"
        )
        _git(repo, "commit", "--no-verify", "-m", message)

        if on_before_push is not None:
            on_before_push(attempt)

        # No `--force`: `git push` refuses a non-fast-forward update by
        # default, which IS the `--ff-only` guarantee — a racing writer that
        # advanced `branch` after our recompute is rejected here, never
        # clobbered.
        pushed = _git(repo, "push", "--porcelain",
                      remote, f"HEAD:{branch}", check=False)
        if pushed.returncode == 0:
            return "applied", plan

        # Surface git's real rejection. Without this the caller only prints the
        # generic "branch kept moving" summary, so a NON-race rejection — a
        # branch ruleset (e.g. GH006 "Changes must be made through the merge
        # queue"), a permission/auth failure, a hook — is indistinguishable from
        # a genuine fast-forward race and effectively invisible. The retry loop
        # below still treats every failure as a lost race; this only makes the
        # cause diagnosable in the run log.
        detail = (pushed.stderr or pushed.stdout or "").strip()
        sys.stderr.write(
            f"version-at-land: push attempt {attempt} rejected "
            f"(exit {pushed.returncode}){':' if detail else '.'}\n")
        if detail:
            sys.stderr.write(f"{detail}\n")

        # Non-fast-forward (someone else advanced the branch) — discard our
        # local bump commit and retry from the new tip.
        _git(repo, "reset", "--hard", head)
    return "exhausted", []


# ── PR-route transaction (merge-queue compatible) ───────────────────────────

def _gh(repo: Path, *args: str, check: bool = True) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["gh", *args],
        cwd=str(repo), check=check, capture_output=True, text=True,
    )


def _bump_message(plan: list[Assignment], base: str, head: str) -> str:
    summary = "; ".join(f"{a.surface} {a.current}->{a.assigned} ({a.level})"
                        for a in plan)
    return (
        f"{BUMP_SUBJECT}\n\n"
        f"Assigned from Version-Bump intent on {base[:12]}..{head[:12]}.\n"
        f"{summary}\n\n"
        f"{APPLIED_MARKER}: {head}\n"
    )


def _semver(v: str) -> tuple[int, ...]:
    return tuple(int(x) for x in v.split(".") if x.isdigit())


def _strictly_increasing(repo: Path, config: Config, plan: list[Assignment],
                         head: str) -> list[Assignment]:
    """Keep only assignments that STRICTLY raise the surface's version at `head`.
    `write_version` writes its target unconditionally, so a plan computed from a
    stale base (a slow or racing drain) could otherwise write a number <= what
    already landed — a version REGRESSION, not a no-op. Comparing each
    assignment against the fresh-head value drops any already-satisfied surface,
    so a stale replay collapses to nothing instead of walking a version
    backward. This is the real double-bump / regression guard (independent of
    whether the `Version-Bump-Applied` marker survives a merge)."""
    surfaces = {s.name: s for s in config.surfaces}
    kept: list[Assignment] = []
    with _in_repo(repo):
        for a in plan:
            cur = _current_at_base(head, surfaces[a.surface])
            if cur is None:
                # No readable version file for this surface at head — cannot
                # PROVE the assignment advances it. Fail closed (drop it) rather
                # than fall back to the stale base value and keep a phantom
                # assignment that _write_plan might then no-op.
                sys.stderr.write(
                    f"version-at-land: skip {a.surface} — no readable version at "
                    f"head to compare against (fail-closed).\n")
                continue
            if _semver(a.assigned) > _semver(cur):
                kept.append(a)
            else:
                sys.stderr.write(
                    f"version-at-land: skip {a.surface} — assigned {a.assigned} "
                    f"does not exceed current {cur} at head (stale drain).\n")
    return kept


def _list_open_bump_pr(repo: Path) -> tuple[bool, str | None]:
    """`(ok, number)` for the open PR on `BUMP_BRANCH`.

      ok=True,  number="N"  — a confirmed open bump PR.
      ok=True,  number=None — confirmed NO open bump PR.
      ok=False, number=None — the query FAILED (unknown state).

    The tri-state is load-bearing: a force-reclaim must never run on an UNKNOWN
    PR state (an API error must not be read as "no PR" and clobber a queued
    PR)."""
    res = _gh(repo, "pr", "list", "--head", BUMP_BRANCH, "--state", "open",
              "--json", "number", check=False)
    if res.returncode != 0:
        return False, None
    try:
        prs = json.loads(res.stdout or "[]")
    except json.JSONDecodeError:
        return False, None
    return True, (str(prs[0]["number"]) if prs else None)


def _remote_sha(repo: Path, remote: str, ref: str) -> str | None:
    """The SHA `remote` currently has for `ref`, or None if absent/unresolved.
    Used as the EXPLICIT `--force-with-lease` expectation so a reclaim is a real
    compare-and-swap against the exact state we decided was abandoned."""
    res = _git(repo, "ls-remote", remote, ref, check=False)
    if res.returncode != 0 or not res.stdout.strip():
        return None
    return res.stdout.split()[0].strip()


def _is_non_ff_rejection(detail: str) -> bool:
    """True ONLY for a genuine non-fast-forward rejection (the branch diverged)
    — the one case a reclaim is even considered. Match the NFF-specific signals
    (`non-fast-forward` / `fetch first`), NOT the generic `failed to push some
    refs` or a bare `[rejected]`: a pre-receive-hook / ruleset / `[remote
    rejected]` failure also emits those, and must fail loudly instead of
    entering the reclaim path."""
    d = detail.lower()
    return "non-fast-forward" in d or "fetch first" in d


def _arm_auto_merge(repo: Path, ref: str) -> bool:
    """Arm GitHub-native auto-merge with `--merge` (NEVER `--squash`: the repo
    contract, .agents/contract.toml, requires `--merge` so the bump-marker
    commit is preserved and does not trip an auto-release false alarm).
    Idempotent — re-arming an already-armed PR is a safe no-op, so any drain
    that finds an open-but-unarmed bump PR self-heals it. Returns success."""
    res = _gh(repo, "pr", "merge", ref, "--auto", "--merge", check=False)
    if res.returncode != 0:
        sys.stderr.write(f"version-at-land: arming auto-merge on {ref} failed "
                         f"(exit {res.returncode}).\n{res.stderr or ''}\n")
        return False
    return True


def apply_via_pr(
    repo: Path,
    config: Config,
    remote: str = "origin",
    branch: str = "main",
    fallback_base: str | None = None,
) -> tuple[str, list[Assignment]]:
    """PR-route bumper: build the bump on `BUMP_BRANCH`, open a `chore: bump
    versions` PR, and arm auto-merge (`--merge`) so it lands THROUGH the merge
    queue when one is enabled (and as a normal auto-merge PR when it is not — so
    this route is safe to enable before the queue). Returns `(status, plan)`:

      "noop"       — nothing to assign (no intent, or every surface already at/
                     above target at the fresh head).
      "pending"    — an open bump PR already exists; this run RE-ARMS its
                     auto-merge (healing a prior failed arm) and defers instead
                     of opening a rival PR.
      "pr-opened"  — a bump PR was pushed, opened, and armed.
      "arm-failed" — the PR is open but auto-merge could not be armed; the run
                     fails (visible + retried) and the next drain re-arms it via
                     the "pending" path, so there is no permanent wedge.

    Safety:
      * Regression — `_strictly_increasing` drops any assignment not exceeding
        the fresh-head version (fail-closed if that version is unreadable), so a
        stale / racing drain never writes a number <= what already landed.
      * Concurrency — the workflow SERIALIZES all PR-route drains (a single
        constant `concurrency` group when PULP_BUMP_ROUTE=pr), so no two drains
        run at once and the reclaim has no competitor. The in-function guards
        are defense-in-depth for that invariant: plain (non-force) push first
        (never clobbers); a divergent branch is reclaimed ONLY on a CONFIRMED
        "no open PR" (an errored/unknown lookup fails closed), and even then via
        an EXPLICIT `--force-with-lease=<branch>:<observed-sha>` compare-and-swap.
        A live queued PR's head is never overwritten.
      * Wedge-free — a failed auto-merge arm (initial OR re-arm) returns
        "arm-failed" (red, retried), never a green defer; any later drain
        re-arms an open-but-unarmed PR.
      * Double-bump — independent of marker survival: a re-drained landed range
        yields an empty `_strictly_increasing` plan → noop.
    """
    _git(repo, "fetch", "--quiet", remote, branch)
    head = _rev_parse(repo, f"{remote}/{branch}")
    _git(repo, "reset", "--hard", head)

    base = drain_base(repo, head, fallback_base)
    with _in_repo(repo):
        plan = plan_for_range(repo, config, base, head)
    plan = _strictly_increasing(repo, config, plan, head)
    if not plan:
        return "noop", []

    # One open bump PR at a time (the debounce lock). If one exists, RE-ARM its
    # auto-merge (heals a prior failed arm) and defer rather than opening a rival.
    # A failed re-arm surfaces as "arm-failed" (red run) — never a green defer,
    # or a persistently-unarmable PR would wedge the drain silently.
    ok, existing = _list_open_bump_pr(repo)
    if ok and existing is not None:
        return ("pending" if _arm_auto_merge(repo, existing) else "arm-failed"), plan

    edited = _write_plan(repo, config, plan)
    if not edited:
        return "noop", plan

    # Stage ONLY the edited version files (never `git add -A` — a blanket add
    # re-stages this repo's submodule gitlink).
    _git(repo, "add", "--", *dict.fromkeys(edited))
    _git(repo, "commit", "--no-verify", "-m", _bump_message(plan, base, head))

    # Plain push first — creates the branch or fast-forwards it. A NON-force
    # push cannot clobber anyone.
    pushed = _git(repo, "push", "--porcelain",
                  remote, f"HEAD:{BUMP_BRANCH}", check=False)
    if pushed.returncode != 0:
        # Combine both streams — porcelain writes the ref status to stdout while
        # the "Updates were rejected" hint goes to stderr.
        detail = f"{pushed.stderr or ''}\n{pushed.stdout or ''}".strip()
        # Only a non-fast-forward rejection means the branch diverged. Auth /
        # hook / ruleset / network failures are NOT concurrent-drain losses.
        if not _is_non_ff_rejection(detail):
            sys.stderr.write(f"version-at-land: push to {BUMP_BRANCH} failed — "
                             f"not a fast-forward rejection (exit "
                             f"{pushed.returncode}).\n{detail}\n")
            return "exhausted", plan
        # Divergent branch. Capture the EXACT current remote SHA NOW, before the
        # PR check, so the reclaim lease is a compare-and-swap against precisely
        # this state — a competitor that pushes afterward moves the ref and the
        # explicit lease rejects (an implicit lease would be defeated by a fetch
        # adopting the competitor's head; see git-push docs).
        observed = _remote_sha(repo, remote, BUMP_BRANCH)
        ok, other = _list_open_bump_pr(repo)
        if not ok:
            # UNKNOWN PR state — never force over a possibly-queued PR head.
            sys.stderr.write("version-at-land: cannot confirm bump-PR state; "
                             "refusing to reclaim the branch.\n")
            return "exhausted", plan
        if other is not None:
            # A live queued PR owns the branch — re-arm it, never clobber.
            return ("pending" if _arm_auto_merge(repo, other)
                    else "arm-failed"), plan
        if observed is None:
            sys.stderr.write(f"version-at-land: {BUMP_BRANCH} diverged but its "
                             "SHA could not be read; refusing to reclaim.\n")
            return "exhausted", plan
        # Confirmed no PR → stale abandoned branch. Reclaim with an EXPLICIT
        # lease at the observed SHA (a true CAS, not the fetch-defeatable
        # implicit form).
        reclaimed = _git(repo, "push",
                         f"--force-with-lease={BUMP_BRANCH}:{observed}",
                         "--porcelain", remote, f"HEAD:{BUMP_BRANCH}", check=False)
        if reclaimed.returncode != 0:
            rdetail = (reclaimed.stderr or reclaimed.stdout or "").strip()
            sys.stderr.write(f"version-at-land: reclaim of {BUMP_BRANCH} rejected "
                             f"(exit {reclaimed.returncode}) — the branch moved "
                             f"under us; a concurrent drain won.\n{rdetail}\n")
            ok2, other2 = _list_open_bump_pr(repo)
            if ok2 and other2 is not None:
                return ("pending" if _arm_auto_merge(repo, other2)
                        else "arm-failed"), plan
            return "exhausted", plan

    created = _gh(repo, "pr", "create", "--base", branch, "--head", BUMP_BRANCH,
                  "--title", BUMP_SUBJECT,
                  "--body", _bump_message(plan, base, head), check=False)
    if created.returncode != 0:
        stderr = created.stderr or ""
        # A concurrent drain opened the PR between our checks — GitHub rejects a
        # second open PR from the same head. Re-arm theirs and defer; a failed
        # re-arm surfaces as "arm-failed", never a green defer.
        if "already exists" in stderr or "a pull request for branch" in stderr:
            ok, other = _list_open_bump_pr(repo)
            if ok and other is not None:
                return ("pending" if _arm_auto_merge(repo, other)
                        else "arm-failed"), plan
            # `create` says a PR exists but the lookup errored or found none —
            # an inconsistent/unknown state. Fail closed (red), do NOT report a
            # green "pending" for a PR we could not confirm or arm.
            sys.stderr.write("version-at-land: pr create reported an existing PR "
                             "but it could not be confirmed/armed.\n")
            return "exhausted", plan
        sys.stderr.write(f"version-at-land: pr create failed "
                         f"(exit {created.returncode}).\n{stderr}\n")
        return "exhausted", plan

    if not _arm_auto_merge(repo, BUMP_BRANCH):
        # PR is open but unarmed — fail loudly so the run is red + retried; the
        # next drain re-arms it via the "pending" path (no permanent wedge).
        return "arm-failed", plan
    return "pr-opened", plan


# ── CLI ──────────────────────────────────────────────────────────────────────

def _repo_root() -> Path:
    return Path(subprocess.run(["git", "rev-parse", "--show-toplevel"],
                               check=True, capture_output=True, text=True).stdout.strip())


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--base",
                    help="Start of the drain range (exclusive). Required in "
                         "--mode dry-run/apply. In --push it is only a FALLBACK "
                         "base (e.g. a push event's `before` SHA) used when the "
                         "bot has never run and no Version-Bump-Applied marker "
                         "exists yet; the marker takes precedence otherwise.")
    ap.add_argument("--head", default="HEAD", help="End of the range (default: HEAD).")
    ap.add_argument("--config", default="tools/scripts/versioning.json")
    ap.add_argument("--mode", choices=["dry-run", "apply"], default="dry-run",
                    help="dry-run (default): print, write nothing. apply: write "
                         "version files in place (caller commits).")
    ap.add_argument("--push", action="store_true",
                    help="Hardened single-writer transaction: recompute from a "
                         "fresh origin, write + commit with a Version-Bump-Applied "
                         "marker, and push --ff-only with retry. Ignores --base/"
                         "--head/--mode (derives the range from the remote).")
    ap.add_argument("--route", choices=["direct", "pr"], default="direct",
                    help="With --push, how the bump lands. 'direct' (default): "
                         "push --ff-only straight to the branch — today's "
                         "behavior, incompatible with a 'Require merge queue' "
                         "rule. 'pr': open a bump PR on a dedicated branch and "
                         "auto-merge it (routes through the merge queue when one "
                         "is enabled). The workflow selects this via the "
                         "PULP_BUMP_ROUTE repo variable; unset ⇒ direct ⇒ no "
                         "behavior change.")
    ap.add_argument("--remote", default="origin")
    ap.add_argument("--branch", default="main")
    ap.add_argument("--max-retries", type=int, default=3)
    ap.add_argument("--json", action="store_true", help="Emit the plan as JSON.")
    args = ap.parse_args(argv)

    repo = _repo_root()
    config_path = Path(args.config)
    if not config_path.is_absolute():
        config_path = repo / config_path
    config = load_config(config_path)

    if args.push:
        if args.route == "pr":
            status, plan = apply_via_pr(
                repo, config, remote=args.remote, branch=args.branch,
                fallback_base=args.base,
            )
        else:
            status, plan = apply_and_push(
                repo, config, remote=args.remote, branch=args.branch,
                max_retries=args.max_retries, fallback_base=args.base,
            )
        if args.json:
            print(json.dumps({"status": status,
                              "plan": [a.__dict__ for a in plan]}, indent=2))
        elif status == "noop":
            print("version-at-land: no version intent in range — nothing to push.")
        elif status == "pending":
            print("version-at-land: a bump PR is already open — deferring to it.")
        elif status in ("applied", "pr-opened"):
            verb = "pushed" if status == "applied" else "opened bump PR for"
            for a in plan:
                print(f"version-at-land: {a.surface} {verb} {a.current} -> "
                      f"{a.assigned} ({a.level})")
        elif status == "arm-failed":
            sys.stderr.write(
                "version-at-land: bump PR opened but auto-merge could not be "
                "armed. Failing so it retries; the next drain re-arms it.\n")
            return 1
        else:  # "exhausted"
            sys.stderr.write(
                "version-at-land: could not land the bump "
                f"(status={status}). See the error above. Re-run.\n")
            return 1
        return 0

    if not args.base:
        ap.error("--base is required unless --push is given")

    # Derive the level from the live path + conventional-commit heuristic over
    # the drain range (not positive intent trailers), bumping FROM each
    # surface's version at `base`.
    plan = plan_for_range(repo, config, args.base, args.head)

    if args.mode == "apply":
        _write_plan(repo, config, plan)

    if args.json:
        print(json.dumps([a.__dict__ for a in plan], indent=2))
    elif not plan:
        print("version-at-land: no version intent in range — nothing to assign.")
    else:
        verb = "would assign" if args.mode == "dry-run" else "assigned"
        for a in plan:
            print(f"version-at-land: {a.surface} {verb} {a.current} -> {a.assigned} "
                  f"({a.level})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

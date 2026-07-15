#!/usr/bin/env python3
"""Advise when several related branches could ship as ONE PR.

A PR here costs ~an hour before it can merge: a ~25-minute local diff-coverage
build, then a Shipyard validation that must run SERIALLY with any other. So `n`
related PRs is `n` sequential hours, not `n` parallel ones. Folding siblings into
one PR often also RAISES diff coverage, because the gate is computed over the
whole PR diff and a densely-tested branch lifts a weakly-tested one.

Runs from the pre-push hook — the only choke point EVERY route to a PR passes
through (`shipyard pr`, `pulp pr`, `gh pr create`, legacy local-ci, a human,
Claude, or Codex). A skill only reaches an agent that chooses to read it; a push
reaches everyone.

ALWAYS ADVISORY: exits 0 unconditionally. Combining is usually right, not always
— an urgent fix must not be held hostage to a gated branch, and an unfinished
branch must never be folded in to save a CI run. This only makes sure the
question gets ASKED. Heuristics: .agents/skills/pr-batching/SKILL.md

DESIGN NOTE — this must stay CHEAP and QUIET. A working checkout here can carry
500+ local branches, nearly all stale. So:
  * prefilter by recency with ONE `for-each-ref` before any per-branch work;
  * only then do the (more expensive) relatedness tests, on a handful;
  * report at most MAX_REPORT, and say little.
A noisy advisor is worse than none: it trains everyone to ignore it.
"""

from __future__ import annotations

import os
import subprocess
import sys

BASE = "origin/main"
SKILL = ".agents/skills/pr-batching/SKILL.md"

RECENT_DAYS = 14   # a branch older than this is not live work
MAX_CANDIDATES = 12  # hard cap on how many we even examine
MAX_REPORT = 5     # ...and how many we mention

# Subsystems that carry NO relatedness signal, because the repo's own gates force
# nearly every PR to touch them: the version-bump gate rewrites .claude-plugin/*
# and the skill-sync gate makes most changes update a SKILL.md. Two branches
# "sharing" these have nothing whatsoever in common, and treating them as related
# is what turns this advisor into noise everyone learns to ignore.
NOISE_SUBSYSTEMS = {
    ".claude-plugin",
    ".agents/skills",
}


def git(*args: str) -> str:
    try:
        return subprocess.run(
            ["git", *args], capture_output=True, text=True, check=True
        ).stdout.strip()
    except subprocess.CalledProcessError:
        return ""


def ok(*args: str) -> bool:
    return subprocess.run(["git", *args], capture_output=True).returncode == 0


def recent_branches() -> list[str]:
    """Live branches only, newest first. ONE git call, no per-branch cost."""
    out = git(
        "for-each-ref",
        "--sort=-committerdate",
        f"--format=%(refname:short)%09%(committerdate:unix)",
        "refs/heads/",
    )
    import time

    cutoff = time.time() - RECENT_DAYS * 86400
    live = []
    for line in out.splitlines():
        name, _, ts = line.partition("\t")
        if not ts.isdigit():
            continue
        if int(ts) < cutoff:
            break  # sorted newest-first: everything after this is older too
        if name and name != "main":
            live.append(name)
    return live


def commits_ahead(ref: str) -> int:
    n = git("rev-list", "--count", f"{BASE}..{ref}")
    return int(n) if n.isdigit() else 0


def subsystems(ref: str) -> set[str]:
    files = git("diff", "--name-only", f"{BASE}...{ref}").splitlines()
    dirs = set()
    for f in files:
        if f.count("/") < 1:
            continue  # a root file (CMakeLists.txt) names no subsystem
        # Prefix match, not exact: the noise entries are DIRECTORIES, and a
        # two-component key like ".claude-plugin/marketplace.json" would sail
        # straight past an exact-match filter on ".claude-plugin".
        if any(f == n or f.startswith(n + "/") for n in NOISE_SUBSYSTEMS):
            continue
        dirs.add("/".join(f.split("/")[:2]))
    return dirs


def shares_unmerged_work(a: str, b: str) -> bool:
    """True if a and b share commits that are NOT yet in origin/main.

    This is the test that actually means 'sibling'. Comparing merge-bases
    directly does NOT: two branches cut from different points of an old main
    share an ancient merge-base, which is an ANCESTOR of origin/main — related to
    nothing. What matters is whether their common point is AHEAD of the base.
    """
    mb = git("merge-base", a, b)
    if not mb:
        return False
    n = git("rev-list", "--count", f"{BASE}..{mb}")
    return n.isdigit() and int(n) > 0


def main() -> int:
    if os.environ.get("PULP_SKIP_PR_BATCH_ADVICE") == "1":
        return 0

    current = git("rev-parse", "--abbrev-ref", "HEAD")
    if not current or current in ("HEAD", "main"):
        return 0
    if not ok("rev-parse", "--verify", "--quiet", BASE):
        return 0
    if commits_ahead(current) == 0:
        return 0

    live = [b for b in recent_branches() if b != current][:MAX_CANDIDATES]
    if not live:
        return 0

    here = subsystems(current)

    related = []
    for b in live:
        if commits_ahead(b) == 0:
            continue

        contained = ok("merge-base", "--is-ancestor", b, current)
        sibling = shares_unmerged_work(current, b)
        overlap = here & subsystems(b)

        # Related enough to be worth folding in only if they share unmerged work
        # (siblings — they fold with a cherry-pick) or touch the same subsystem.
        # Unrelated subsystems should stay apart even when batching is tempting.
        if contained or sibling or overlap:
            related.append((b, commits_ahead(b), contained, sibling, sorted(overlap)))

    if not related:
        return 0

    # Strongest signal first. A sibling folds with a cherry-pick and is nearly
    # always worth batching; a bare subsystem overlap is a much weaker hint, and
    # burying the real candidates under weak ones is how an advisor gets ignored.
    related.sort(key=lambda r: (not (r[2] or r[3]), -r[1]))

    e = sys.stderr
    print("", file=e)
    print("[pre-push] pr-batching — these branches look shippable together:", file=e)
    for b, n, contained, sibling, overlap in related[:MAX_REPORT]:
        if contained:
            why = "already contained in this branch"
        elif sibling:
            why = "sibling (shares unmerged work — folds with a cherry-pick)"
        else:
            why = "same subsystem: " + ", ".join(overlap[:2])
        print(f"[pre-push]   {b} ({n} commit{'s' if n != 1 else ''}) — {why}", file=e)
    if len(related) > MAX_REPORT:
        print(f"[pre-push]   …and {len(related) - MAX_REPORT} more", file=e)

    print("", file=e)
    print(
        "[pre-push]   One PR instead of several saves a ~25-min coverage build and a",
        file=e,
    )
    print(
        "[pre-push]   serial Shipyard validation EACH — and a well-tested branch can lift",
        file=e,
    )
    print("[pre-push]   a weak one over the 75% diff-coverage gate.", file=e)
    print(
        "[pre-push]   Do NOT combine an unfinished branch, an urgent one that would be",
        file=e,
    )
    print("[pre-push]   held hostage, or one that may need reverting alone.", file=e)
    print(f"[pre-push]   {SKILL} · silence: PULP_SKIP_PR_BATCH_ADVICE=1", file=e)
    print("", file=e)

    return 0  # ALWAYS advisory — never blocks a push


if __name__ == "__main__":
    sys.exit(main())

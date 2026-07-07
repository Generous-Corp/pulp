#!/usr/bin/env python3
"""pr_check_triage.py — is a red PR check YOUR fault, or already red on main?

When a PR check fails, the first question is always "did my change break this,
or is it pre-existing on main?" Answering it by hand (open the run, read the
log, cross-check main) is slow — it cost ~30 min chasing a `from_chars`
sanitizer failure that turned out to be a known-broken main lane, already fixed
elsewhere, and not even a required check.

This tool answers it mechanically. For each non-green check on a PR it prints:
  - whether the check gates the merge (REQUIRED) or is advisory,
  - main HEAD's conclusion for the same check, and
  - a verdict: PRE-EXISTING (also red / not run on main — not your change),
    REGRESSED (green on main, red here — look at it), or NEW (PR-only check).

The comparison logic is a pure function (`triage`) so it is unit-tested without
GitHub; the CLI is a thin `gh api` wrapper.

See planning/2026-07-07-parallel-merge-land-coordination.md (T0.3).

Usage:
    python3 tools/scripts/pr_check_triage.py <pr-number> [--repo owner/name]
    python3 tools/scripts/pr_check_triage.py <pr-number> --gh ghapp   # App token
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from dataclasses import dataclass

# Conclusions we treat as "not a problem" (a check in these states is green
# enough to ignore for triage).
OK = {"SUCCESS", "NEUTRAL", "SKIPPED", None, ""}
RED = {"FAILURE", "ERROR", "CANCELLED", "TIMED_OUT", "ACTION_REQUIRED", "STALE"}


@dataclass(frozen=True)
class TriageRow:
    name: str
    required: bool
    pr_state: str
    main_state: str
    verdict: str  # PRE-EXISTING | REGRESSED | NEW | PENDING


def _norm(state: str | None) -> str:
    return (state or "").upper()


def triage(pr_checks: dict[str, str | None],
           main_checks: dict[str, str | None],
           required: set[str]) -> list[TriageRow]:
    """Classify each non-green PR check against main. Pure — no I/O.

    pr_checks / main_checks: {check-name -> conclusion-or-status}.
    required: names that gate the merge (branch-protection required contexts).
    """
    rows: list[TriageRow] = []
    for name, pr_state in sorted(pr_checks.items()):
        st = _norm(pr_state)
        if st in {_norm(x) for x in OK}:
            continue  # green — nothing to triage
        req = name in required
        if st in {"QUEUED", "IN_PROGRESS", "PENDING", "WAITING", "REQUESTED"}:
            rows.append(TriageRow(name, req, st, _norm(main_checks.get(name)), "PENDING"))
            continue
        if name not in main_checks:
            # main never runs this check (a PR-only / merge-group lane) — can't
            # blame the PR from a main comparison.
            rows.append(TriageRow(name, req, st, "ABSENT", "NEW"))
            continue
        main_state = _norm(main_checks.get(name))
        if main_state in RED or main_state in {"", "ABSENT"}:
            verdict = "PRE-EXISTING"
        elif main_state in {_norm(x) for x in OK}:
            verdict = "REGRESSED"
        else:  # main pending — unknown
            verdict = "NEW"
        rows.append(TriageRow(name, req, st, main_state or "ABSENT", verdict))
    # REQUIRED + REGRESSED first (most actionable), then required, then the rest.
    order = {"REGRESSED": 0, "PENDING": 1, "NEW": 2, "PRE-EXISTING": 3}
    rows.sort(key=lambda r: (not r.required, order.get(r.verdict, 9), r.name))
    return rows


def format_rows(rows: list[TriageRow]) -> str:
    if not rows:
        return "All checks green (or none red) — nothing to triage."
    hint = {
        "REGRESSED": "← look here: green on main, red on your PR",
        "PRE-EXISTING": "not your change (already red / not run on main)",
        "NEW": "PR-only check (no main baseline)",
        "PENDING": "still running",
    }
    lines = [f"{'CHECK':<48} {'GATES':<9} {'PR':<12} {'MAIN':<12} VERDICT"]
    for r in rows:
        gate = "REQUIRED" if r.required else "advisory"
        lines.append(f"{r.name[:47]:<48} {gate:<9} {r.pr_state:<12} "
                     f"{r.main_state:<12} {r.verdict}  {hint.get(r.verdict, '')}")
    reg = [r for r in rows if r.verdict == "REGRESSED" and r.required]
    lines.append("")
    if reg:
        lines.append(f"⚠ {len(reg)} REQUIRED check(s) regressed by this PR — fix before merge: "
                     + ", ".join(r.name for r in reg))
    else:
        lines.append("✓ No required check was regressed by this PR "
                     "(any red required checks are pre-existing/pending on main).")
    return "\n".join(lines)


# ── CLI (thin gh-api wrapper) ────────────────────────────────────────────────

def _gh(gh: str, *args: str) -> str:
    return subprocess.run([gh, *args], check=True, capture_output=True, text=True).stdout


def _checks_for_sha(gh: str, repo: str, sha: str) -> dict[str, str | None]:
    """{name -> conclusion|status} merging check-runs and legacy statuses."""
    out: dict[str, str | None] = {}
    runs = json.loads(_gh(gh, "api", "--paginate",
                           f"repos/{repo}/commits/{sha}/check-runs"))
    for cr in runs.get("check_runs", []):
        out[cr["name"]] = cr.get("conclusion") or cr.get("status")
    status = json.loads(_gh(gh, "api", f"repos/{repo}/commits/{sha}/status"))
    for s in status.get("statuses", []):
        out.setdefault(s["context"], s.get("state"))
    return out


def _required_contexts(gh: str, repo: str, branch: str = "main") -> set[str]:
    try:
        prot = json.loads(_gh(gh, "api",
                              f"repos/{repo}/branches/{branch}/protection"))
        return set(prot.get("required_status_checks", {}).get("contexts", []) or [])
    except subprocess.CalledProcessError:
        return set()


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("pr", type=int, help="PR number.")
    ap.add_argument("--repo", default="danielraffel/pulp", help="owner/name.")
    ap.add_argument("--gh", default="ghapp",
                    help="gh CLI to use (default: ghapp — App token, higher rate limit).")
    args = ap.parse_args(argv)

    pr = json.loads(_gh(args.gh, "api", f"repos/{args.repo}/pulls/{args.pr}"))
    head_sha = pr["head"]["sha"]
    base = pr["base"]["ref"]
    main_sha = json.loads(_gh(args.gh, "api",
                              f"repos/{args.repo}/commits/{base}"))["sha"]

    pr_checks = _checks_for_sha(args.gh, args.repo, head_sha)
    main_checks = _checks_for_sha(args.gh, args.repo, main_sha)
    required = _required_contexts(args.gh, args.repo, base)

    print(f"PR #{args.pr} head {head_sha[:10]} vs {base} {main_sha[:10]} "
          f"({len(required)} required checks)\n")
    print(format_rows(triage(pr_checks, main_checks, required)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

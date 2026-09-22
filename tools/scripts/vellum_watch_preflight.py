#!/usr/bin/env python3
"""Advisory pre-push hint: does this range owe a Vellum watch event?

WHY THIS IS ADVISORY, AND MUST STAY ADVISORY
--------------------------------------------
`vellum_expansion_watch_check.py` guards a cross-repository provenance
boundary. Its authoritative run lives in two REQUIRED GitHub checks, and
`vellum-trusted-gate.yml` deliberately executes the checker from a trusted
root rather than from the pull request's own copy, while `.github/CODEOWNERS`
locks the events directory, the checker and the checker's test.

A local gate would execute the branch's copy of a script that exists precisely
so the branch's copy is not trusted. So this hint never blocks, never changes
a required context, and never touches the acceptance, the events directory or
the checker. It only moves the moment of DISCOVERY earlier than "CI refused
your PR twenty minutes after you pushed".

The verdict is also base-sensitive in ways that have manufactured false reds
before, which is the second reason a local copy must not hold a veto: comparing
against `origin/main` rather than the merge-base reports `watch events are
append-only` on any branch that is merely stale. This hint therefore always
resolves the MERGE-BASE, and says so in what it prints.

WHY IT EXISTS AT ALL
--------------------
The requirement triggers on changed PATHS, not on intent: the checker globs the
changed-file list against its capability-family selectors and reads nothing of
the diff, so a one-line `#include` under `test/test_browser_capture*` or an
ordinary edit under `tools/import-design/**` owes a hand-authored event file.
Three pull requests discovered that the same evening by failing a required
check, each paying a full CI round trip.

Exit codes (informational — every caller ignores them):
  0   nothing owed, or the range's affected families are already covered
  10  an event is owed and the range does not cover it
  20  no verdict available (no checker, no acceptance, no git range)
"""

from __future__ import annotations

import argparse
import importlib.util
import pathlib
import subprocess
import sys
import types

SKILL = ".agents/skills/pulp-vellum-change-routing/SKILL.md"
CHECKER_REL = "tools/scripts/vellum_expansion_watch_check.py"

NOTHING_OWED = 0
EVENT_OWED = 10
NO_VERDICT = 20


def _load_checker(root: pathlib.Path) -> types.ModuleType | None:
    """Import the repo's checker, or return None.

    An external clone, a deleted checker, or a syntax error in someone's
    in-flight edit must all degrade to "no hint", never to a traceback on
    somebody's push.
    """
    path = root / CHECKER_REL
    if not path.is_file():
        return None
    try:
        spec = importlib.util.spec_from_file_location("_pulp_vellum_watch_check", path)
        if spec is None or spec.loader is None:
            return None
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
    except Exception:
        return None
    required = ("load_acceptance", "_affected", "_changes", "verify", "EVENT_ROOT")
    if not all(hasattr(module, name) for name in required):
        return None
    return module


def _git(root: pathlib.Path, *args: str) -> str | None:
    try:
        out = subprocess.run(
            ["git", "-C", str(root), *args],
            check=True, capture_output=True, text=True,
        )
    except (OSError, subprocess.CalledProcessError):
        return None
    return out.stdout.strip()


def _resolve_range(root: pathlib.Path, base: str, head: str) -> tuple[str, str] | None:
    """(merge_base_sha, head_sha), both full 40-char.

    The MERGE-BASE, not the base ref's tip: the checker rejects a ref name
    outright, and comparing a stale branch against a moved `origin/main`
    manufactures `watch events are append-only` — a false red on a
    required-gate surface, which is worse than the friction being fixed.
    """
    head_sha = _git(root, "rev-parse", head)
    if not head_sha:
        return None
    merge_base = _git(root, "merge-base", base, head)
    if not merge_base:
        return None
    base_sha = _git(root, "rev-parse", merge_base)
    if not base_sha:
        return None
    return base_sha, head_sha


def _emit(lines: list[str], stream) -> None:
    for line in lines:
        print(line, file=stream)


def run(root: pathlib.Path, base: str, head: str, stream) -> int:
    checker = _load_checker(root)
    if checker is None:
        return NO_VERDICT

    resolved = _resolve_range(root, base, head)
    if resolved is None:
        return NO_VERDICT
    base_sha, head_sha = resolved

    try:
        scopes, _raw = checker.load_acceptance(root)
    except Exception:
        return NO_VERDICT
    if scopes is None:
        # The watch is not installed in this checkout; nothing can be owed.
        return NOTHING_OWED

    event_prefix = checker.EVENT_ROOT.as_posix() + "/"
    try:
        changes = checker._changes(root, base_sha, head_sha)
    except Exception:
        return NO_VERDICT
    changed = {path for _status, path in changes if not path.startswith(event_prefix)}

    # The path filter. The large majority of pushes stop here having spent one
    # `git diff` and a glob match, which is what keeps this affordable in a
    # hook advertised as sub-second.
    try:
        affected = checker._affected(scopes, changed)
    except Exception:
        return NO_VERDICT
    if not affected:
        return NOTHING_OWED

    try:
        report = checker.verify(root, base_sha, head_sha)
    except Exception:
        return NO_VERDICT

    if report.get("status") == "pass":
        _emit([
            "  vellum-watch: this range affects "
            f"{', '.join(sorted(affected))} and its watch events cover it.",
        ], stream)
        return NOTHING_OWED

    errors = report.get("errors") or ["(no detail)"]
    _emit([
        "  ⚠︎ vellum-watch (ADVISORY — this does not block your push):",
        f"     this range touches capability famil{'y' if len(affected) == 1 else 'ies'} "
        f"{', '.join(sorted(affected))},",
        "     so the required `Vellum freeze` check will want a watch event.",
        f"     checker says: {errors[0]}",
        "",
        "     The trigger is changed PATHS, not intent — a one-line edit under a",
        "     watched tree qualifies. Reproduce the CI verdict exactly:",
        "",
        f"       python3 {CHECKER_REL} --repo . \\",
        '         --base "$(git rev-parse "$(git merge-base origin/main HEAD)")" \\',
        '         --head "$(git rev-parse HEAD)"',
        "",
        "     COMMIT the event file before re-running. The checker measures the",
        "     COMMIT RANGE, never the working tree, so an event you have written",
        "     but not committed scores covered=[] identically to having written",
        "     none — which reads as a malformed file and sends you to debug a",
        "     correct one.",
        "",
        f"     Authoring rules (families, schema, append-only): {SKILL}",
    ], stream)
    return EVENT_OWED


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=pathlib.Path, default=pathlib.Path.cwd())
    parser.add_argument("--base", default="origin/main")
    parser.add_argument("--head", default="HEAD")
    args = parser.parse_args(argv)
    try:
        return run(args.repo.resolve(), args.base, args.head, sys.stderr)
    except Exception:
        # Advisory means advisory: nothing this script can hit is worth
        # interrupting a push over.
        return NO_VERDICT


if __name__ == "__main__":
    sys.exit(main())

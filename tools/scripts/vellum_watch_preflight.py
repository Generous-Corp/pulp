#!/usr/bin/env python3
"""Local gate: does this range owe a Vellum watch event, and what exactly?

WHAT IT DOES
------------
`vellum_expansion_watch_check.py` guards a cross-repository provenance
boundary. Its authoritative run lives in two REQUIRED GitHub checks
(`Vellum freeze`, `Vellum trusted freeze`), and `vellum-trusted-gate.yml`
executes the checker from a trusted root rather than from the pull request's
own copy, while `.github/CODEOWNERS` locks the events directory, the checker
and the checker's test.

This script runs the same verdict BEFORE the push and, when an event is owed,
prints the exact event JSON that would satisfy it (families, acceptance id and
sha256, suggested covering tests, watch-only/no-authority) — or writes it with
`--write-event`. `tools/scripts/gates.sh` and `.githooks/pre-push` run it with
`--enforce`, so an owed event FAILS locally (demotable in the hook with
`PULP_DISABLE_PREPUSH_GATES=1`, like every other primary gate).

WHY ENFORCING LOCALLY IS SAFE
-----------------------------
It used to be advisory, on the argument that a local copy must not hold a
veto over a trusted-root check. A local refusal is not that: it can only stop
a push the required checks would also refuse; it can never ACCEPT anything on
CI's behalf, and the trusted run stays the only authority. Advisory output was
measured not to work — on 2026-10-02 three pull requests (#9143, #9275,
#9283) each went red on both required Vellum checks, each paying a CI round
trip, with this hint printing the warning they scrolled past.

The real risk of a local copy is a FALSE red, so the constraints are:
  * the base is always the MERGE-BASE, never the base ref's tip — comparing a
    stale branch against a moved `origin/main` manufactures `watch events are
    append-only`;
  * "no verdict" (no checker, no acceptance, no git range, any internal
    error) never blocks: only a positive "event owed" verdict does.

Exit codes:
  0   nothing owed, or the range's affected families are already covered
  10  an event is owed and the range does not cover it
  20  no verdict available (no checker, no acceptance, no git range)
  2   --write-event refused (bad arguments, file exists, nothing owed)
"""

from __future__ import annotations

import argparse
import datetime as dt
import importlib.util
import json
import os
import pathlib
import re
import subprocess
import sys
import types

SKILL = ".agents/skills/pulp-vellum-change-routing/SKILL.md"
CHECKER_REL = "tools/scripts/vellum_expansion_watch_check.py"
EVENT_DIR_REL = ".github/vellum-expansion-watch-events"

NOTHING_OWED = 0
EVENT_OWED = 10
NO_VERDICT = 20
WRITE_REFUSED = 2

ACCEPTANCE_ID = "full-design-import-render-v1-pulp-watch"
RATIONALE_PLACEHOLDER = (
    "<REPLACE: what changed under the affected families, why it is safe, and "
    "that no design-import, Chromium, rendering, or packaging authority moves>"
)
EVENT_ID_RE = re.compile(r"^20[0-9]{6}-[a-z0-9]+(?:-[a-z0-9]+)*$")


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


def _range_events(root: pathlib.Path, head_sha: str, checker,
                  changes: list[tuple[str, str]]) -> tuple[set[str], list[str]]:
    """Families claimed by events ADDED in the range, read from the HEAD COMMIT.

    Read from the commit, not the working tree: the checker measures the
    commit range, so an uncommitted event covers nothing.
    """
    prefix = checker.EVENT_ROOT.as_posix() + "/"
    covered: set[str] = set()
    problems: list[str] = []
    for status, path in changes:
        if not path.startswith(prefix) or status != "A":
            continue
        blob = _git(root, "show", f"{head_sha}:{path}")
        try:
            families = json.loads(blob or "").get("capability_families") or []
        except (ValueError, AttributeError):
            problems.append(f"{path}: not valid JSON")
            continue
        covered.update(f for f in families if isinstance(f, str))
    return covered, problems


def _suggested_tests(changed: set[str], checker_rel: str) -> list[str]:
    """Covering-test commands derived from the test files the range touches."""
    tests: list[str] = []
    for path in sorted(changed):
        name = pathlib.PurePosixPath(path).name
        if path.endswith(".py") and name.startswith("test_"):
            tests.append(f"python3 {path}")
        elif path.endswith((".test.mjs", ".test.js", ".test.ts")):
            tests.append(f"node --test {path}")
        elif path.startswith("test/") and name.startswith("test_") and path.endswith(".cpp"):
            target = "pulp-test-" + name[len("test_"):-len(".cpp")].replace("_", "-")
            tests.append(f"ctest --test-dir build -R {target}")
    tests.append(
        f"python3 {checker_rel} --base <merge-base origin/main HEAD> --head <HEAD>")
    tests.append("vellum-expansion-watch")
    return tests


def _slug(text: str) -> str:
    text = re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")
    text = re.sub(r"-{2,}", "-", text)
    return text[:60].strip("-") or "change"


def _default_event_id(root: pathlib.Path, today: dt.date) -> str:
    branch = _git(root, "rev-parse", "--abbrev-ref", "HEAD") or "change"
    branch = branch.rsplit("/", 1)[-1]
    slug = _slug(branch)
    if not slug.endswith("-watch"):
        slug = f"{slug}-watch"
    return f"{today.strftime('%Y%m%d')}-{slug}"


def build_event(checker, families: list[str], tests: list[str], *,
                event_id: str, created_at: str, rationale: str) -> dict:
    """The exact event shape `validate_event` accepts, key for key."""
    return {
        "schema_version": 1,
        "kind": "authority-expansion-watch-change",
        "event_id": event_id,
        "created_at": created_at,
        "acceptance_id": ACCEPTANCE_ID,
        "acceptance_sha256": checker.EXPECTED_ACCEPTANCE_SHA256,
        "capability_families": sorted(families),
        "rationale": rationale,
        "tests": tests,
        "disposition": "watch-only-no-authority",
        "authority_effect": "none",
    }


def _render(event: dict) -> str:
    return json.dumps(event, indent=2) + "\n"


class Verdict:
    def __init__(self, code: int, affected=frozenset(), needed=frozenset(),
                 over=frozenset(), errors=(), event=None):
        self.code = code
        self.affected = set(affected)
        self.needed = set(needed)
        self.over = set(over)
        self.errors = list(errors)
        self.event = event


def evaluate(root: pathlib.Path, base: str, head: str, *,
             event_id: str | None = None, rationale: str | None = None,
             now: dt.datetime | None = None) -> Verdict:
    checker = _load_checker(root)
    if checker is None:
        return Verdict(NO_VERDICT)

    resolved = _resolve_range(root, base, head)
    if resolved is None:
        return Verdict(NO_VERDICT)
    base_sha, head_sha = resolved

    try:
        scopes, _raw = checker.load_acceptance(root)
    except Exception:
        return Verdict(NO_VERDICT)
    if scopes is None:
        # The watch is not installed in this checkout; nothing can be owed.
        return Verdict(NOTHING_OWED)

    event_prefix = checker.EVENT_ROOT.as_posix() + "/"
    try:
        changes = checker._changes(root, base_sha, head_sha)
    except Exception:
        return Verdict(NO_VERDICT)
    changed = {path for _status, path in changes if not path.startswith(event_prefix)}

    # The path filter. The large majority of pushes stop here having spent one
    # `git diff` and a glob match, which is what keeps this affordable in a
    # hook advertised as sub-second.
    try:
        affected = checker._affected(scopes, changed)
    except Exception:
        return Verdict(NO_VERDICT)
    has_events = any(p.startswith(event_prefix) for _s, p in changes)
    if not affected and not has_events:
        return Verdict(NOTHING_OWED)

    try:
        report = checker.verify(root, base_sha, head_sha)
    except Exception:
        return Verdict(NO_VERDICT)
    if report.get("status") == "pass":
        return Verdict(NOTHING_OWED, affected=affected)

    covered, problems = _range_events(root, head_sha, checker, changes)
    needed = affected - covered
    over = covered - affected
    errors = list(report.get("errors") or ["(no detail)"]) + problems
    now = now or dt.datetime.now(dt.timezone.utc)
    event = None
    if needed:
        event = build_event(
            checker,
            sorted(needed),
            _suggested_tests(changed, CHECKER_REL),
            event_id=event_id or _default_event_id(root, now.date()),
            created_at=now.strftime("%Y-%m-%dT%H:%M:%SZ"),
            rationale=rationale or RATIONALE_PLACEHOLDER,
        )
    return Verdict(EVENT_OWED, affected=affected, needed=needed, over=over,
                   errors=errors, event=event)


def run(root: pathlib.Path, base: str, head: str, stream, *,
        enforce: bool = False) -> int:
    verdict = evaluate(root, base, head)
    if verdict.code == NOTHING_OWED and verdict.affected:
        _emit([
            "  vellum-watch: this range affects "
            f"{', '.join(sorted(verdict.affected))} and its watch events cover it.",
        ], stream)
    if verdict.code != EVENT_OWED:
        return verdict.code

    label = ("✗ vellum-watch (BLOCKING — the required `Vellum freeze` and "
             "`Vellum trusted freeze` checks will fail the same way)"
             if enforce else "⚠︎ vellum-watch (ADVISORY — this does not block your push)")
    affected = sorted(verdict.affected)
    lines = [
        f"  {label}:",
        f"     this range touches capability famil{'y' if len(affected) == 1 else 'ies'} "
        f"{', '.join(affected) or '(none)'}.",
        f"     checker says: {verdict.errors[0]}",
    ]
    for extra in verdict.errors[1:]:
        lines.append(f"                   {extra}")
    if verdict.over:
        lines += [
            "",
            "     Events in this range claim families the range does NOT touch: "
            f"{', '.join(sorted(verdict.over))}.",
            "     Coverage must be EXACT — drop those families from the event.",
        ]
    if verdict.event is not None:
        lines += [
            "",
            "     Add this file (or run the --write-event command below), replace the",
            "     rationale, and COMMIT it:",
            "",
            f"     {EVENT_DIR_REL}/{verdict.event['event_id']}.json",
            "",
        ]
        lines += ["       " + ln for ln in _render(verdict.event).splitlines()]
        lines += [
            "",
            "     Or write it in one step:",
            "",
            "       python3 tools/scripts/vellum_watch_preflight.py --write-event \\",
            '         --rationale "<what changed, why no authority moves>"',
        ]
    lines += [
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
    ]
    _emit(lines, stream)
    return EVENT_OWED


def write_event(root: pathlib.Path, base: str, head: str, stream, *,
                rationale: str | None, event_id: str | None) -> int:
    if not rationale or len(rationale.strip()) < 24 or rationale.lstrip().startswith("<"):
        print("vellum-watch: --write-event needs a real --rationale (24+ characters) "
              "saying what changed and that no authority moves.", file=stream)
        return WRITE_REFUSED
    if event_id is not None and not EVENT_ID_RE.fullmatch(event_id):
        print(f"vellum-watch: --event-id {event_id!r} must look like "
              "YYYYMMDD-lower-case-words", file=stream)
        return WRITE_REFUSED
    verdict = evaluate(root, base, head, event_id=event_id, rationale=rationale.strip())
    if verdict.code != EVENT_OWED or verdict.event is None:
        print("vellum-watch: no event is owed by this range"
              + ("" if verdict.code != EVENT_OWED else
                 f" that a new event can fix: {verdict.errors[0]}"),
              file=stream)
        return WRITE_REFUSED if verdict.code == EVENT_OWED else verdict.code
    checker = _load_checker(root)
    target = root / checker.EVENT_ROOT / f"{verdict.event['event_id']}.json"
    if target.exists():
        print(f"vellum-watch: {target.relative_to(root)} already exists; "
              "pass --event-id to choose another name", file=stream)
        return WRITE_REFUSED
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(_render(verdict.event), encoding="utf-8")
    rel = target.relative_to(root).as_posix()
    _emit([
        f"vellum-watch: wrote {rel}",
        f"  families: {', '.join(verdict.event['capability_families'])}",
        "  Review the suggested tests, then commit it:",
        f"    git add -- {rel} && git commit -m 'ci: record a Vellum watch event'",
    ], stream)
    return NOTHING_OWED


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=pathlib.Path, default=pathlib.Path.cwd())
    parser.add_argument("--base", default="origin/main")
    parser.add_argument("--head", default="HEAD")
    parser.add_argument(
        "--enforce", action="store_true",
        help="label an owed event as BLOCKING (the caller fails on exit 10)")
    parser.add_argument(
        "--write-event", action="store_true",
        help="write the owed event file into the working tree")
    parser.add_argument("--rationale", help="rationale text for --write-event")
    parser.add_argument("--event-id", help="event id for --write-event "
                        "(default: <today>-<branch>-watch)")
    args = parser.parse_args(argv)
    root = args.repo.resolve()
    if args.write_event:
        return write_event(root, args.base, args.head, sys.stdout,
                           rationale=args.rationale, event_id=args.event_id)
    try:
        return run(root, args.base, args.head, sys.stderr, enforce=args.enforce)
    except Exception:
        # An internal error is "no verdict", and no verdict never blocks.
        return NO_VERDICT


if __name__ == "__main__":
    sys.exit(main())

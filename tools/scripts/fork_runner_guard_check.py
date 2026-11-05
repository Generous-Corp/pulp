#!/usr/bin/env python3
"""A fork pull request, deleted or not, must never get a self-hosted runner from a variable.

Repository variables resolve for fork pull requests, so a job a pull request
can start must not take its `runs-on` from a variable unless that variable is
a GitHub-hosted lane in `runner_topology.json`, or the expression gates it.
The gate is

    (github.event_name != 'pull_request' || github.event.pull_request.head.repo.full_name == github.repository) && vars.X || '"<hosted label>"'

or any expression that tests `github.event.pull_request.head.repo.full_name ==
github.repository` before reading the variable. A deleted fork's head
repository is null, so it fails the comparison and gets the hosted label;
`!github.event.pull_request.head.repo.fork` would pass it, because `!null` is
true. A dispatch-only selector (`github.event_name == 'workflow_dispatch' &&
vars.X`) is fine too.

Every step a pull request can run that calls `resolve_runs_on.py` must also
pass `PR_HEAD_REPO`, because the resolver routes a pull request whose head
repository is missing or foreign to its hosted fallback.

Exit 1 lists each finding; exit 0 when there are none.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS = ROOT / ".github" / "workflows"
TOPOLOGY = ROOT / "tools" / "scripts" / "runner_topology.json"
SAME_REPO = "github.event.pull_request.head.repo.full_name == github.repository"
PR_TRIGGERS = ("pull_request", "pull_request_target")
HEAD_REPO = "${{ github.event.pull_request.head.repo.full_name }}"
VARIABLE = re.compile(r"\bvars\.([A-Z0-9_]+)")


def hosted_lane_variables(topology: Path = TOPOLOGY) -> set[str]:
    lanes = json.loads(topology.read_text(encoding="utf-8"))["lanes"]
    return {lane["variable"] for lane in lanes if lane.get("provisioning") == "github-hosted"}


def guarded_at(expression: str, at: int) -> bool:
    """Whether the `&&` chain that reads the variable at `at` first requires a
    same-repository pull request or a dispatch: the text since the last `||`
    outside parentheses before `at`."""
    depth, start = 0, 0
    for i in range(at - 1, -1, -1):
        c = expression[i]
        if c == ")":
            depth += 1
        elif c == "(":
            if depth == 0:
                start = i + 1
                break
            depth -= 1
        elif expression.startswith("||", i) and depth == 0:
            start = i + 2
            break
    chain = expression[start:at]
    # A chain opened inside parentheses inherits the conditions ANDed before them.
    return SAME_REPO in chain or "github.event_name == 'workflow_dispatch'" in chain or (
        start > 0 and expression[start - 1] == "(" and guarded_at(expression, start - 1))


def triggers(doc: dict) -> set[str]:
    on = doc.get("on", doc.get(True))
    if isinstance(on, str):
        return {on}
    if isinstance(on, list):
        return set(on)
    return set(on or {})


def findings(path: Path, hosted: set[str]) -> list[str]:
    doc = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(doc, dict) or not triggers(doc) & set(PR_TRIGGERS):
        return []
    found = []
    for name, job in (doc.get("jobs") or {}).items():
        runs_on = " ".join(str(job.get("runs-on", "")).split())
        variables = set(VARIABLE.findall(runs_on))
        unguarded = sorted({m.group(1) for m in VARIABLE.finditer(runs_on)
                            if m.group(1) not in hosted and not guarded_at(runs_on, m.start())})
        if unguarded:
            found.append(f"{path.name}: job {name} takes runs-on from {', '.join(unguarded)} "
                         "without the fork guard")
        for index, step in enumerate(job.get("steps") or []):
            if "resolve_runs_on.py" in str(step.get("run", "")):
                env = {**(job.get("env") or {}), **(step.get("env") or {})}
                if env.get("PR_HEAD_REPO") != HEAD_REPO:
                    label = step.get("name") or step.get("id") or f"step {index + 1}"
                    found.append(f"{path.name}: job {name} ({label}) calls resolve_runs_on.py without "
                                 f"PR_HEAD_REPO: {HEAD_REPO}")
    return found


def main(argv: list[str]) -> int:
    paths = [Path(a) for a in argv] or sorted(WORKFLOWS.glob("*.yml"))
    hosted = hosted_lane_variables()
    found = [f for path in paths for f in findings(path, hosted)]
    for line in found:
        print(line)
    if not found:
        print(f"fork-runner-guard: {len(paths)} workflows checked; no fork pull request can reach a "
              "variable-selected runner")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

#!/usr/bin/env python3
"""Keep the schedule-backstop manifest honest about the workflows it names.

GitHub delays and drops ``schedule`` events under load, and on this repository
an hourly-or-faster cron fires roughly once every five hours. The frequent
safety nets (merge-stall, runner health, release reconcile, ...) therefore run
far less often than their cron says. An external dispatcher (tartci's
schedule-backstop agent) closes that gap by calling ``workflow_dispatch`` on
each workflow listed in ``.github/schedule-backstop.json`` whenever the newest
``main`` run of that workflow is older than its cadence. The cron stays in
place as the backstop's own backstop.

The manifest is the contract between this repository and the dispatcher, so
this check holds every listed workflow to what the dispatcher assumes:

* it exists, accepts ``workflow_dispatch``, and has no required dispatch input
  (the dispatcher sends none, so a dispatched run must behave like a scheduled
  one);
* it keeps an hourly-or-faster ``schedule`` cron, and the manifest's
  ``cadence_minutes`` equals the longest gap between that cron's firings; or
  it keeps exactly one daily cron (a fixed minute and hour) and is listed at
  1440, because a daily check that counts consecutive days cannot afford a
  dropped cron (the read audit's Stage 0 streak);
* it declares a top-level ``concurrency`` group, so a dispatch that overlaps a
  late scheduled run cannot pile up;
* no job names a self-hosted runner label (the dispatcher must never add load
  to the fleet the safety nets watch).

It also requires every workflow with an hourly-or-faster cron to be either
listed or excluded with a reason, so a new frequent safety net cannot silently
miss the backstop.

Usage:
    schedule_backstop_check.py                  # check the repository
    schedule_backstop_check.py --root <dir>     # check another checkout

Exit codes: 0 = consistent, 1 = violations, 2 = environment error.
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import sys

try:
    import yaml
except ImportError:
    print("schedule_backstop_check: PyYAML not available", file=sys.stderr)
    sys.exit(2)

MANIFEST = ".github/schedule-backstop.json"
WORKFLOWS = ".github/workflows"
SCHEMA_VERSION = 1
SELF_HOSTED_MARKERS = ("self-hosted", "pulp-build-vm", "pulp-gate")
DAILY_MINUTES = 1440


def _on_block(doc):
    # YAML parses the bare key `on:` as the boolean True, so check both.
    if not isinstance(doc, dict):
        return None
    return doc.get("on", doc.get(True))


def _minutes(field: str) -> set[int] | None:
    """Expand a cron minute field into the minutes of the hour it fires on."""
    out: set[int] = set()
    for part in field.split(","):
        if part == "*":
            out.update(range(60))
        elif part.startswith("*/") and part[2:].isdigit() and int(part[2:]) > 0:
            out.update(range(0, 60, int(part[2:])))
        elif part.isdigit() and int(part) < 60:
            out.add(int(part))
        else:
            return None
    return out


def hourly_cadence(cron: str) -> int | None:
    """Longest gap in minutes between firings, or None if not hourly-or-faster."""
    fields = cron.split()
    if len(fields) != 5 or fields[1:] != ["*", "*", "*", "*"]:
        return None
    minutes = _minutes(fields[0])
    if not minutes:
        return None
    ordered = sorted(minutes)
    gaps = [b - a for a, b in zip(ordered, ordered[1:])]
    gaps.append(ordered[0] + 60 - ordered[-1])
    return max(gaps)


def _crons(doc) -> list[str]:
    on = _on_block(doc)
    if not isinstance(on, dict) or not isinstance(on.get("schedule"), list):
        return []
    return [row["cron"] for row in on["schedule"] if isinstance(row, dict) and isinstance(row.get("cron"), str)]


def schedule_cadence(doc) -> int | None:
    """Cadence of the workflow's combined hourly-or-faster crons, if any."""
    minutes: set[int] = set()
    for cron in _crons(doc):
        if hourly_cadence(cron) is not None:
            minutes |= _minutes(cron.split()[0]) or set()
    if not minutes:
        return None
    return hourly_cadence(",".join(str(m) for m in sorted(minutes)) + " * * * *")


def is_daily(cron: str) -> bool:
    """A cron that fires once a day: a fixed minute and hour, every day."""
    fields = cron.split()
    return (len(fields) == 5 and fields[2:] == ["*", "*", "*"]
            and fields[0].isdigit() and int(fields[0]) < 60 and fields[1].isdigit() and int(fields[1]) < 24)


def listed_cadence(doc) -> int | None:
    """The cadence a manifest row must state: the hourly cadence, else 1440 for
    a workflow whose only cron is one daily firing."""
    hourly = schedule_cadence(doc)
    if hourly is not None:
        return hourly
    crons = _crons(doc)
    return DAILY_MINUTES if len(crons) == 1 and is_daily(crons[0]) else None


def _dispatch_inputs(doc):
    on = _on_block(doc)
    if not isinstance(on, dict) or "workflow_dispatch" not in on:
        return None
    block = on["workflow_dispatch"] or {}
    return (block.get("inputs") or {}) if isinstance(block, dict) else {}


def _runs_on_labels(job) -> list[str]:
    value = job.get("runs-on")
    if isinstance(value, str):
        return [value]
    if isinstance(value, list):
        return [str(v) for v in value]
    if isinstance(value, dict):
        return [json.dumps(value)]
    return []


def check_workflow(path: str, cadence: object) -> list[str]:
    name = os.path.basename(path)
    try:
        with open(path, encoding="utf-8") as source:
            doc = yaml.safe_load(source)
    except FileNotFoundError:
        return [f"{name}: listed in {MANIFEST} but does not exist"]
    except yaml.YAMLError as exc:
        return [f"{name}: YAML parse error: {exc}"]
    violations: list[str] = []
    inputs = _dispatch_inputs(doc)
    if inputs is None:
        violations.append(f"{name}: has no workflow_dispatch trigger")
    else:
        for key, spec in inputs.items():
            if isinstance(spec, dict) and spec.get("required") is True and "default" not in spec:
                violations.append(f"{name}: dispatch input '{key}' is required with no default")
    actual = listed_cadence(doc)
    if actual is None:
        violations.append(f"{name}: has neither an hourly-or-faster cron nor exactly one daily cron")
    elif actual != cadence:
        violations.append(
            f"{name}: manifest cadence_minutes={cadence} but its cron fires every {actual} min"
        )
    if not isinstance(doc, dict) or "concurrency" not in doc:
        violations.append(f"{name}: has no top-level concurrency group")
    jobs = doc.get("jobs", {}) if isinstance(doc, dict) else {}
    for job_name, job in (jobs or {}).items():
        if not isinstance(job, dict):
            continue
        for label in _runs_on_labels(job):
            if any(marker in label for marker in SELF_HOSTED_MARKERS):
                violations.append(f"{name}: job '{job_name}' runs on self-hosted label {label!r}")
    return violations


def check(root: str) -> list[str]:
    manifest_path = os.path.join(root, MANIFEST)
    try:
        with open(manifest_path, encoding="utf-8") as source:
            manifest = json.load(source)
    except (OSError, json.JSONDecodeError) as exc:
        return [f"{MANIFEST}: cannot read: {exc}"]
    if not isinstance(manifest, dict) or manifest.get("schema_version") != SCHEMA_VERSION:
        return [f"{MANIFEST}: schema_version must be {SCHEMA_VERSION}"]
    if set(manifest) != {"schema_version", "ref", "workflows", "excluded"}:
        return [f"{MANIFEST}: expected exactly schema_version, ref, workflows, excluded"]
    if not isinstance(manifest["ref"], str) or not manifest["ref"]:
        return [f"{MANIFEST}: ref must be a non-empty branch name"]

    violations: list[str] = []
    named: set[str] = set()
    for row in manifest.get("workflows") or []:
        if not isinstance(row, dict) or set(row) != {"file", "cadence_minutes"}:
            violations.append(f"{MANIFEST}: workflow rows need exactly file and cadence_minutes")
            continue
        file, cadence = row["file"], row["cadence_minutes"]
        if not isinstance(file, str) or "/" in file or file in named:
            violations.append(f"{MANIFEST}: workflow file {file!r} is not a unique bare name")
            continue
        if type(cadence) is not int or not (1 <= cadence <= 60 or cadence == DAILY_MINUTES):
            violations.append(f"{MANIFEST}: {file} cadence_minutes must be an integer in 1..60, or "
                              f"{DAILY_MINUTES} for a daily workflow")
            continue
        named.add(file)
        violations.extend(check_workflow(os.path.join(root, WORKFLOWS, file), cadence))
    for row in manifest.get("excluded") or []:
        if not isinstance(row, dict) or set(row) != {"file", "reason"}:
            violations.append(f"{MANIFEST}: excluded rows need exactly file and reason")
            continue
        file, reason = row["file"], row["reason"]
        if not isinstance(reason, str) or not reason.strip():
            violations.append(f"{MANIFEST}: excluded {file} needs a reason")
        if not isinstance(file, str) or file in named:
            violations.append(f"{MANIFEST}: excluded {file!r} is duplicated or also listed")
            continue
        if not os.path.exists(os.path.join(root, WORKFLOWS, file)):
            violations.append(f"{MANIFEST}: excluded {file} does not exist")
        named.add(file)

    pattern = os.path.join(root, WORKFLOWS, "*.y*ml")
    for path in sorted(glob.glob(pattern)):
        try:
            with open(path, encoding="utf-8") as source:
                doc = yaml.safe_load(source)
        except yaml.YAMLError:
            continue
        if schedule_cadence(doc) is not None and os.path.basename(path) not in named:
            violations.append(
                f"{os.path.basename(path)}: fires hourly or faster but is neither listed nor "
                f"excluded in {MANIFEST}"
            )
    return violations


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", default=".")
    args = parser.parse_args(argv)
    violations = check(args.root)
    if violations:
        print("schedule-backstop: violations found:", file=sys.stderr)
        for violation in violations:
            print(f"  x {violation}", file=sys.stderr)
        print(
            f"\nEvery workflow in {MANIFEST} is dispatched by an external backstop at its\n"
            "cadence. Keep cadence_minutes equal to the cron, keep workflow_dispatch with\n"
            "no required input, keep a concurrency group, and stay on GitHub-hosted\n"
            "runners. A new hourly-or-faster cron must be listed or excluded with a reason.",
            file=sys.stderr,
        )
        return 1
    print("schedule-backstop: manifest consistent")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

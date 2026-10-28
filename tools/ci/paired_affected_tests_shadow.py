#!/usr/bin/env python3
"""Run an affected-test command beside the authoritative full command.

This is an observation-only primitive for merge-group experiments.  Both
commands receive the same checkout and build directory; the full command is
authoritative and its exit status is never changed by the selected command.
The receipt records exact identities, outcomes, and failures outside the
selected result so a promotion decision can be made from evidence.

Commands are supplied as JSON arrays rather than shell strings.  That keeps
argument boundaries unambiguous and avoids giving a shadow experiment shell
authority.  A caller should use separate JUnit paths for the two processes.
The process returns zero after writing a receipt, including when either
experiment cannot start; shadow telemetry must never make the merge gate red.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

SCHEMA = "pulp-paired-affected-tests-shadow/v1"


def _digest(value: Any) -> str:
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def _run(command: list[str], log: Path) -> tuple[int | None, float, str | None]:
    started = time.monotonic()
    try:
        with log.open("w", encoding="utf-8") as stream:
            proc = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, check=False)
        return proc.returncode, round(time.monotonic() - started, 1), None
    except OSError as exc:
        return None, round(time.monotonic() - started, 1), repr(exc)


def _junit(path: Path) -> dict[str, str]:
    """Read CTest JUnit outcomes without treating a missing file as success."""
    import xml.etree.ElementTree as ET
    if not path.is_file():
        return {}
    try:
        root = ET.parse(path).getroot()
    except (OSError, ET.ParseError):
        return {}
    out: dict[str, str] = {}
    for case in root.iter("testcase"):
        name = case.get("name", "")
        if not name:
            continue
        if case.find("failure") is not None or case.find("error") is not None or case.get("status") == "fail":
            out[name] = "fail"
        elif case.find("skipped") is not None or case.get("status") in ("notrun", "disabled"):
            out[name] = "notrun"
        else:
            out[name] = "run"
    return out


def receipt(args: argparse.Namespace, selected: dict, full: dict) -> dict:
    selected_outcomes = _junit(Path(args.selected_junit))
    full_outcomes = _junit(Path(args.full_junit))
    selected_names = set(selected.get("tests") or selected.get("selected_tests") or selected_outcomes)
    full_failures = {name for name, state in full_outcomes.items() if state == "fail"}
    selected_failures = {name for name, state in selected_outcomes.items() if state == "fail"}
    outside = sorted(full_failures - selected_names)
    return {
        "schema": SCHEMA,
        "mode": "shadow",
        "shadow_only": True,
        "authoritative": "full",
        "repository": args.repository,
        "event": args.event,
        "base": args.base,
        "head": args.head,
        "tree": args.tree,
        "identity_complete": all((args.repository, args.event, args.base, args.head, args.tree)),
        "selection": selected,
        "selection_digest": _digest(selected),
        "selected_outcomes": selected_outcomes,
        "selected_failures": sorted(selected_failures),
        "full_outcomes": full_outcomes,
        "full_failures": sorted(full_failures),
        "failed_outside_selection": outside,
        "full_outcome": full.get("outcome", "unavailable"),
        "selected_process": selected.get("process", {}),
        "full_process": full.get("process", {}),
        "selection_complete": bool(selected_outcomes) and selected_names.issubset(selected_outcomes),
    }


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--selected-command", type=Path, required=True, help="JSON array")
    ap.add_argument("--full-command", type=Path, required=True, help="JSON array")
    ap.add_argument("--selected-junit", required=True)
    ap.add_argument("--full-junit", required=True)
    ap.add_argument("--selection", type=Path, required=True, help="JSON selection plan")
    ap.add_argument("--receipt-out", type=Path, required=True)
    ap.add_argument("--repository", default=os.environ.get("GITHUB_REPOSITORY", ""))
    ap.add_argument("--event", default=os.environ.get("GITHUB_EVENT_NAME", "merge_group"))
    ap.add_argument("--base", default="")
    ap.add_argument("--head", default=os.environ.get("GITHUB_SHA", ""))
    ap.add_argument("--tree", default="")
    args = ap.parse_args(argv)

    try:
        selected_plan = json.loads(args.selection.read_text(encoding="utf-8"))
        selected_cmd = json.loads(args.selected_command.read_text(encoding="utf-8"))
        full_cmd = json.loads(args.full_command.read_text(encoding="utf-8"))
        if not all(isinstance(x, list) and all(isinstance(y, str) for y in x) for x in (selected_cmd, full_cmd)):
            raise ValueError("commands must be JSON arrays of strings")
        if not isinstance(selected_plan, dict):
            raise ValueError("selection must be a JSON object")
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        selected_plan = {"tests": [], "selection_error": str(exc)}
        selected_cmd, full_cmd = [], []

    work = args.receipt_out.parent
    work.mkdir(parents=True, exist_ok=True)
    selected_proc: dict[str, Any] = {"command_digest": _digest(selected_cmd)}
    full_proc: dict[str, Any] = {"command_digest": _digest(full_cmd)}
    # Start selected first, then immediately run the authoritative suite.  The
    # selected process is deliberately independent and never shares output.
    selected_started = time.monotonic()
    selected_error = None
    selected_rc: int | None = None
    try:
        selected_log = work / "selected.log"
        with selected_log.open("w", encoding="utf-8") as stream:
            selected_child = subprocess.Popen(selected_cmd, stdout=stream, stderr=subprocess.STDOUT) if selected_cmd else None
            if selected_child is None:
                selected_error = "selection command unavailable"
            full_rc, full_seconds, full_error = _run(full_cmd, work / "full.log") if full_cmd else (None, 0.0, "full command unavailable")
            if selected_child is not None:
                selected_rc = selected_child.wait()
        selected_proc.update(returncode=selected_rc, seconds=round(time.monotonic() - selected_started, 1), error=selected_error)
        full_proc.update(returncode=full_rc, seconds=full_seconds, error=full_error)
    except OSError as exc:
        selected_proc.update(returncode=selected_rc, seconds=round(time.monotonic() - selected_started, 1), error=repr(exc))
        full_proc.update(returncode=None, seconds=0.0, error=repr(exc))

    selected_plan["process"] = selected_proc
    full = {"process": full_proc, "outcome": "success" if full_proc.get("returncode") == 0 else "failure" if full_proc.get("returncode") is not None else "unavailable"}
    out = receipt(args, selected_plan, full)
    args.receipt_out.write_text(json.dumps(out, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print("::notice title=paired-affected-tests-shadow::" + json.dumps({"schema": SCHEMA, "failed_outside_selection": out["failed_outside_selection"], "full_outcome": out["full_outcome"]}, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    sys.exit(main())

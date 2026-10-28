#!/usr/bin/env python3
"""Run an affected-test command beside the authoritative full command.

This is an observation-only primitive for merge-group experiments.  The full
command is authoritative and its exit status is never changed by the selected
command. Callers must provide separate working directories: the selected run
is not allowed to race the full run's build or test outputs.
The receipt records exact identities, outcomes, and failures outside the
selected result so a promotion decision can be made from evidence.

Commands are supplied as JSON arrays rather than shell strings.  That keeps
argument boundaries unambiguous and avoids giving a shadow experiment shell
authority.  A caller should use separate JUnit paths for the two processes.
The selected process has a bounded timeout and is terminated as a process group
if it does not finish; the full process has no such bound here because its
normal CI timeout remains authoritative.
The process returns zero after writing a receipt, including when either
experiment cannot start; shadow telemetry must never make the merge gate red.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

SCHEMA = "pulp-paired-affected-tests-shadow/v1"


def _digest(value: Any) -> str:
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def _popen(command: list[str], log: Path, cwd: Path) -> tuple[subprocess.Popen[bytes] | None, Any, str | None]:
    """Start one experiment in its own output stream and process group."""
    try:
        stream = log.open("wb")
        kwargs: dict[str, Any] = {
            "stdout": stream,
            "stderr": subprocess.STDOUT,
            "cwd": str(cwd),
        }
        if os.name == "posix":
            kwargs["start_new_session"] = True
        elif hasattr(subprocess, "CREATE_NEW_PROCESS_GROUP"):
            kwargs["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
        return subprocess.Popen(command, **kwargs), stream, None
    except OSError as exc:
        return None, None, repr(exc)


def _terminate(proc: subprocess.Popen[bytes]) -> str:
    """Stop a timed-out selected process, including descendants."""
    if proc.poll() is not None:
        return "already-exited"
    try:
        if os.name == "posix":
            os.killpg(proc.pid, signal.SIGTERM)
        else:
            proc.terminate()
        try:
            proc.wait(timeout=2)
            return "terminated"
        except subprocess.TimeoutExpired:
            if os.name == "posix":
                os.killpg(proc.pid, signal.SIGKILL)
            else:
                proc.kill()
            proc.wait(timeout=2)
            return "killed"
    except (OSError, subprocess.TimeoutExpired) as exc:
        return f"termination-error:{exc!r}"


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
    selected_outcome = selected.get("outcome", "unavailable")
    full_outcome = full.get("outcome", "unavailable")
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
        "full_outcome": full_outcome,
        # An absent authoritative run is never a successful observation.
        "full_success": full_outcome == "success",
        "outcome_comparison": {
            "selected": selected_outcome,
            "full": full_outcome,
            "same": selected_outcome == full_outcome,
            "full_success": full_outcome == "success",
        },
        "selected_process": selected.get("process", {}),
        "full_process": full.get("process", {}),
        "selection_complete": bool(selected_outcomes) and selected_names.issubset(selected_outcomes),
    }


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--selected-command", type=Path, required=True, help="JSON array")
    ap.add_argument("--full-command", type=Path, required=True, help="JSON array")
    ap.add_argument("--selected-cwd", type=Path, required=True, help="isolated selected-test working directory")
    ap.add_argument("--full-cwd", type=Path, required=True, help="isolated full-test working directory")
    ap.add_argument("--selected-timeout-seconds", type=float, default=120.0,
                    help="bound selected-test execution (default: 120 seconds)")
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

    selected_plan: dict[str, Any]
    selection_error: str | None = None
    try:
        selected_plan = json.loads(args.selection.read_text(encoding="utf-8"))
        if not isinstance(selected_plan, dict):
            raise ValueError("selection must be a JSON object")
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        selection_error = str(exc)
        selected_plan = {"tests": [], "selection_error": selection_error}

    def load_command(path: Path, name: str) -> tuple[list[str], str | None]:
        try:
            value = json.loads(path.read_text(encoding="utf-8"))
            if not isinstance(value, list) or not all(isinstance(x, str) for x in value):
                raise ValueError(f"{name} command must be a JSON array of strings")
            return value, None
        except (OSError, ValueError, json.JSONDecodeError) as exc:
            return [], f"{name} command unavailable: {exc}"

    selected_cmd, selected_command_error = load_command(args.selected_command, "selected")
    full_cmd, full_command_error = load_command(args.full_command, "full")
    if selection_error is not None:
        selected_cmd = []
        selected_command_error = f"selection unavailable: {selection_error}"

    if args.selected_timeout_seconds <= 0:
        selected_plan["selection_error"] = "selected timeout must be positive"
        selected_cmd = []
        selected_command_error = "selected timeout must be positive"
    selected_cwd = args.selected_cwd
    full_cwd = args.full_cwd
    try:
        selected_cwd = args.selected_cwd.resolve()
        full_cwd = args.full_cwd.resolve()
        if selected_cwd == full_cwd:
            raise ValueError("selected and full working directories must be different")
    except (OSError, ValueError) as exc:
        selected_plan["selection_error"] = str(exc)
        selected_cmd = []
        selected_command_error = str(exc)

    if not selected_cwd.is_dir():
        selected_plan["selection_error"] = "selected working directory must already exist"
        selected_cmd = []
        selected_command_error = "selected working directory must already exist"
    if not full_cwd.is_dir():
        full_cmd = []
        full_command_error = "full working directory must already exist"

    work = args.receipt_out.parent
    work.mkdir(parents=True, exist_ok=True)
    selected_proc: dict[str, Any] = {"command_digest": _digest(selected_cmd)}
    full_proc: dict[str, Any] = {"command_digest": _digest(full_cmd)}
    # Start selected first, then immediately run the authoritative suite.  Both
    # have isolated working directories and output.  Monitor the selected child
    # while full runs so a hung selector cannot hold the shadow job forever.
    selected_started = time.monotonic()
    selected_child, selected_stream, selected_error = _popen(selected_cmd, work / "selected.log", selected_cwd) if selected_cmd else (None, None, selected_command_error or "selection command unavailable")
    full_started = time.monotonic()
    full_child, full_stream, full_error = _popen(full_cmd, work / "full.log", full_cwd) if full_cmd else (None, None, full_command_error or "full command unavailable")
    selected_timed_out = False
    selected_termination = None
    selected_rc: int | None = None
    full_rc: int | None = None
    while (selected_child is not None and selected_rc is None) or (full_child is not None and full_rc is None):
        now = time.monotonic()
        if selected_child is not None and selected_rc is None:
            selected_rc = selected_child.poll()
            if selected_rc is None and now - selected_started >= args.selected_timeout_seconds:
                selected_timed_out = True
                selected_termination = _terminate(selected_child)
                selected_rc = selected_child.returncode
                if selected_rc is None:
                    # Do not let a broken wait/kill path wedge the observer.
                    # The termination status remains in the receipt, while
                    # 124 is the conventional bounded-timeout sentinel.
                    selected_rc = 124
        if full_child is not None and full_rc is None:
            full_rc = full_child.poll()
        if (selected_child is None or selected_rc is not None) and (full_child is None or full_rc is not None):
            break
        time.sleep(0.05)
    if selected_stream is not None:
        selected_stream.close()
    if full_stream is not None:
        full_stream.close()
    selected_proc.update(returncode=selected_rc, seconds=round(time.monotonic() - selected_started, 1),
                         error=selected_error, cwd=str(selected_cwd), timeout_seconds=args.selected_timeout_seconds,
                         timed_out=selected_timed_out, termination=selected_termination)
    full_proc.update(returncode=full_rc, seconds=round(time.monotonic() - full_started, 1), error=full_error,
                     cwd=str(full_cwd))

    selected_plan["process"] = selected_proc
    selected_plan["outcome"] = ("success" if selected_rc == 0 else "failure"
                                 if selected_rc is not None else "unavailable")
    full = {"process": full_proc, "outcome": "success" if full_proc.get("returncode") == 0 else "failure" if full_proc.get("returncode") is not None else "unavailable"}
    out = receipt(args, selected_plan, full)
    args.receipt_out.write_text(json.dumps(out, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print("::notice title=paired-affected-tests-shadow::" + json.dumps({"schema": SCHEMA, "failed_outside_selection": out["failed_outside_selection"], "full_outcome": out["full_outcome"], "full_success": out["full_success"], "outcome_comparison": out["outcome_comparison"]}, separators=(",", ":")))
    # The selected run is advisory, but the authoritative full run remains the
    # step's truth.  Preserve its exit status and fail closed when it could not
    # start; otherwise a red full suite could be reported as green.
    return full_rc if full_rc is not None else 1


if __name__ == "__main__":
    sys.exit(main())

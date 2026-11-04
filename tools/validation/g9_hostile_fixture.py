#!/usr/bin/env python3
"""Darwin G-9 hostile-process evidence; no hosted-plugin isolation runtime."""
from __future__ import annotations

import argparse
import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path


def process_state(pid: int) -> str | None:
    """Return Darwin process state, or None after the PID disappears."""
    result = subprocess.run(["/bin/ps", "-p", str(pid), "-o", "state="],
                            capture_output=True, text=True, encoding="utf-8",
                            check=False)
    return result.stdout.strip() if result.returncode == 0 else None


def is_live_process_state(state: str | None) -> bool:
    """A listed zombie is terminated, even while its PID awaits reaping."""
    return bool(state) and "Z" not in state.upper()


def wait_gone(pid: int, seconds: float) -> bool:
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if process_state(pid) is None:
            return True
        time.sleep(0.02)
    return process_state(pid) is None


def cleanup_descendant(pid: int) -> dict:
    """Clean up the fixture child; this is harness hygiene, not containment."""
    before = process_state(pid)
    signals: list[str] = []
    if before is not None:
        try:
            os.kill(pid, signal.SIGTERM)
            signals.append("SIGTERM")
        except ProcessLookupError:
            pass
    gone = wait_gone(pid, 2)
    if not gone:
        try:
            os.kill(pid, signal.SIGKILL)
            signals.append("SIGKILL")
        except ProcessLookupError:
            pass
        gone = wait_gone(pid, 2)
    return {"state_before": before, "signals": signals,
            "gone_after_cleanup": gone, "state_after": process_state(pid)}


def run_case(name: str, timeout: float, out: Path) -> dict:
    child_code = "import time; time.sleep(30)"
    cases = {
        "crash": ["/bin/sh", "-c", "kill -ABRT $$"],
        "hang": [sys.executable, "-c", "import time; time.sleep(30)"],
        "deadline": [sys.executable, "-c", f"import time; time.sleep({timeout * 2})"],
        "descendant": [sys.executable, "-c", "import subprocess,sys,time; "
                       "p=subprocess.Popen([sys.executable,'-c'," + repr(child_code) + "],"
                       "stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL); "
                       "print(p.pid,flush=True); time.sleep(30)"],
        "negative_clean_exit": [sys.executable, "-c", "pass"],
        "negative_slow_but_within_budget":
            [sys.executable, "-c", f"import time; time.sleep({timeout / 4})"],
    }
    started = time.monotonic()
    proc = subprocess.Popen(cases[name], stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True, encoding="utf-8")
    timed_out = False
    try:
        stdout, stderr = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        proc.send_signal(signal.SIGTERM)
        try:
            stdout, stderr = proc.communicate(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()
            stdout, stderr = proc.communicate(timeout=2)
    record = {"case": name, "pid": proc.pid, "returncode": proc.returncode,
              "timed_out": timed_out,
              "elapsed_ms": round((time.monotonic() - started) * 1000, 1),
              "stdout": stdout, "stderr": stderr}
    if name == "descendant":
        try:
            descendant_pid = int(stdout.strip())
        except ValueError:
            record["descendant_error"] = "child PID handshake missing"
        else:
            record["descendant_pid"] = descendant_pid
            record["parent_gone"] = process_state(proc.pid) is None
            record["descendant_state_after_parent"] = process_state(descendant_pid)
            record["descendant_alive_after_parent"] = (
                is_live_process_state(record["descendant_state_after_parent"]))
            record["descendant_cleanup"] = cleanup_descendant(descendant_pid)
    record["detected"] = timed_out or proc.returncode != 0
    (out / f"{name}.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    return record


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=0.5)
    args = parser.parse_args()
    if sys.platform != "darwin":
        parser.error("this fixture currently measures Darwin only")
    if args.timeout < 0.3:
        parser.error("--timeout must be at least 0.3 seconds for process startup")
    args.output.mkdir(parents=True, exist_ok=True)
    names = ("crash", "hang", "deadline", "descendant", "negative_clean_exit",
             "negative_slow_but_within_budget")
    results = [run_case(name, args.timeout, args.output) for name in names]
    by_name = {item["case"]: item for item in results}
    checks = {
        "crash_detected": by_name["crash"]["detected"] and not by_name["crash"]["timed_out"],
        "hang_detected": by_name["hang"]["timed_out"],
        "deadline_detected": by_name["deadline"]["timed_out"],
        "descendant_parent_timed_out": by_name["descendant"]["timed_out"],
        "descendant_survived_parent": by_name["descendant"].get("parent_gone", False)
            and by_name["descendant"].get("descendant_alive_after_parent", False),
        "descendant_cleaned_by_fixture": by_name["descendant"].get(
            "descendant_cleanup", {}).get("gone_after_cleanup", False),
        "clean_control_abstained": by_name["negative_clean_exit"]["returncode"] == 0
            and not by_name["negative_clean_exit"]["detected"],
        "slow_control_abstained":
            by_name["negative_slow_but_within_budget"]["returncode"] == 0
            and not by_name["negative_slow_but_within_budget"]["detected"]
            and by_name["negative_slow_but_within_budget"]["elapsed_ms"] < args.timeout * 1000,
    }
    receipt = {"fixture": "G-9", "platform": sys.platform, "pid": os.getpid(),
               "timeout_s": args.timeout, "cases": results, "checks": checks,
               "pass": all(checks.values()), "isolation_implemented": False}
    (args.output / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n",
                                              encoding="utf-8")
    print(json.dumps({"pass": receipt["pass"], "checks": checks}, indent=2))
    return 0 if receipt["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

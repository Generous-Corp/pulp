#!/usr/bin/env python3
"""Evidence-only hostile hosted-plugin fixture for G-9.

This deliberately observes failure modes; it does not implement isolation.
"""
from __future__ import annotations

import argparse
import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path


CASES = {
    "crash": "import os,signal; os.kill(os.getpid(), signal.SIGABRT)",
    "hang": "import time; time.sleep(30)",
    "deadline": "import time; time.sleep(0.75)",
    "descendant": "import subprocess,os,time; child=subprocess.Popen([sys.executable,'-c','import os,time; print(os.getpid(), flush=True); time.sleep(30)']); time.sleep(30)",
}


def run_case(name: str, timeout: float, out: Path) -> dict:
    code = "import sys; " + CASES[name]
    if name == "descendant":
        code = code.replace("subprocess.Popen([sys.executable,'-c','import time; time.sleep(30)'])",
                            "subprocess.Popen([sys.executable,'-c','import time; time.sleep(30)'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)")
    started = time.monotonic()
    proc = subprocess.Popen([sys.executable, "-c", code], stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, text=True, encoding="utf-8")
    timed_out = False
    try:
        stdout, stderr = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        proc.send_signal(signal.SIGTERM)
        stdout, stderr = proc.communicate(timeout=2)
    descendant_pid = None
    descendant_alive_after_parent = False
    if name == "descendant":
        try:
            descendant_pid = int(stdout.strip().splitlines()[0])
            os.kill(descendant_pid, 0)
            descendant_alive_after_parent = True
            os.kill(descendant_pid, signal.SIGTERM)
        except (ValueError, IndexError, ProcessLookupError, PermissionError):
            pass
    record = {
        "case": name, "pid": proc.pid, "returncode": proc.returncode,
        "timed_out": timed_out, "elapsed_ms": round((time.monotonic()-started)*1000, 1),
        "stdout": stdout, "stderr": stderr,
        "descendant_pid": descendant_pid,
        "descendant_alive_after_parent": descendant_alive_after_parent,
    }
    (out / f"{name}.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    return record


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=0.1)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    results = [run_case(name, args.timeout, args.output) for name in CASES]
    # Planted-negative controls exercise the classifier's required abstentions.
    controls = [
        {"case": "negative_clean_exit", "returncode": 0, "timed_out": False,
         "expected_detection": False},
        {"case": "negative_slow_but_within_budget", "returncode": 0, "timed_out": False,
         "expected_detection": False},
    ]
    receipt = {"fixture": "G-9", "platform": sys.platform, "pid": os.getpid(),
               "cases": results, "planted_negative_controls": controls,
               "isolation_implemented": False}
    (args.output / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

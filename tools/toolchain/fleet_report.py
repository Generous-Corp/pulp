#!/usr/bin/env python3
"""Fleet drift report: run check_env.py on each host and print one table.

    python3 tools/toolchain/fleet_report.py                      # m1 m3 m5 m5s over ssh
    python3 tools/toolchain/fleet_report.py --host m1 --host m3=local --host m5 --host m5s

Each host gets this checkout's check_env.py and expected.json in a temporary
directory (removed afterwards) and runs them with its own python3 under a
fixed, non-login PATH, the one agents and lanes reach over ssh. It only reads
versions: nothing is installed or changed on any host. A host that cannot be
reached is a row of `unreachable`, never a silent gap.

Cells show the version a host runs; a cell equal to the pin is marked `=`.
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Callable, Dict, List, Optional, Tuple

HERE = Path(__file__).resolve().parent
DEFAULT_HOSTS = ("m1", "m3", "m5", "m5s")
DEFAULT_PATH = "$HOME/.local/bin:$HOME/.cargo/bin:/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin"
# Row order and the check_env tool name for each manifest key.
ROWS: Tuple[Tuple[str, str], ...] = (
    ("python3", "python"), ("cmake", "cmake"), ("ninja", "ninja"), ("node", "node"),
    ("rustc", "rust"), ("clang-format", "clang_format"), ("mise", "mise"),
)

# Runs on the host: write the two files to a temp dir, run, clean up.
PAYLOAD = """
import os, shutil, subprocess, sys, tempfile
d = tempfile.mkdtemp(prefix="pulp-fleet-report-")
try:
    os.makedirs(os.path.join(d, "tools", "toolchain"))
    for name, text in ((("check_env.py", {check_env!r}), ("expected.json", {expected!r}))):
        with open(os.path.join(d, "tools", "toolchain", name), "w", encoding="utf-8") as handle:
            handle.write(text)
    out = subprocess.run([sys.executable, os.path.join(d, "tools", "toolchain", "check_env.py"),
                          "--root", d, "--format", "json"], capture_output=True, text=True)
    sys.stdout.write(out.stdout)
    sys.stderr.write(out.stderr)
finally:
    shutil.rmtree(d, ignore_errors=True)
"""

Runner = Callable[[str, str, str], Tuple[int, str, str]]


def payload() -> str:
    return PAYLOAD.format(
        check_env=(HERE / "check_env.py").read_text(encoding="utf-8"),
        expected=(HERE / "expected.json").read_text(encoding="utf-8"),
    )


def run_on(target: str, script: str, path: str) -> Tuple[int, str, str]:
    """(exit, stdout, stderr) of `python3 -` on `target` ("local" runs here)."""
    if target == "local":
        env = dict(os.environ, PATH=os.path.expandvars(path))
        argv = ["python3", "-"]
    else:
        env = None
        argv = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", target,
                f'env PATH="{path}" python3 -']
    try:
        out = subprocess.run(argv, input=script, capture_output=True, text=True, encoding="utf-8",
                             timeout=120, check=False, env=env)
    except (OSError, subprocess.SubprocessError) as exc:
        return 255, "", str(exc)
    return out.returncode, out.stdout, out.stderr


def host_column(code: int, stdout: str, stderr: str, pins: Dict[str, str]) -> Dict[str, str]:
    """Display name -> cell for one host."""
    try:
        report = json.loads(stdout)
    except ValueError:
        reason = (stderr.strip().splitlines() or [f"exit {code}"])[-1][:60]
        return {display: f"unreachable ({reason})" for display, _ in ROWS}
    found = {f["tool"]: f for f in report.get("findings", [])}
    column = {}
    for display, key in ROWS:
        finding = found.get(display)
        if finding is None:
            column[display] = f"= {pins[key]}"
        elif finding.get("note") == "not installed" or finding.get("found") == "nothing":
            column[display] = "none"
        elif str(finding.get("found", "")).startswith("the pinned binary, installed but not on PATH"):
            column[display] = "pinned, not on PATH"
        else:
            column[display] = finding["found"]
    return column


def table(hosts: List[str], columns: Dict[str, Dict[str, str]], pins: Dict[str, str]) -> str:
    lines = ["| tool | " + " | ".join(hosts) + " | pin |", "|---" * (len(hosts) + 2) + "|"]
    for display, key in ROWS:
        cells = [columns[h][display] for h in hosts]
        lines.append(f"| {display} | " + " | ".join(cells) + f" | {pins[key]} |")
    return "\n".join(lines)


def main(argv: Optional[List[str]] = None, run: Runner = run_on) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument("--host", action="append", metavar="NAME[=TARGET]",
                        help="a host to report; TARGET is its ssh name, or `local` (default: m1 m3 m5 m5s)")
    parser.add_argument("--remote-path", default=DEFAULT_PATH,
                        help="PATH each host runs with (default: the non-login ssh PATH)")
    args = parser.parse_args(argv)
    specs = args.host or list(DEFAULT_HOSTS)
    expected = json.loads((HERE / "expected.json").read_text(encoding="utf-8"))
    pins = dict(expected["tools"], mise=expected["mise"]["version"])
    script = payload()
    names, columns = [], {}
    for spec in specs:
        name, _, target = spec.partition("=")
        names.append(name)
        columns[name] = host_column(*run(target or name, script, args.remote_path), pins)
    print(table(names, columns, pins))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

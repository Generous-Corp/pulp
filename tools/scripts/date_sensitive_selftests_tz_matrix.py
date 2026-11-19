#!/usr/bin/env python3
"""Run date-sensitive Python selftests under zones a calendar day apart.

A test that derives an expectation from the real clock while the code under test
runs on a pinned date passes until the real date crosses a window boundary, then
fails every run. Waiting for midnight UTC is not a test. Running the same
selftest with ``TZ=Pacific/Kiritimati`` (UTC+14), ``TZ=Pacific/Pago_Pago``
(UTC-11) and ``TZ=UTC`` puts the local date a day away from the UTC date for at
least one of those zones at every moment, so a local-clock dependency shows up
now rather than at the next date rollover. Each run takes about a second.

The list lives in ``date_sensitive_selftests.json``. ``tests`` gate; ``pending``
entries are printed and skipped (``--include-pending`` runs them, for checking a
fix that has not landed). The run fails if any selftest exits non-zero in any
zone, and names the zones that disagreed.

Detection limits: it exercises the local-time and process-clock paths only. A
test that reads the UTC clock (``datetime.utcnow``) in both the fixture and the
checker agrees with itself in every zone and is not caught here.

Usage::

    date_sensitive_selftests_tz_matrix.py [--config PATH] [--include-pending]
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CONFIG = ROOT / "tools/scripts/date_sensitive_selftests.json"
TIMEOUT_S = 120


def run_matrix(tests: list[str], zones: list[str], root: Path = ROOT) -> dict[str, dict[str, int]]:
    results: dict[str, dict[str, int]] = {}
    for rel in tests:
        results[rel] = {}
        for zone in zones:
            env = dict(os.environ, TZ=zone)
            proc = subprocess.run(
                [sys.executable, str(root / rel)], cwd=root, env=env,
                capture_output=True, text=True, encoding="utf-8", timeout=TIMEOUT_S,
            )
            results[rel][zone] = proc.returncode
            if proc.returncode != 0:
                tail = (proc.stdout + proc.stderr).strip().splitlines()[-8:]
                print(f"--- {rel} TZ={zone} exit {proc.returncode}\n" + "\n".join(t[:300] for t in tail), file=sys.stderr)
    return results


def failures(results: dict[str, dict[str, int]]) -> list[str]:
    out = []
    for rel, by_zone in results.items():
        bad = [z for z, rc in by_zone.items() if rc != 0]
        if bad:
            out.append(f"{rel}: failed under TZ={', '.join(bad)}")
    return out


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--config", type=Path, default=CONFIG)
    parser.add_argument("--include-pending", action="store_true")
    args = parser.parse_args(argv)
    config = json.loads(args.config.read_text(encoding="utf-8"))
    zones = config["zones"]
    tests = [e["path"] for e in config.get("tests", [])]
    pending = [e["path"] for e in config.get("pending", [])]
    if args.include_pending:
        tests += pending
    elif pending:
        print("pending (not run): " + ", ".join(pending))
    bad = failures(run_matrix(tests, zones))
    if bad:
        print("date-sensitive selftests disagree across zones:\n  " + "\n  ".join(bad), file=sys.stderr)
        print("A result that depends on the zone depends on the real clock; pin the date.", file=sys.stderr)
        return 1
    print(f"date_sensitive_selftests_tz_matrix: {len(tests)} selftest(s) x {len(zones)} zone(s) ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

#!/usr/bin/env python3
"""Proves the CLAP conformance checker: the well-behaved modules pass every
check, and each deliberately broken fixture fails exactly its own check.

usage: test_clap_conformance.py <checker> name=<module.clap> ...
"""
import json
import subprocess
import sys

EXPECT_FAIL = {
    "heavy_memory": "scan-cost",
    "heavy_cpu": "scan-cost",
    "echo": "automation-echo",
    "null_input": "input-ports",
    "state": "state-round-trip",
}


def run(checker, module):
    proc = subprocess.run([checker, module, "--json"], capture_output=True, text=True, timeout=100)
    if proc.returncode not in (0, 1):
        raise SystemExit(f"{module}: checker exit {proc.returncode}: {proc.stderr}")
    return {c["name"]: c for c in json.loads(proc.stdout)["checks"]}


def main():
    checker = sys.argv[1]
    modules = dict(a.split("=", 1) for a in sys.argv[2:])
    errors = []
    for name, module in modules.items():
        checks = run(checker, module)
        want = EXPECT_FAIL.get(name)
        for cname, c in checks.items():
            expected = "FAIL" if cname == want else None
            if expected and c["status"] != "FAIL":
                errors.append(f"{name}: {cname} should FAIL, got {c['status']} ({c['detail']})")
            if not expected and c["status"] == "FAIL":
                errors.append(f"{name}: {cname} should not fail ({c['detail']})")
            if not expected and c["status"] == "SKIP" and name in ("pulp", "good"):
                errors.append(f"{name}: {cname} was skipped ({c['detail']})")
        print(name, {k: v["status"] for k, v in checks.items()})
    if errors:
        print("\n".join(errors))
        raise SystemExit(1)


main()

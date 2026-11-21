#!/usr/bin/env python3
"""Proves the CLAP conformance checker: the well-behaved modules pass every
check, and each deliberately broken fixture fails exactly its own check.

usage: test_clap_conformance.py <checker> name=<module.clap> ...
"""
import json
import subprocess
import sys
import time

EXPECT_FAIL = {
    "heavy_memory": "scan-cost",
    "heavy_cpu": "scan-cost",
    "echo": "automation-echo",
    "echo_main": "automation-echo",
    "null_input": "input-ports",
    "state": "state-round-trip",
    "crash_teardown": "automation-echo",
}


def run(checker, module):
    proc = subprocess.run([checker, module, "--json"], capture_output=True, text=True, encoding="utf-8", timeout=100)
    if proc.returncode not in (0, 1):
        raise SystemExit(f"{module}: checker exit {proc.returncode}: {proc.stderr}")
    return {c["name"]: c for c in json.loads(proc.stdout)["checks"]}


# Fixtures that hang: the checker must still return within a bound, each
# affected check failed and reported as timed out.
HANG = {
    "hang_init": ["scan-cost", "automation-echo", "input-ports", "state-round-trip"],
    "hang_process": ["automation-echo", "input-ports", "state-round-trip"],
}
HANG_BOUND_S = 60


def check_hang(checker, name, module, errors):
    start = time.monotonic()
    proc = subprocess.run(
        [checker, module, "--json", "--timeout-seconds", "2", "--plugin-id", "dev.pulp.test.conformance-fixture"],
        capture_output=True, text=True, encoding="utf-8", timeout=HANG_BOUND_S)
    took = time.monotonic() - start
    if took > HANG_BOUND_S / 2:
        errors.append(f"{name}: took {took:.0f}s")
    checks = {c["name"]: c for c in json.loads(proc.stdout)["checks"]}
    for cname in HANG[name]:
        c = checks.get(cname)
        if not c or c["status"] != "FAIL" or "timed out" not in c["detail"]:
            errors.append(f"{name}: {cname} should FAIL with a timeout, got {c}")
    print(name, {k: v["status"] for k, v in checks.items()}, f"{took:.1f}s")
    if name == "hang_init":
        # Without --plugin-id the id lookup itself hangs: module unusable (exit 2).
        proc = subprocess.run([checker, module, "--json", "--timeout-seconds", "2"],
                              capture_output=True, text=True, encoding="utf-8", timeout=HANG_BOUND_S)
        if proc.returncode != 2 or "timed out" not in proc.stderr:
            errors.append(f"{name}: id lookup hang: exit {proc.returncode}, stderr {proc.stderr!r}")


def check_control_chars(checker, module, errors):
    proc = subprocess.run([checker, module, "--json"], capture_output=True, text=True,
                          encoding="utf-8", timeout=100)
    report = json.loads(proc.stdout)  # must be valid JSON
    if report["plugin"] != "dev.pulp.test.conformance\tfixture\nctrl":
        errors.append(f"control_chars: id not round-tripped: {report['plugin']!r}")
    print("control_chars", {c["name"]: c["status"] for c in report["checks"]})


def main():
    checker = sys.argv[1]
    modules = dict(a.split("=", 1) for a in sys.argv[2:])
    errors = []
    for name in list(modules):
        if name in HANG:
            check_hang(checker, name, modules.pop(name), errors)
        elif name == "control_chars":
            check_control_chars(checker, modules.pop(name), errors)
    for name, module in modules.items():
        checks = run(checker, module)
        want = EXPECT_FAIL.get(name)
        for cname, c in checks.items():
            expected = "FAIL" if cname == want else None
            if expected and c["status"] != "FAIL":
                errors.append(f"{name}: {cname} should FAIL, got {c['status']} ({c['detail']})")
            if not expected and c["status"] == "FAIL":
                errors.append(f"{name}: {cname} should not fail ({c['detail']})")
            if name == "crash_teardown" and cname == "automation-echo" and "crashed" not in c["detail"]:
                errors.append(f"{name}: crash not reported against its check: {c['detail']}")
            if not expected and c["status"] == "SKIP" and name in ("pulp", "good"):
                errors.append(f"{name}: {cname} was skipped ({c['detail']})")
        print(name, {k: v["status"] for k, v in checks.items()})
    if errors:
        print("\n".join(errors))
        raise SystemExit(1)


main()

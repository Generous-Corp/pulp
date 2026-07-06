#!/usr/bin/env python3
"""Tests for scheduled_workflow_fork_guard_check.py."""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "scheduled_workflow_fork_guard_check.py")

GUARD = "github.event_name != 'schedule' || github.repository == 'danielraffel/pulp'"

GUARDED = f"""\
name: monitor
on:
  schedule:
    - cron: '0 6 * * *'
  workflow_dispatch:
jobs:
  check:
    if: {GUARD}
    runs-on: ubuntu-latest
    steps:
      - run: echo hi
"""

UNGUARDED = """\
name: monitor
on:
  schedule:
    - cron: '0 6 * * *'
jobs:
  check:
    runs-on: ubuntu-latest
    steps:
      - run: echo hi
"""

COMPOSED = f"""\
name: monitor
on:
  schedule:
    - cron: '0 6 * * *'
  push:
jobs:
  check:
    if: (github.event_name != 'schedule' || github.repository == 'danielraffel/pulp') && (github.event_name != 'pull_request')
    runs-on: ubuntu-latest
    steps:
      - run: echo hi
"""

NON_SCHEDULED = """\
name: on-pr
on:
  pull_request:
jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - run: echo hi
"""

DEPENDENT_OK = f"""\
name: monitor
on:
  schedule:
    - cron: '0 6 * * *'
jobs:
  root:
    if: {GUARD}
    runs-on: ubuntu-latest
    steps:
      - run: echo hi
  child:
    needs: root
    runs-on: ubuntu-latest
    steps:
      - run: echo hi
"""

EXEMPT = """\
# fork-guard-exempt: this monitor is meant to run on forks too
name: monitor
on:
  schedule:
    - cron: '0 6 * * *'
jobs:
  check:
    runs-on: ubuntu-latest
    steps:
      - run: echo hi
"""


def run(*files):
    return subprocess.run(
        [sys.executable, SCRIPT, *files],
        capture_output=True, text=True,
    )


def write(d, name, content):
    p = os.path.join(d, name)
    open(p, "w").write(content)
    return p


def main():
    passed = failed = 0

    def check(name, content, expect_ok):
        nonlocal passed, failed
        with tempfile.TemporaryDirectory() as d:
            p = write(d, "wf.yml", content)
            r = run(p)
            ok = (r.returncode == 0)
            if ok == expect_ok:
                passed += 1
                print(f"  [PASS] {name}")
            else:
                failed += 1
                print(f"  [FAIL] {name}: rc={r.returncode} want_ok={expect_ok}\n{r.stderr}")

    print("== scheduled_workflow_fork_guard_check ==")
    check("guarded scheduled workflow passes", GUARDED, True)
    check("unguarded scheduled workflow fails", UNGUARDED, False)
    check("composed guard passes", COMPOSED, True)
    check("non-scheduled workflow ignored", NON_SCHEDULED, True)
    check("dependent job needs no guard (root guarded)", DEPENDENT_OK, True)
    check("exempt marker skips the check", EXEMPT, True)

    # Multi-file: one bad among good -> overall fail, and the bad file is named.
    with tempfile.TemporaryDirectory() as d:
        good = write(d, "good.yml", GUARDED)
        bad = write(d, "bad.yml", UNGUARDED)
        r = run(good, bad)
        if r.returncode == 1 and "bad.yml" in r.stderr and "good.yml" not in r.stderr:
            passed += 1
            print("  [PASS] multi-file names only the offender")
        else:
            failed += 1
            print(f"  [FAIL] multi-file: rc={r.returncode}\n{r.stderr}")

    print(f"\nscheduled_workflow_fork_guard_check: {passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

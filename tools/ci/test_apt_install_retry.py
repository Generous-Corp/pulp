#!/usr/bin/env python3
"""Selftest for tools/ci/apt-install-retry.sh using a stubbed apt-get."""

import os
import pathlib
import subprocess
import tempfile
import unittest

HELPER = pathlib.Path(__file__).with_name("apt-install-retry.sh")

STUB = """#!/usr/bin/env bash
echo "$*" >> "$STUB_LOG"
case "$1" in
  update) exit 0 ;;
  install)
    n=$(grep -c '^install ' "$STUB_LOG")
    if [ "$n" -le "${STUB_FAIL_INSTALLS:-0}" ]; then
      echo "E: Failed to fetch http://security.ubuntu.com/x.deb 404 Not Found" >&2
      exit 100
    fi
    exit 0 ;;
esac
"""


class AptInstallRetry(unittest.TestCase):
    def run_helper(self, fail_installs, *args):
        with tempfile.TemporaryDirectory() as d:
            d = pathlib.Path(d)
            stub = d / "apt-get"
            stub.write_text(STUB, encoding="utf-8")
            stub.chmod(0o755)
            log = d / "log"
            log.write_text("", encoding="utf-8")
            env = dict(
                os.environ,
                PATH=f"{d}:{os.environ['PATH']}",
                STUB_LOG=str(log),
                STUB_FAIL_INSTALLS=str(fail_installs),
                PULP_APT_RETRY_DELAY_SECS="0",
            )
            r = subprocess.run(
                ["bash", str(HELPER), *(args or ("pkg",))],
                env=env, capture_output=True, text=True, encoding="utf-8",
            )
            return r, log.read_text(encoding="utf-8").splitlines()

    def test_recovers_after_one_404(self):
        r, calls = self.run_helper(1)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual([c.split()[0] for c in calls],
                         ["update", "install", "update", "install"])

    def test_gives_up_after_three_attempts(self):
        r, calls = self.run_helper(99)
        self.assertNotEqual(r.returncode, 0)
        self.assertEqual(sum(c.startswith("install") for c in calls), 3)
        self.assertEqual(sum(c.startswith("update") for c in calls), 3)

    def test_first_try_success_runs_once(self):
        r, calls = self.run_helper(0, "a", "b")
        self.assertEqual(r.returncode, 0)
        self.assertEqual(len(calls), 2)
        self.assertIn("a b", calls[1])

    def test_no_packages_is_usage_error(self):
        with tempfile.TemporaryDirectory():
            r = subprocess.run(["bash", str(HELPER)], capture_output=True, text=True, encoding="utf-8")
        self.assertEqual(r.returncode, 2)


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Tests for tools/scripts/live_build_check.py.

The whole value of this check is one discrimination: a marker whose pid is alive
means a build is running here, and a marker whose pid is gone means a build died
here. The file looks the same in both cases — it necessarily outlives a SIGKILL,
which is exactly how the sibling build-dir-sentinel detects interrupted builds —
so a check that keys on presence rather than liveness is wrong in both directions
at once. It cries wolf on every dead build until someone stops reading it, and it
is silent on precisely the case it exists to catch.

Both directions are asserted here, because a one-directional test would pass on a
check that always warns.

The third direction is what the check DOES about a dead marker. Reporting one and
leaving it re-proves the same finding on every run forever, which reads the same
as a check that passes — so the dead-marker tests assert the file is gone, that a
second run is silent, and that the reaper never touches a live build's marker.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

CHECK = Path(__file__).with_name("live_build_check.py")


def dead_pid() -> int:
    """A pid that has certainly exited (spawned and reaped here)."""
    proc = subprocess.Popen(["true"])
    proc.wait()
    try:
        os.kill(proc.pid, 0)
    except ProcessLookupError:
        return proc.pid
    raise unittest.SkipTest("could not obtain a reliably-dead pid")


class LiveBuildCheckTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.marker = self.root / ".pulp-build-active"

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _run(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, str(CHECK), "--root", str(self.root), *args],
            capture_output=True, text=True, check=False,
        )

    def _write(self, pid: int, *, age_secs: int = 0, jobs: str = "3") -> None:
        started = int(time.time()) - age_secs
        self.marker.write_text(
            f"pid={pid}\n"
            f"started_at=2026-08-16T12:53:00Z\n"
            f"started_epoch={started}\n"
            f"jobs={jobs}\n"
            f"lease=test-lease\n"
            f"command=cmake --build build\n",
            encoding="utf-8",
        )

    # --- the discrimination this check exists for ----------------------------

    def test_live_marker_warns(self) -> None:
        self._write(os.getpid(), age_secs=7200)
        r = self._run()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("GOVERNED BUILD IS RUNNING", r.stdout)
        self.assertIn(str(os.getpid()), r.stdout)

    def test_dead_marker_does_not_warn(self) -> None:
        """A build that already died must not raise an alarm about itself."""
        self._write(dead_pid())
        r = self._run()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertNotIn("GOVERNED BUILD IS RUNNING", r.stdout)
        self.assertIn("stale marker", r.stdout)

    # --- the finding has to change something ---------------------------------

    def test_dead_marker_is_reaped(self) -> None:
        """Proof of death is acted on, not filed."""
        self._write(dead_pid())
        r = self._run()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertFalse(
            self.marker.exists(),
            "check proved the marker dead and left it in the tree")
        self.assertIn("reaped stale marker", r.stdout)

    def test_reaped_marker_reports_what_was_building_before_deleting(self) -> None:
        """The file is about to go; the forensics must survive in the output."""
        pid = dead_pid()
        self._write(pid)
        r = self._run()
        self.assertIn(str(pid), r.stdout)
        self.assertIn("cmake --build build", r.stdout)

    def test_second_run_after_reap_is_silent(self) -> None:
        """The loop closes: the advisory appears once, not on every push."""
        self._write(dead_pid())
        first = self._run("--quiet-when-idle")
        self.assertEqual(first.returncode, 0, first.stderr)
        self.assertIn("reaped stale marker", first.stdout)

        second = self._run("--quiet-when-idle")
        self.assertEqual(second.returncode, 0, second.stderr)
        self.assertEqual(second.stdout.strip(), "")
        self.assertNotIn("stale marker", second.stdout + second.stderr)

    # --- the reaper must never touch a marker it has not proven dead ---------

    def test_live_marker_is_never_reaped(self) -> None:
        """The destructive branch is unreachable while the owner is alive."""
        self._write(os.getpid())
        r = self._run()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue(self.marker.exists(),
                        "reaped the marker of a build that is still running")

    def test_malformed_marker_is_not_reaped(self) -> None:
        """No liveness claim was established, so there is nothing to act on."""
        self.marker.write_text("started_at=2026-08-16T12:53:00Z\n", encoding="utf-8")
        r = self._run()
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
        self.assertTrue(self.marker.exists(),
                        "deleted the evidence of a writer defect")

    # --- a reap that cannot happen must not read as a clean run --------------

    def test_unreapable_dead_marker_escalates(self) -> None:
        if os.geteuid() == 0:
            self.skipTest("root bypasses directory permissions")
        self._write(dead_pid())
        os.chmod(self.root, 0o500)
        try:
            r = self._run()
        finally:
            os.chmod(self.root, 0o700)
        self.assertEqual(r.returncode, 3, r.stdout + r.stderr)
        self.assertTrue(self.marker.exists())
        self.assertIn("could not be removed", r.stderr)

    def test_no_marker_is_silent_when_asked(self) -> None:
        r = self._run("--quiet-when-idle")
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(r.stdout.strip(), "")

    def test_no_marker_reports_when_not_quiet(self) -> None:
        r = self._run()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("no governed build active", r.stdout)

    # --- it must never be the reason a push fails ----------------------------

    def test_live_build_still_exits_zero(self) -> None:
        self._write(os.getpid())
        self.assertEqual(self._run().returncode, 0)

    # --- a malformed marker is a writer defect, reported as such -------------

    def test_marker_without_a_pid_is_an_error_not_a_finding(self) -> None:
        self.marker.write_text("started_at=2026-08-16T12:53:00Z\n", encoding="utf-8")
        r = self._run()
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
        self.assertIn("no usable pid", r.stderr)

    # --- reporting detail the operator acts on -------------------------------

    def test_warning_reports_age_and_parallelism(self) -> None:
        self._write(os.getpid(), age_secs=7200, jobs="3")
        r = self._run()
        self.assertIn("120m", r.stdout)
        self.assertIn("-j3", r.stdout)


if __name__ == "__main__":
    unittest.main()

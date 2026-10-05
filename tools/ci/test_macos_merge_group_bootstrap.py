#!/usr/bin/env python3
"""The merge-group `macos` bootstrap verdict, run against stub dependency results.

A dependency that failed, or has no result, fails the gate closed. One that was
cancelled (the queue re-batched, or no runner was ever acquired) cancels the
run instead: failing closed there made GitHub eject the PR, re-batch, and meet
the same wait again. The script must never exit 0 on a dependency that did
not succeed.
"""

from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().with_name("macos_merge_group_bootstrap.sh")


def run(provider: str, classify: str, *, native: str = "false", receipt: str = "",
        reused: str = "") -> tuple[int, str, bool]:
    with tempfile.TemporaryDirectory() as td:
        marker = Path(td) / "cancel-requested"
        env = {k: v for k, v in os.environ.items() if not k.startswith(("GITHUB_", "GH_"))}
        env.update({
            "PROVIDER_RESULT": provider, "CLASSIFY_RESULT": classify,
            "NATIVE_REQUIRED": native, "RECEIPT_RESULT": receipt, "RECEIPT_REUSED": reused,
            "GITHUB_RUN_ID": "4242", "CANCEL_WAIT_SECONDS": "0",
            "GITHUB_STEP_SUMMARY": str(Path(td) / "summary.md"),
            "BOOTSTRAP_CANCEL_CMD": f"touch {marker}",
        })
        proc = subprocess.run(["bash", str(SCRIPT)], env=env, capture_output=True, text=True,
                              timeout=30)
        return proc.returncode, proc.stdout + proc.stderr, marker.exists()


class VerdictTests(unittest.TestCase):
    def test_success_proceeds_skip_safe_and_receipt_reused(self) -> None:
        rc, out, cancelled = run("success", "success")
        self.assertEqual((rc, cancelled), (0, False), out)
        self.assertIn("Skip-safe merge group", out)
        rc, out, cancelled = run("success", "success", native="true", receipt="success",
                                 reused="true")
        self.assertEqual((rc, cancelled), (0, False), out)
        self.assertIn("protected PR receipt revalidated", out)

    def test_failure_fails_closed_and_never_cancels(self) -> None:
        for provider, classify in (("failure", "success"), ("success", "failure"),
                                   ("failure", "cancelled"), ("cancelled", "failure")):
            with self.subTest(provider=provider, classify=classify):
                rc, out, cancelled = run(provider, classify)
                self.assertEqual((rc, cancelled), (1, False), out)
                self.assertIn("failing macos gate closed", out)

    def test_missing_or_skipped_result_fails_closed(self) -> None:
        for provider, classify in (("", "success"), ("success", ""), ("skipped", "success")):
            with self.subTest(provider=provider, classify=classify):
                rc, out, cancelled = run(provider, classify)
                self.assertEqual((rc, cancelled), (1, False), out)
                self.assertIn("failing macos gate closed", out)

    def test_cancelled_dependency_cancels_the_run(self) -> None:
        for provider, classify in (("cancelled", "success"), ("success", "cancelled"),
                                   ("cancelled", "cancelled")):
            with self.subTest(provider=provider, classify=classify):
                rc, out, cancelled = run(provider, classify)
                self.assertTrue(cancelled, out)
                self.assertIn("cancel requested for run 4242", out)
                # The cancel never took hold here, so it still fails closed:
                # a cancelled dependency is never a pass.
                self.assertEqual(rc, 1, out)
                self.assertIn("was not cancelled within 0s", out)

    def test_a_cancel_request_that_fails_still_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            env = {**os.environ, "PROVIDER_RESULT": "cancelled", "CLASSIFY_RESULT": "success",
                   "CANCEL_WAIT_SECONDS": "0", "BOOTSTRAP_CANCEL_CMD": "exit 7",
                   "GITHUB_RUN_ID": "4242", "GITHUB_STEP_SUMMARY": str(Path(td) / "s")}
            proc = subprocess.run(["bash", str(SCRIPT)], env=env, capture_output=True,
                                  text=True, timeout=30)
        self.assertEqual(proc.returncode, 1, proc.stdout)
        self.assertIn("could not request cancellation", proc.stdout)

    def test_receipt_unavailable_fails_closed(self) -> None:
        rc, out, cancelled = run("success", "success", native="true", receipt="failure")
        self.assertEqual((rc, cancelled), (1, False), out)
        self.assertIn("protected receipt decision unavailable", out)


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""The merge-group `macos` bootstrap verdict, run against stub dependency results.

A dependency that failed, was skipped, or has no result fails the gate closed.
A cancelled classify (GitHub never assigned the preamble a runner) is
classified in-job instead, and the verdict proceeds on that answer; a failing
in-job classifier fails closed. The script must never exit 0 on a dependency
that failed.
"""

from __future__ import annotations

import os
import subprocess
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[2] / "tools/ci/macos_merge_group_bootstrap.sh"


def run(provider: str, classify: str, *, native: str = "false", receipt: str = "",
        reused: str = "", fallback: str = "native_build_required=false",
        fallback_rc: int = 0) -> tuple[int, str, bool]:
    with tempfile.TemporaryDirectory() as td:
        marker = Path(td) / "classified-in-job"
        env = {k: v for k, v in os.environ.items() if not k.startswith("GITHUB_")}
        env.update({
            "PROVIDER_RESULT": provider, "CLASSIFY_RESULT": classify,
            "NATIVE_REQUIRED": native, "RECEIPT_RESULT": receipt, "RECEIPT_REUSED": reused,
            "GITHUB_STEP_SUMMARY": str(Path(td) / "summary.md"),
            "CLASSIFY_FALLBACK_CMD": f"touch {marker}; echo progress; echo {fallback}; exit {fallback_rc}",
        })
        proc = subprocess.run(["bash", str(SCRIPT)], env=env, capture_output=True, text=True,
                              timeout=30)
        return proc.returncode, proc.stdout + proc.stderr, marker.exists()


class VerdictTests(unittest.TestCase):
    def test_success_proceeds_without_classifying_again(self) -> None:
        rc, out, classified = run("success", "success")
        self.assertEqual((rc, classified), (0, False), out)
        self.assertIn("Skip-safe merge group", out)
        rc, out, classified = run("success", "success", native="true", receipt="success",
                                  reused="true")
        self.assertEqual((rc, classified), (0, False), out)
        self.assertIn("protected PR receipt revalidated", out)

    def test_failure_fails_closed_and_never_classifies(self) -> None:
        for provider, classify in (("failure", "success"), ("success", "failure"),
                                   ("failure", "cancelled"), ("cancelled", "failure")):
            with self.subTest(provider=provider, classify=classify):
                rc, out, classified = run(provider, classify)
                self.assertEqual((rc, classified), (1, False), out)
                self.assertIn("failing macos gate closed", out)

    def test_missing_or_skipped_result_fails_closed(self) -> None:
        for provider, classify in (("", "success"), ("success", ""), ("skipped", "success"),
                                   ("success", "skipped")):
            with self.subTest(provider=provider, classify=classify):
                rc, out, classified = run(provider, classify)
                self.assertEqual((rc, classified), (1, False), out)
                self.assertIn("failing macos gate closed", out)

    def test_cancelled_classify_is_classified_in_job_and_proceeds(self) -> None:
        for provider in ("success", "cancelled"):
            with self.subTest(provider=provider):
                rc, out, classified = run(provider, "cancelled", native="")
                self.assertTrue(classified, out)
                self.assertIn("preamble not acquired (infrastructure)", out)
                self.assertIn("in-job classification: native_build_required=false", out)
                self.assertIn("Skip-safe merge group", out)
                self.assertEqual(rc, 0, out)

    def test_a_classification_that_was_never_published_is_classified_in_job(self) -> None:
        rc, out, classified = run("success", "success", native="")
        self.assertTrue(classified, out)
        self.assertEqual(rc, 0, out)

    def test_cancelled_classify_with_a_failing_classifier_fails_closed(self) -> None:
        rc, out, classified = run("success", "cancelled", native="", fallback_rc=3)
        self.assertTrue(classified, out)
        self.assertEqual(rc, 1, out)
        self.assertIn("the in-job classifier failed", out)
        rc, out, _ = run("success", "cancelled", native="", fallback="nonsense")
        self.assertEqual(rc, 1, out)
        self.assertIn("gave no classification", out)

    def test_cancelled_classify_that_needs_the_native_build_fails_closed(self) -> None:
        # The native leg and the receipt reuse both wait on classify, so a
        # group that needs the native build cannot be validated here.
        rc, out, classified = run("success", "cancelled", native="",
                                  fallback="native_build_required=true")
        self.assertTrue(classified, out)
        self.assertEqual(rc, 1, out)
        self.assertIn("requires the native build", out)

    def test_cancelled_provider_with_a_native_group_fails_closed(self) -> None:
        rc, out, classified = run("cancelled", "success", native="true", receipt="skipped")
        self.assertEqual((rc, classified), (1, False), out)
        self.assertIn("protected receipt decision unavailable", out)

    def test_receipt_unavailable_fails_closed(self) -> None:
        rc, out, classified = run("success", "success", native="true", receipt="failure")
        self.assertEqual((rc, classified), (1, False), out)
        self.assertIn("protected receipt decision unavailable", out)


if __name__ == "__main__":
    unittest.main()

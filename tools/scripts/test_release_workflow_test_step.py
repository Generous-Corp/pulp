#!/usr/bin/env python3
"""Regression test for release-pipeline test-step exclusions.

Issue #720: sign-and-release.yml ran the full ctest suite, which includes
auval-Pulp* validation tests. On hosted GitHub macOS runners, the
freshly-installed .component bundle is not picked up by the
AudioComponentRegistrar consistently, so auval returns "Cannot get
Component's Name strings / Error -50" and the pipeline fails before it
ever reaches the sign / notarize / publish steps.

The dedicated `validate.yml` workflow already owns those validation gates
on PRs (with the documented codesigning caveat). Removing them from
sign-and-release.yml is the correct separation of concerns.

This test asserts the workflow keeps using `-LE validation` so a future
edit cannot silently re-introduce the failure mode that lost 30+
consecutive sign-and-release runs.

Run:
    python3 tools/scripts/test_release_workflow_test_step.py
"""

from __future__ import annotations

import re
import sys
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
SIGN_AND_RELEASE = REPO_ROOT / ".github" / "workflows" / "sign-and-release.yml"


class SignAndReleaseTestStep(unittest.TestCase):
    """sign-and-release.yml must skip validation-labeled tests."""

    def setUp(self) -> None:
        self.assertTrue(
            SIGN_AND_RELEASE.exists(),
            f"missing workflow file: {SIGN_AND_RELEASE}",
        )
        self.text = SIGN_AND_RELEASE.read_text()

    def _find_test_step_run(self) -> str:
        """Return the shell text under the `name: Test` step."""
        # Match the Test step block: `name: Test` followed by an optional
        # comment block, then `run:`. The run can be a single line
        # (`run: ctest ...`) or a literal block (`run: |`).
        pattern = re.compile(
            r"-\s*name:\s*Test\s*\n"            # step header
            r"(?:\s*#[^\n]*\n)*"               # optional comment lines
            r"\s*run:\s*(.+?)(?=\n\s*-\s*name:|\Z)",
            re.DOTALL,
        )
        match = pattern.search(self.text)
        self.assertIsNotNone(
            match,
            "could not locate the `name: Test` step in sign-and-release.yml",
        )
        return match.group(1)

    def test_test_step_invokes_ctest(self) -> None:
        run_block = self._find_test_step_run()
        self.assertIn(
            "ctest",
            run_block,
            "sign-and-release Test step should still invoke ctest",
        )

    def test_test_step_excludes_validation_label(self) -> None:
        """Regression for #720.

        The Test step must pass `-LE validation` to ctest (or otherwise
        explicitly skip the validation label) so auval / pluginval /
        clap-validator failures on hosted GH runners do not silently
        break the sign-and-release pipeline.
        """
        run_block = self._find_test_step_run()
        # Accept either the short `-LE validation` form or the long
        # `--label-exclude validation` form so future edits have
        # flexibility.
        has_short = re.search(r"-LE\s+validation", run_block)
        has_long = re.search(r"--label-exclude\s+validation", run_block)
        self.assertTrue(
            has_short or has_long,
            "sign-and-release Test step must exclude the `validation` ctest "
            "label (issue #720). Without `-LE validation`, auval failures on "
            "hosted GitHub macOS runners break the entire release pipeline. "
            f"Found run block:\n{run_block}",
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)

#!/usr/bin/env python3
"""
Unit tests for auto_release_decision.decide().

Covers the three historical auto-release bug classes (#498, #501, #513)
plus baseline correctness cases. Run with:

    python3 -m pytest tools/scripts/test_auto_release_decision.py -v

or without pytest:

    python3 tools/scripts/test_auto_release_decision.py
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import pathlib
import runpy
import sys
import unittest
from unittest import mock

SCRIPT = pathlib.Path(__file__).parent / "auto_release_decision.py"

spec = importlib.util.spec_from_file_location(
    "auto_release_decision",
    SCRIPT,
)
ard = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(ard)


class SemverCmpTests(unittest.TestCase):
    def test_parse_version_accepts_numeric_triplets(self):
        self.assertEqual(ard.parse_version("1.2.3"), (1, 2, 3))
        self.assertEqual(ard.parse_version("01.002.0003"), (1, 2, 3))

    def test_parse_version_rejects_missing_or_malformed_values(self):
        self.assertIsNone(ard.parse_version(""))
        self.assertIsNone(ard.parse_version(None))
        self.assertIsNone(ard.parse_version("1.2"))
        self.assertIsNone(ard.parse_version("1.2.3.4"))
        self.assertIsNone(ard.parse_version("1.two.3"))

    def test_strict_greater(self):
        self.assertEqual(ard.semver_cmp("0.23.1", "0.23.0"), "gt")
        self.assertEqual(ard.semver_cmp("1.0.0", "0.99.99"), "gt")

    def test_equal(self):
        self.assertEqual(ard.semver_cmp("0.23.1", "0.23.1"), "eq")

    def test_less(self):
        self.assertEqual(ard.semver_cmp("0.22.0", "0.23.0"), "lt")

    def test_empty_is_less(self):
        self.assertEqual(ard.semver_cmp("", "0.1.0"), "lt")
        self.assertEqual(ard.semver_cmp(None, "0.1.0"), "lt")

    def test_empty_vs_empty(self):
        self.assertEqual(ard.semver_cmp("", ""), "eq")
        self.assertEqual(ard.semver_cmp(None, None), "eq")

    def test_malformed(self):
        # Not strictly required, but document behavior
        self.assertEqual(ard.semver_cmp("not.a.version", "0.1.0"), "lt")
        self.assertEqual(ard.semver_cmp("0.1", "0.1.0"), "lt")
        self.assertEqual(ard.semver_cmp("1.0.0", "latest"), "gt")
        self.assertEqual(ard.semver_cmp("broken", "also-broken"), "eq")


class DecideTests(unittest.TestCase):
    """Core decision-matrix cases."""

    # ── Self-heal after cascade-cancel (#513 / v0.23.1 recovery) ─────────
    def test_self_heal_tags_when_tag_is_behind_head(self):
        """#509 bumped 0.23.0→0.23.1, but auto-release for that push was
        broken (#510 YAML bug). After #510 merged, next auto-release run
        sees HEAD=0.23.1 > tag=0.23.0 and must tag — the bump commit
        did NOT carry Release: skip, so the tag is sanctioned."""
        result = ard.decide(
            head_version="0.23.1",
            tag_version="0.23.0",
            bump_commit_has_skip=False,
            surface="sdk",
        )
        self.assertEqual(result["should_tag"], 1)
        self.assertIn("tagging", result["reason"])

    # ── Sticky skip invariant ────────────────────────────────────────────
    def test_sticky_skip_blocks_tag_permanently(self):
        """If the bump commit carries Release: skip, even on a later
        unrelated push (prev==HEAD==0.23.1, tag=0.22.0) the decision is
        still NO TAG. The skip is a property of the bump commit, not
        the current push."""
        result = ard.decide(
            head_version="0.23.1",
            tag_version="0.22.0",
            bump_commit_has_skip=True,
            surface="sdk",
        )
        self.assertEqual(result["should_tag"], 0)
        self.assertIn("sticky skip", result["reason"])

    def test_sticky_skip_still_blocks_when_bump_is_immediate(self):
        """The push that introduces the bump + skip trailer should
        itself not tag."""
        result = ard.decide(
            head_version="0.23.1",
            tag_version="0.23.0",
            bump_commit_has_skip=True,
            surface="sdk",
        )
        self.assertEqual(result["should_tag"], 0)

    # ── Revert / downgrade ───────────────────────────────────────────────
    def test_downgrade_does_not_tag(self):
        """Reverting 0.23.1 back to 0.22.0 must not tag — the old
        v0.23.1 tag should keep pointing at the pre-revert commit.
        Manual cleanup is correct for downgrades."""
        result = ard.decide(
            head_version="0.22.0",
            tag_version="0.23.1",
            bump_commit_has_skip=False,
            surface="sdk",
        )
        self.assertEqual(result["should_tag"], 0)
        self.assertIn("revert", result["reason"].lower())

    # ── No-op push (most common case) ────────────────────────────────────
    def test_no_op_push_when_already_tagged(self):
        """Unrelated push after the tag is created. HEAD-version matches
        latest tag. Nothing to do."""
        result = ard.decide(
            head_version="0.23.1",
            tag_version="0.23.1",
            bump_commit_has_skip=False,
            surface="sdk",
        )
        self.assertEqual(result["should_tag"], 0)
        self.assertIn("already tagged", result["reason"])

    # ── First release in a repo ──────────────────────────────────────────
    def test_first_release_tags(self):
        """Empty tag history + any valid head version → tag."""
        result = ard.decide(
            head_version="0.1.0",
            tag_version="",
            bump_commit_has_skip=False,
            surface="sdk",
        )
        self.assertEqual(result["should_tag"], 1)

    def test_first_release_with_none_tag(self):
        result = ard.decide(
            head_version="0.1.0",
            tag_version=None,
            bump_commit_has_skip=False,
            surface="sdk",
        )
        self.assertEqual(result["should_tag"], 1)

    def test_valid_head_with_malformed_tag_is_treated_as_first_release(self):
        result = ard.decide(
            head_version="0.2.0",
            tag_version="not-a-semver",
            bump_commit_has_skip=False,
            surface="plugin",
        )
        self.assertEqual(result["should_tag"], 1)
        self.assertIn("vnot-a-semver", result["reason"])

    # ── Empty head version ───────────────────────────────────────────────
    def test_missing_head_version_does_not_tag(self):
        """Defensive: if extraction failed, don't tag."""
        result = ard.decide(
            head_version="",
            tag_version="0.23.0",
            bump_commit_has_skip=False,
            surface="sdk",
        )
        self.assertEqual(result["should_tag"], 0)
        self.assertIn("no sdk version", result["reason"].lower())

    # ── Per-surface labeling ─────────────────────────────────────────────
    def test_surface_appears_in_reason(self):
        for surface in ("sdk", "plugin", "some-future-surface"):
            result = ard.decide(
                head_version="0.1.0",
                tag_version="",
                bump_commit_has_skip=False,
                surface=surface,
            )
            self.assertEqual(result["surface"], surface)
            self.assertIn(surface, result["reason"])


class HistoricalBugRegressions(unittest.TestCase):
    """Explicit regression gates for the three prior auto-release bugs."""

    def test_498_self_heal_still_works(self):
        """#498 motivation: cascade-cancel leaves prev==HEAD but tag
        behind. New decision: tag based on head>tag, ignore prev."""
        result = ard.decide(
            head_version="0.23.1",
            tag_version="0.22.0",
            bump_commit_has_skip=False,
            surface="sdk",
        )
        self.assertEqual(result["should_tag"], 1, "#498 self-heal regressed")

    def test_501_sticky_skip_still_works(self):
        """#501 motivation: skip trailer must stick across unrelated
        pushes. Bump-commit-trailer check preserves this."""
        result = ard.decide(
            head_version="0.23.1",
            tag_version="0.22.0",
            bump_commit_has_skip=True,
            surface="sdk",
        )
        self.assertEqual(result["should_tag"], 0, "#501 sticky skip regressed")

    def test_501_revert_still_works(self):
        """#501 motivation: downgrade shouldn't re-tag old version."""
        result = ard.decide(
            head_version="0.22.0",
            tag_version="0.23.0",
            bump_commit_has_skip=False,
            surface="sdk",
        )
        self.assertEqual(result["should_tag"], 0, "#501 revert semantics regressed")

    def test_513_v0231_recovery_case(self):
        """#513 exact scenario: on 2026-04-20, #509 merged (bumped to
        0.23.1) but auto-release was broken by #501's YAML bug. #510
        fixed the YAML. Next push (#515 merge) should have tagged
        v0.23.1 but didn't because #501's logic required prev<HEAD,
        which was false after the bump landed one commit earlier.
        Correct behavior now: tag."""
        result = ard.decide(
            head_version="0.23.1",
            tag_version="0.23.0",
            bump_commit_has_skip=False,
            surface="sdk",
        )
        self.assertEqual(result["should_tag"], 1, "#513 v0.23.1 recovery regressed")


class CliContractTests(unittest.TestCase):
    """Exercise the JSON CLI used by auto-release.yml."""

    def run_main(self, *args: str) -> tuple[int, dict]:
        stdout = io.StringIO()
        with mock.patch.object(sys, "argv", ["auto_release_decision.py", *args]), \
             contextlib.redirect_stdout(stdout):
            rc = ard.main()
        return rc, json.loads(stdout.getvalue())

    def test_cli_outputs_json_for_tagging_decision(self):
        rc, result = self.run_main(
            "--head-version", "0.24.0",
            "--tag-version", "0.23.9",
            "--bump-commit-has-skip", "0",
            "--surface", "plugin",
        )

        self.assertEqual(rc, 0)
        self.assertEqual(result["should_tag"], 1)
        self.assertEqual(result["surface"], "plugin")
        self.assertIn("tagging", result["reason"])

    def test_cli_converts_skip_choice_to_bool(self):
        rc, result = self.run_main(
            "--head-version", "0.24.0",
            "--tag-version", "0.23.9",
            "--bump-commit-has-skip", "1",
            "--surface", "sdk",
        )

        self.assertEqual(rc, 0)
        self.assertEqual(result["should_tag"], 0)
        self.assertIn("sticky skip", result["reason"])

    def test_cli_defaults_to_missing_sdk_version(self):
        rc, result = self.run_main()

        self.assertEqual(rc, 0)
        self.assertEqual(result["should_tag"], 0)
        self.assertEqual(result["surface"], "sdk")
        self.assertIn("no sdk version", result["reason"].lower())

    def test_cli_empty_tag_version_tags_valid_head(self):
        rc, result = self.run_main(
            "--head-version", "1.0.0",
            "--tag-version", "",
            "--surface", "sdk",
        )

        self.assertEqual(rc, 0)
        self.assertEqual(result["should_tag"], 1)
        self.assertIn("v<none>", result["reason"])

    def test_cli_rejects_non_binary_skip_choice(self):
        stderr = io.StringIO()
        with mock.patch.object(
            sys,
            "argv",
            ["auto_release_decision.py", "--bump-commit-has-skip", "2"],
        ), contextlib.redirect_stderr(stderr):
            with self.assertRaises(SystemExit) as cm:
                ard.main()

        self.assertEqual(cm.exception.code, 2)
        self.assertIn("invalid choice", stderr.getvalue())

    def test_cli_help_exits_zero(self):
        stdout = io.StringIO()
        with mock.patch.object(sys, "argv", ["auto_release_decision.py", "--help"]), \
             contextlib.redirect_stdout(stdout):
            with self.assertRaises(SystemExit) as cm:
                ard.main()

        self.assertEqual(cm.exception.code, 0)
        self.assertIn("Decide whether auto-release.yml", stdout.getvalue())

    def test_script_entrypoint_prints_json_and_exits_zero(self):
        stdout = io.StringIO()
        with mock.patch.object(
            sys,
            "argv",
            [
                str(SCRIPT),
                "--head-version", "0.24.0",
                "--tag-version", "0.23.9",
                "--surface", "plugin",
            ],
        ), contextlib.redirect_stdout(stdout):
            with self.assertRaises(SystemExit) as cm:
                runpy.run_path(str(SCRIPT), run_name="__main__")

        self.assertEqual(cm.exception.code, 0)
        result = json.loads(stdout.getvalue())
        self.assertEqual(result["should_tag"], 1)
        self.assertEqual(result["surface"], "plugin")


if __name__ == "__main__":
    unittest.main(verbosity=2)

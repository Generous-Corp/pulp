#!/usr/bin/env python3
"""Tests for editor_host_acceptance.py's verdicts, without a build or a Vellum SDK.

The runner's promise is that an item passes only when every one of its checks
was seen to pass, and that an item it cannot run is SKIP, never PASS. These
feed it synthetic ctest output and assert exactly that.
"""

from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import editor_host_acceptance as acc  # noqa: E402


def full_output() -> str:
    """ctest -V output in which every runnable item's checks pass."""
    lines = []
    for item in acc.ITEMS:
        for check in item.checks:
            if isinstance(check, acc.TestPassed):
                lines.append(f"1/9 Test  #1: {check.name} ..........   Passed    0.10 sec")
            elif isinstance(check, acc.Line):
                lines.append(f"12: [{check.tag}] ok: {check.text}")
            elif isinstance(check, acc.TagClean):
                for i in range(check.minimum):
                    lines.append(f"12: [{check.tag}] ok: filler check {i}")
    return "\n".join(lines)


class Verdicts(unittest.TestCase):
    def evaluate(self, text: str, direction: bool | None = True) -> dict:
        obs = acc.parse_ctest_output(text)
        obs.direction_passed = direction
        return {v.key: v for v in acc.evaluate(acc.ITEMS, obs)}

    def test_every_check_seen_passes_every_runnable_item(self) -> None:
        verdicts = self.evaluate(full_output())
        for key, v in verdicts.items():
            item = next(i for i in acc.ITEMS if i.key == key)
            expected = acc.SKIP if item.skip_reason else acc.PASS
            self.assertEqual(v.status, expected, f"{key}: {v.details}")

    def test_unrunnable_items_are_skip_never_pass(self) -> None:
        verdicts = self.evaluate(full_output())
        skipped = {key for key, v in verdicts.items() if v.status == acc.SKIP}
        self.assertEqual(skipped, {"2e", "3b", "3c", "3g", "6", "7"})
        for key in skipped:
            self.assertTrue(verdicts[key].details[0], f"{key} has no reason")

    def test_a_failed_harness_line_fails_its_item(self) -> None:
        text = full_output() + "\n12: [vellum/clap] FAIL: no gesture is left open after closing"
        verdicts = self.evaluate(text)
        self.assertEqual(verdicts["2b"].status, acc.FAIL)
        self.assertEqual(verdicts["2a"].status, acc.PASS)

    def test_a_missing_line_fails_its_item(self) -> None:
        text = full_output().replace("ok: the drag lands on exactly 0.75", "ok: something else")
        verdicts = self.evaluate(text)
        self.assertEqual(verdicts["3d"].status, acc.FAIL)
        self.assertEqual(verdicts["4"].status, acc.FAIL)

    def test_a_failed_or_absent_test_fails_its_item(self) -> None:
        failed = full_output().replace(
            "pulp-editor-host-slice-symbols ..........   Passed",
            "pulp-editor-host-slice-symbols ..........***Failed")
        self.assertEqual(self.evaluate(failed)["1"].status, acc.FAIL)
        absent = "\n".join(
            line for line in full_output().splitlines()
            if "pulp-editor-host-slice-audio-identical" not in line)
        self.assertEqual(self.evaluate(absent)["2a"].status, acc.FAIL)

    def test_too_few_checks_under_a_tag_fails(self) -> None:
        obs = acc.parse_ctest_output("1: [x/au] ok: one\n1: [x/au] ok: two")
        ok, _ = acc.evaluate_check(acc.TagClean("x/au", 3), obs)
        self.assertFalse(ok)
        ok, _ = acc.evaluate_check(acc.TagClean("x/au", 2), obs)
        self.assertTrue(ok)

    def test_direction_check_must_have_run_and_passed(self) -> None:
        self.assertEqual(self.evaluate(full_output(), direction=False)["5"].status, acc.FAIL)
        self.assertEqual(self.evaluate(full_output(), direction=None)["5"].status, acc.FAIL)

    def test_empty_output_passes_nothing(self) -> None:
        verdicts = self.evaluate("", direction=None)
        self.assertFalse(any(v.status == acc.PASS for v in verdicts.values()))

    def test_summary_never_calls_a_skip_a_pass(self) -> None:
        obs = acc.parse_ctest_output(full_output())
        obs.direction_passed = True
        table = acc.render_table(acc.evaluate(acc.ITEMS, obs))
        self.assertIn("SKIP", table)
        self.assertIn("SKIPPED items are not passes", table)
        self.assertNotIn("acceptance passed", table)


if __name__ == "__main__":
    unittest.main()

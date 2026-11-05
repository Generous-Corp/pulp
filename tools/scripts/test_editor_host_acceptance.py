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


def mutations_confirmed() -> dict:
    return {check.name: "CONFIRMED" for item in acc.ITEMS for check in item.checks
            if isinstance(check, acc.Mutation)}


class Verdicts(unittest.TestCase):
    def evaluate(self, text: str, direction: bool | None = True, mutations: dict | None = None,
                 mutation_skip_reason: str = "") -> dict:
        obs = acc.parse_ctest_output(text)
        obs.direction_passed = direction
        obs.mutations = mutations_confirmed() if mutations is None else mutations
        obs.mutation_skip_reason = mutation_skip_reason
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
        self.assertEqual(skipped, {"6", "7"})
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

    def test_the_mutation_must_be_confirmed(self) -> None:
        for verdict in ("NOT CONFIRMED", "INCONCLUSIVE"):
            verdicts = self.evaluate(full_output(), mutations={"rmlui-gesture": verdict})
            self.assertEqual(verdicts["3g"].status, acc.FAIL, verdict)
        self.assertEqual(self.evaluate(full_output(), mutations={})["3g"].status, acc.FAIL)
        self.assertEqual(self.evaluate(full_output())["3g"].status, acc.PASS)

    def test_a_mutation_that_did_not_run_is_skip_with_its_reason(self) -> None:
        verdicts = self.evaluate(full_output(), mutations={}, mutation_skip_reason="not run: x")
        self.assertEqual(verdicts["3g"].status, acc.SKIP)
        self.assertEqual(verdicts["3g"].details, ["not run: x"])
        self.assertEqual(verdicts["3d"].status, acc.PASS)

    def test_confirm_failure_output_is_read_for_its_verdict(self) -> None:
        self.assertEqual(acc.parse_confirm_failure(
            "confirm-failure: breaking\nconfirm-failure: CONFIRMED — the test passes"), "CONFIRMED")
        self.assertEqual(acc.parse_confirm_failure(
            "confirm-failure: NOT CONFIRMED — the test still passes"), "NOT CONFIRMED")
        self.assertEqual(acc.parse_confirm_failure("no verdict at all"), "INCONCLUSIVE")

    def test_the_text_field_and_empty_document_items_need_their_lines(self) -> None:
        for key, line in (("2e", "Space is consumed when a text field has focus"),
                          ("3b", "an empty document's frame does not equal the golden image")):
            text = full_output().replace(f"ok: {line}", "ok: something else")
            self.assertEqual(self.evaluate(text)[key].status, acc.FAIL, key)

    def test_3c_runs_when_the_sdk_carries_the_vellum_renderer(self) -> None:
        verdicts = self.evaluate(full_output())
        self.assertEqual(verdicts["3c"].status, acc.PASS)
        text = full_output().replace("ok: an empty document falls outside that tolerance",
                                     "FAIL: an empty document falls outside that tolerance")
        self.assertEqual(self.evaluate(text)["3c"].status, acc.FAIL)

    def test_3c_is_skip_with_its_reason_when_the_sdk_lacks_the_vellum_renderer(self) -> None:
        without = "\n".join(
            line for line in full_output().splitlines()
            if "pulp-editor-host-slice-rmlui-renderer" not in line and "[rmlui-renderer]" not in line)
        marker = "2/9 Test  #2: pulp-editor-host-slice-rmlui-renderer-absent ....   Passed    0.01 sec"
        verdict = self.evaluate(without + "\n" + marker)["3c"]
        self.assertEqual(verdict.status, acc.SKIP)
        self.assertEqual(verdict.details, ["Vellum SDK built without VELLUM_RMLUI_VELLUM_RENDERER"])
        # Neither the renderer test nor the marker: the item did not run, a FAIL.
        self.assertEqual(self.evaluate(without)["3c"].status, acc.FAIL)

    def test_empty_output_passes_nothing(self) -> None:
        verdicts = self.evaluate("", direction=None)
        self.assertFalse(any(v.status == acc.PASS for v in verdicts.values()))

    def test_summary_never_calls_a_skip_a_pass(self) -> None:
        obs = acc.parse_ctest_output(full_output())
        obs.direction_passed = True
        obs.mutations = mutations_confirmed()
        table = acc.render_table(acc.evaluate(acc.ITEMS, obs))
        self.assertIn("SKIP", table)
        self.assertIn("SKIPPED items are not passes", table)
        self.assertNotIn("acceptance passed", table)


if __name__ == "__main__":
    unittest.main()

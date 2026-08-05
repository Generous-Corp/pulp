#!/usr/bin/env python3
"""Unit tests for check_agent_panel_invariants.py's failure classification.

The negative agent-panel fixture (polystrike) passes by being REFUSED, so the
only thing standing between "the clipping gate still fires" and "nobody
noticed it stopped" is which of three verdicts this function returns. It is
tested in all three directions, because a classifier only ever watched
returning one of them is not known to be able to return the others.

The regression that motivates it: the browser-unavailable heuristic ran FIRST
and matched on the very output that proves the gate worked, so the fixture
skipped every time and ctest reported the skip inside "100% tests passed".
The `real_refusal_output` fixture below is a verbatim capture of that output.
"""

from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path

THIS_DIR = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location(
    "check_agent_panel_invariants", THIS_DIR / "check_agent_panel_invariants.py")
_mod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_mod)
classify_failure = _mod.classify_failure

# Verbatim from the importer refusing test/fixtures/agent-panels/polystrike.
# Kept literal rather than paraphrased: the bug was a substring match, and a
# tidied-up copy would not contain the substrings that caused it.
REAL_REFUSAL = (
    "Detected runnable HTML; using the browser-solved import lane.\n"
    "[browser-capture] selected system browser Google Chrome/151.0.7922.75 at "
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome\n"
    "Error: browser HTML capture failed [capture-control-clipped]: browser "
    "capture failed: [browser-capture] browser=Chrome/151.0.7922.75 "
    "capture-control-clipped: 4 bound control(s) are clipped out of view: "
    "param_2 cut by 109px inside .pulp-panel.\n"
    "  the native design path is NOT covered by this run\n"
)

# A browser that genuinely is not there. No named reason anywhere in it.
NO_BROWSER = (
    "Detected runnable HTML; using the browser-solved import lane.\n"
    "Error: browser capture failed: no usable browser was found\n"
    "  the native design path is NOT covered by this run\n"
)

# An unrelated crash. The gate must NOT accept this as its refusal.
UNRELATED = (
    "Detected runnable HTML; using the browser-solved import lane.\n"
    "Error: capture-source-unresolved: styles.css returned 404\n"
    "  the native design path is NOT covered by this run\n"
)


class ClassifyFailure(unittest.TestCase):
    def test_the_named_refusal_wins_over_the_browser_heuristic(self):
        # The regression. Both heuristic substrings ("browser", "not") are
        # present in this output -- "browser" from the capture banners and
        # "not" from the trailing "NOT covered" note -- so before the fix this
        # returned browser-unavailable and the fixture skipped forever.
        self.assertIn("browser", REAL_REFUSAL.lower())
        self.assertIn("not", REAL_REFUSAL.lower())
        self.assertEqual(
            classify_failure(REAL_REFUSAL, "capture-control-clipped"),
            "rejected-as-intended")

    def test_a_genuinely_absent_browser_still_skips(self):
        # The heuristic must keep working -- reordering must not cost the skip
        # it was added for. No named reason is present, so it falls through.
        self.assertEqual(
            classify_failure(NO_BROWSER, "capture-control-clipped"),
            "browser-unavailable")

    def test_an_unrelated_failure_is_not_the_expected_refusal(self):
        # The whole point of demanding the reason BY NAME: any import error
        # would otherwise satisfy "it failed", and a gate that stopped firing
        # would keep passing on the back of an unrelated crash. This output
        # also trips the browser heuristic, so it is reported as a skip rather
        # than as the refusal -- which is a visible non-pass either way, and
        # never a false PASS.
        self.assertNotEqual(
            classify_failure(UNRELATED, "capture-control-clipped"),
            "rejected-as-intended")

    def test_a_positive_fixture_has_no_expected_refusal(self):
        # expect_reject empty: a positive panel's failure is just a failure.
        self.assertEqual(classify_failure(UNRELATED, ""), "browser-unavailable")
        self.assertEqual(classify_failure("Error: boom\n", ""), "failed")


if __name__ == "__main__":
    unittest.main()

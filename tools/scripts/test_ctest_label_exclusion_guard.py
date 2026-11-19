#!/usr/bin/env python3
"""Tests for ctest_label_exclusion_guard.py."""

from __future__ import annotations

import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location(
    "ctest_label_exclusion_guard", HERE / "ctest_label_exclusion_guard.py")
guard = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(guard)

EXCLUDED = ["validation", "slow", "performance", "bench", "quality-lab"]


def blind(text: str, excluded: list[str] | None = None) -> list[str]:
    """Names of the suites `text` leaves unreachable by both gated lanes."""
    with tempfile.NamedTemporaryFile("w", suffix=".cmake", delete=False) as fh:
        fh.write(text)
        path = Path(fh.name)
    try:
        regs = guard.scan_file(path)
        return [t for t, _r, _h in
                guard.find_blind_targets(regs, excluded or EXCLUDED)]
    finally:
        path.unlink()


def regs(text: str):
    with tempfile.NamedTemporaryFile("w", suffix=".cmake", delete=False) as fh:
        fh.write(text)
        path = Path(fh.name)
    try:
        return guard.scan_file(path)
    finally:
        path.unlink()


class DetectionTest(unittest.TestCase):
    """The condition the guard exists for: every registration excluded."""

    def test_only_excluded_label_is_flagged(self):
        self.assertEqual(blind(
            'pulp_add_test_suite(pulp-test-x LIBRARIES pulp::view LABELS slow)\n'),
            ["pulp-test-x"])

    def test_each_excluded_label_is_detected(self):
        for label in EXCLUDED:
            with self.subTest(label=label):
                self.assertEqual(
                    blind(f'pulp_add_test_suite(pulp-test-x LABELS {label})\n'),
                    ["pulp-test-x"])

    def test_excluded_label_inside_a_list_is_detected(self):
        """A suite is blind on ONE excluded member of a multi-label list."""
        self.assertEqual(blind(
            'pulp_add_test_suite(pulp-test-x LABELS "audio;hardware;validation")\n'),
            ["pulp-test-x"])

    def test_labels_under_properties_is_detected(self):
        """`PROPERTIES ... LABELS slow` excludes exactly as a top-level LABELS
        does, and is the more common shape in the manifests."""
        self.assertEqual(blind(
            'catch_discover_tests(pulp-test-x PROPERTIES '
            'RESOURCE_LOCK pulp_gpu LABELS slow)\n'),
            ["pulp-test-x"])

    def test_multiline_registration_is_detected(self):
        self.assertEqual(blind(
            "pulp_add_test_suite(pulp-test-x\n"
            "    SOURCES a.cpp b.cpp\n"
            "    LIBRARIES pulp::view\n"
            "    LABELS slow\n"
            "    TIMEOUT 900)\n"),
            ["pulp-test-x"])

    def test_two_excluded_registrations_are_still_blind(self):
        """Splitting into two labelled halves reaches no enforced lane either."""
        self.assertEqual(blind(
            'pulp_add_test_suite(pulp-test-x TEST_SPEC "[slow]" LABELS slow)\n'
            'catch_discover_tests(pulp-test-x TEST_SPEC "[bench]" '
            'TEST_PREFIX "b::" LABELS bench)\n'),
            ["pulp-test-x"])


class CompliantShapesTest(unittest.TestCase):
    """Shapes that do reach the required gate and the coverage run."""

    def test_test_spec_split_passes(self):
        """The repo's own idiom: one unlabelled fast registration plus one
        labelled slow registration behind a ctest prefix."""
        self.assertEqual(blind(
            'pulp_add_test_suite(pulp-test-x\n'
            '    LIBRARIES pulp::signal\n'
            '    TEST_SPEC "~[slow]"\n'
            '    TIMEOUT 900)\n'
            'catch_discover_tests(pulp-test-x\n'
            '    TEST_SPEC "[slow]"\n'
            '    TEST_PREFIX "slow::"\n'
            '    LABELS slow)\n'),
            [])

    def test_no_labels_at_all_passes(self):
        self.assertEqual(
            blind('pulp_add_test_suite(pulp-test-x LIBRARIES pulp::view)\n'), [])

    def test_non_excluded_label_passes(self):
        """`lifecycle` is not on the policy list, so the suite still runs."""
        self.assertEqual(blind(
            'catch_discover_tests(pulp-test-x PROPERTIES LABELS lifecycle)\n'), [])

    def test_split_across_two_manifests_passes(self):
        """Grouping is by target, so the unlabelled sibling rescues the suite
        even when the two registrations live in different files."""
        a = regs('catch_discover_tests(pulp-test-x PROPERTIES LABELS slow)\n')
        b = regs('pulp_add_test_suite(pulp-test-x LIBRARIES pulp::view)\n')
        self.assertEqual(guard.find_blind_targets(a + b, EXCLUDED), [])

    def test_unrelated_command_is_not_a_registration(self):
        """Only the two Catch2 suite-registration commands are scanned; a
        standalone add_test labelled elsewhere has no TEST_SPEC to split."""
        self.assertEqual(blind(
            'add_test(NAME thing COMMAND foo)\n'
            'set_tests_properties(thing PROPERTIES LABELS slow)\n'), [])

    def test_commented_out_registration_is_ignored(self):
        self.assertEqual(blind(
            '# pulp_add_test_suite(pulp-test-x LABELS slow)\n'), [])

    def test_trailing_comment_does_not_hide_the_label(self):
        self.assertEqual(blind(
            'pulp_add_test_suite(pulp-test-x  # a suite\n'
            '    LABELS slow)\n'),
            ["pulp-test-x"])


class ConservativeTest(unittest.TestCase):
    """The guard must never manufacture a failure out of something it cannot
    read — a false FAIL here reddens the required gate for nobody's benefit."""

    def test_unresolvable_label_variable_is_not_flagged(self):
        self.assertEqual(
            blind('pulp_add_test_suite(pulp-test-x LABELS ${MY_LABELS})\n'), [])

    def test_variable_target_name_is_skipped(self):
        self.assertEqual(
            blind('pulp_add_test_suite(${NAME} LABELS slow)\n'), [])

    def test_unbalanced_parentheses_are_skipped(self):
        self.assertEqual(blind('pulp_add_test_suite(pulp-test-x LABELS slow\n'), [])

    def test_a_keyword_terminates_the_label_list(self):
        """`LABELS slow TIMEOUT 900` must not read TIMEOUT/900 as labels."""
        r = regs('pulp_add_test_suite(pulp-test-x LABELS slow TIMEOUT 900)\n')
        self.assertEqual(r[0].labels, ["slow"])


class PolicySourceTest(unittest.TestCase):
    """The excluded-label list is read from the coverage policy, never copied."""

    def test_reads_the_live_policy_file(self):
        labels = guard.read_excluded_labels(guard.REPO_ROOT / guard.POLICY_FILE)
        self.assertEqual(sorted(labels), sorted(EXCLUDED))

    def test_tracks_a_changed_policy(self):
        """Add a label to the policy and the guard must start flagging it —
        this is the whole reason the list is not hardcoded."""
        with tempfile.NamedTemporaryFile("w", suffix=".sh", delete=False) as fh:
            fh.write(': "${PULP_COVERAGE_CTEST_LABEL_EXCLUDE:=slow|flaky}"\n')
            path = Path(fh.name)
        try:
            labels = guard.read_excluded_labels(path)
            self.assertEqual(labels, ["slow", "flaky"])
            self.assertEqual(
                blind('pulp_add_test_suite(pulp-test-x LABELS flaky)\n', labels),
                ["pulp-test-x"])
        finally:
            path.unlink()

    def test_missing_policy_default_is_an_error(self):
        """A policy file the guard cannot parse must fail loudly rather than
        fall back to a hardcoded list that can drift from the lanes."""
        with tempfile.NamedTemporaryFile("w", suffix=".sh", delete=False) as fh:
            fh.write("# nothing here\n")
            path = Path(fh.name)
        try:
            with self.assertRaises(ValueError):
                guard.read_excluded_labels(path)
        finally:
            path.unlink()


class AllowlistTest(unittest.TestCase):
    """The frozen backlog is a ledger, not a mute switch."""

    def _write(self, payload: dict) -> Path:
        fh = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False)
        json.dump(payload, fh)
        fh.close()
        return Path(fh.name)

    def test_entry_without_a_reason_is_rejected(self):
        path = self._write({"allow": [{"target": "pulp-test-x", "reason": "  "}]})
        try:
            with self.assertRaises(ValueError):
                guard.read_allowlist(path)
        finally:
            path.unlink()

    def test_entry_with_a_reason_loads(self):
        path = self._write(
            {"allow": [{"target": "pulp-test-x", "reason": "because"}]})
        try:
            self.assertEqual(guard.read_allowlist(path),
                             {"pulp-test-x": "because"})
        finally:
            path.unlink()

    def test_every_shipped_entry_states_a_reason(self):
        allowed = guard.read_allowlist(guard.REPO_ROOT / guard.ALLOWLIST_FILE)
        self.assertTrue(allowed)
        for target, reason in allowed.items():
            self.assertGreater(len(reason), 20, target)


class TreeTest(unittest.TestCase):
    """The guard runs as a ctest against the real tree, so the tree must pass —
    and the ledger must still describe it."""

    def test_tree_passes_with_the_shipped_allowlist(self):
        self.assertEqual(guard.main([]), 0)

    def test_tree_without_the_allowlist_reports_the_frozen_backlog(self):
        """Control: the allowlist is load-bearing. Drop it and the same scan
        must fail, or the shipped OK proves nothing about the guard working."""
        self.assertEqual(guard.main(["--no-allowlist"]), 1)

    def test_allowlist_matches_the_tree_exactly(self):
        """No stale entry (a suite that has since been split) and no missing
        one — the ledger is checked against the live scan, not trusted."""
        regs_all = []
        for p in guard.iter_default_targets():
            regs_all.extend(guard.scan_file(p))
        excluded = guard.read_excluded_labels(guard.REPO_ROOT / guard.POLICY_FILE)
        found = {t for t, _r, _h in guard.find_blind_targets(regs_all, excluded)}
        allowed = set(guard.read_allowlist(guard.REPO_ROOT / guard.ALLOWLIST_FILE))
        self.assertEqual(found, allowed)

    def test_the_motivating_suite_is_scanned(self):
        """Control that the default scan reaches real manifests at all: a
        zero-finding run over an empty file set would pass identically."""
        names = {p.name for p in guard.iter_default_targets()}
        self.assertIn("view_widget_bridge_tests.cmake", names)
        self.assertGreater(len(names), 50)


def long_timeouts(text: str) -> list[str]:
    """Names of registrations in `text` that raise TIMEOUT without an excluded label."""
    with tempfile.NamedTemporaryFile("w", suffix=".cmake", delete=False) as fh:
        fh.write(text)
        path = Path(fh.name)
    try:
        regs = guard.scan_timeouts(path)
        return [r.name for r in guard.find_unlabelled_long_timeouts(regs, EXCLUDED)]
    finally:
        path.unlink()


class LongTimeoutTest(unittest.TestCase):
    """A raised TIMEOUT declares a long runtime; it must stay off the required gate."""

    def test_planted_literal_timeout_without_label_is_flagged(self):
        self.assertEqual(long_timeouts(
            "add_test(NAME slow-thing COMMAND foo)\n"
            "set_tests_properties(slow-thing PROPERTIES TIMEOUT 300)\n"),
            ["slow-thing"])

    def test_set_tests_properties_flags_every_listed_name(self):
        self.assertEqual(long_timeouts(
            "set_tests_properties(old-one new-one PROPERTIES TIMEOUT 300)\n"),
            ["old-one", "new-one"])

    def test_suite_timeout_without_label_is_flagged(self):
        self.assertEqual(long_timeouts(
            "pulp_add_test_suite(pulp-test-fft SOURCES a.cpp TIMEOUT 600)\n"),
            ["pulp-test-fft"])

    def test_catch_discover_properties_timeout_is_flagged(self):
        self.assertEqual(long_timeouts(
            "catch_discover_tests(pulp-test-x PROPERTIES TIMEOUT 900)\n"),
            ["pulp-test-x"])

    def test_scaled_variable_timeout_is_resolved_and_flagged(self):
        self.assertEqual(long_timeouts(
            "pulp_scaled_test_timeout(_t 900)\n"
            'catch_discover_tests(pulp-test-x PROPERTIES TIMEOUT "${_t}")\n'),
            ["pulp-test-x"])

    def test_slow_label_passes(self):
        self.assertEqual(long_timeouts(
            "pulp_add_test_suite(pulp-test-x TIMEOUT 600 LABELS slow)\n"), [])

    def test_labels_inside_properties_pass(self):
        self.assertEqual(long_timeouts(
            "set_tests_properties(t PROPERTIES TIMEOUT 300 LABELS \"a;slow\")\n"), [])

    def test_timeout_at_or_below_default_passes(self):
        self.assertEqual(long_timeouts(
            "set_tests_properties(t PROPERTIES TIMEOUT 120)\n"
            "set_tests_properties(u PROPERTIES TIMEOUT 30)\n"), [])

    def test_no_timeout_is_a_clean_control(self):
        self.assertEqual(long_timeouts(
            "pulp_add_test_suite(pulp-test-x SOURCES a.cpp)\n"
            "set_tests_properties(t PROPERTIES LABELS cmake)\n"), [])

    def test_unresolvable_timeout_never_manufactures_a_failure(self):
        self.assertEqual(long_timeouts(
            "set_tests_properties(t PROPERTIES TIMEOUT ${unknown_var})\n"), [])

    def test_unrelated_label_does_not_rescue(self):
        self.assertEqual(long_timeouts(
            "set_tests_properties(t PROPERTIES TIMEOUT 300 LABELS cmake)\n"),
            ["t"])

    def test_comment_is_ignored(self):
        self.assertEqual(long_timeouts(
            "# set_tests_properties(t PROPERTIES TIMEOUT 300)\n"), [])


class TimeoutAllowlistTest(unittest.TestCase):
    def _run(self, cmake: str, allow: dict | None):
        with tempfile.TemporaryDirectory() as d:
            m = Path(d) / "x.cmake"
            m.write_text(cmake, encoding="utf-8")
            a = Path(d) / "allow.json"
            a.write_text(json.dumps(allow if allow is not None else {"allow": []}),
                         encoding="utf-8")
            return guard.main([str(m), "--allowlist", str(a)])

    SLOW = ("pulp_add_test_suite(pulp-test-base SOURCES a.cpp)\n"
            "set_tests_properties(ok PROPERTIES TIMEOUT 600 LABELS slow)\n"
            "set_tests_properties(t PROPERTIES TIMEOUT 300)\n")

    def test_planted_violation_fails(self):
        self.assertEqual(self._run(self.SLOW, None), 1)

    def test_allowlisted_violation_passes(self):
        self.assertEqual(self._run(self.SLOW, {"allow": [], "timeout_allow": [
            {"target": "t", "reason": "pre-existing, long by design"}]}), 0)

    def test_entry_without_reason_is_rejected(self):
        self.assertEqual(self._run(self.SLOW, {"allow": [], "timeout_allow": [
            {"target": "t", "reason": ""}]}), 1)

    def test_stale_entry_fails(self):
        clean = ("pulp_add_test_suite(pulp-test-base SOURCES a.cpp)\n"
                 "set_tests_properties(ok PROPERTIES TIMEOUT 600 LABELS slow)\n")
        self.assertEqual(self._run(clean, {"allow": [], "timeout_allow": [
            {"target": "ok", "reason": "pre-existing, no longer needed"}]}), 1)

    def test_shipped_ledger_matches_the_tree(self):
        regs = []
        for p in guard.iter_default_targets():
            regs.extend(guard.scan_timeouts(p))
        self.assertGreater(len(regs), 100)
        excluded = guard.read_excluded_labels(guard.REPO_ROOT / guard.POLICY_FILE)
        found = {r.name for r in guard.find_unlabelled_long_timeouts(regs, excluded)}
        allowed = set(guard.read_timeout_allowlist(guard.REPO_ROOT / guard.ALLOWLIST_FILE))
        self.assertEqual(found, allowed)


LAST_TEST_LOG = """Start testing: Nov 19 00:00 UTC
----------------------------------------------------------
1/2 Testing: fast-one
1/2 Test: fast-one
Command: "/x/fast"
Test Passed.
"fast-one" end time: Nov 19 00:00 UTC
"fast-one" time elapsed: 00:00:01
----------------------------------------------------------
2/2 Testing: fft-big
2/2 Test: fft-big
Test Passed.
"fft-big" end time: Nov 19 00:02 UTC
"fft-big" time elapsed: 00:02:05
----------------------------------------------------------
"""


class RuntimeCheckTest(unittest.TestCase):
    def test_last_test_log_is_parsed(self):
        self.assertEqual(guard.parse_last_test_log(LAST_TEST_LOG),
                         {"fast-one": 1.0, "fft-big": 125.0})

    def test_junit_is_parsed(self):
        xml = ('<testsuite><testcase name="a" time="2.5"/>'
               '<testcase name="b" time="40"/></testsuite>')
        self.assertEqual(guard.parse_junit_times(xml), {"a": 2.5, "b": 40.0})

    def test_changed_registration_over_threshold_without_label_is_flagged(self):
        new = "add_test(NAME fft-big COMMAND x)\n"
        out = guard.runtime_violations(
            changed_names=["fft-big"], times={"fft-big": 125.0},
            labels={"fft-big": []}, excluded=EXCLUDED, limit=30.0)
        self.assertEqual([n for n, _t in out], ["fft-big"])
        self.assertTrue(new)

    def test_labelled_slow_test_passes(self):
        self.assertEqual(guard.runtime_violations(
            ["fft-big"], {"fft-big": 125.0}, {"fft-big": ["slow"]},
            EXCLUDED, 30.0), [])

    def test_fast_test_passes(self):
        self.assertEqual(guard.runtime_violations(
            ["fast-one"], {"fast-one": 1.0}, {}, EXCLUDED, 30.0), [])

    def test_unchanged_slow_test_is_not_this_diffs_problem(self):
        self.assertEqual(guard.runtime_violations(
            [], {"fft-big": 125.0}, {}, EXCLUDED, 30.0), [])

    def test_changed_registrations_are_the_set_difference(self):
        old = "add_test(NAME a COMMAND x)\nadd_test(NAME b COMMAND y)\n"
        new = ("add_test(NAME a COMMAND x)\nadd_test(NAME b COMMAND z)\n"
               "add_test(NAME c COMMAND w)\n")
        got = guard.changed_runtime_names(old, new)
        self.assertEqual(sorted(got), ["b", "c"])

    def test_no_change_is_a_clean_control(self):
        t = "add_test(NAME a COMMAND x)\n"
        self.assertEqual(guard.changed_runtime_names(t, t), [])

    def test_suite_target_maps_to_its_ctest_names(self):
        inventory = [{"name": "case one", "exe": "pulp-test-fft", "labels": []},
                     {"name": "other", "exe": "pulp-test-y", "labels": []}]
        names, labels = guard.names_for_targets(["pulp-test-fft"], inventory)
        self.assertEqual(names, ["case one"])
        self.assertEqual(labels, {"case one": []})

    def test_missing_timing_data_is_a_loud_skip_not_a_pass(self):
        import contextlib
        import io
        buf = io.StringIO()
        with tempfile.TemporaryDirectory() as d, contextlib.redirect_stderr(buf):
            rc = guard.main(["--runtime-base", "origin/main", "--build-dir", d])
        self.assertEqual(rc, 0)
        self.assertIn("SKIPPED", buf.getvalue())
        self.assertIn("NOT CHECKED", buf.getvalue())


class ArgumentHandlingTest(unittest.TestCase):
    """A guard that shrugs at its own command line reports a green run over
    nothing. Each case below returned 0 before argparse landed."""

    def test_an_unknown_flag_is_rejected(self):
        """A typo'd flag (--no-allowlists) used to be ignored, so the run
        silently kept the ledger and still printed OK."""
        with self.assertRaises(SystemExit) as caught:
            guard.main(["--no-allowlists"])
        self.assertEqual(caught.exception.code, 2)

    def test_a_named_target_that_does_not_exist_fails(self):
        """Scanning a path that is not there proves nothing, so it must not
        report a pass. The real manifest is named alongside it deliberately:
        the scan then parses plenty, so only the missing-path check can be
        what fails, and breaking that check makes this test fail."""
        real = guard.REPO_ROOT / "test/cmake/character_delay_tests.cmake"
        self.assertEqual(
            guard.main([str(real), "/nope/definitely-missing.cmake"]), 1)

    def test_a_scan_that_parses_nothing_fails(self):
        """Instrument failure wearing the costume of a pass: a manifest with
        no registrations means the scan read nothing at all."""
        with tempfile.NamedTemporaryFile("w", suffix=".cmake", delete=False) as fh:
            fh.write("# no registrations here\n")
            path = Path(fh.name)
        try:
            self.assertEqual(guard.main([str(path)]), 1)
        finally:
            path.unlink()

    def test_a_real_manifest_parses_non_zero(self):
        """The control the case above needs: the same one-file invocation over
        a manifest that DOES register suites must pass, or the failure above
        would be indistinguishable from the guard rejecting every file."""
        real = guard.REPO_ROOT / "test/cmake/character_delay_tests.cmake"
        self.assertTrue(real.is_file(), real)
        self.assertGreater(len(guard.scan_file(real)), 0)
        self.assertEqual(guard.main([str(real)]), 0)

    def test_list_prints_the_scanned_manifests(self):
        """--list used to be swallowed as an unknown flag and run a full scan
        instead, printing an OK summary that looked like the listing worked."""
        import contextlib
        import io
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            self.assertEqual(guard.main(["--list"]), 0)
        lines = [ln for ln in buf.getvalue().splitlines() if ln.strip()]
        self.assertEqual(len(lines), len(list(guard.iter_default_targets())))
        self.assertTrue(all(ln.endswith(".cmake") for ln in lines), lines[:3])


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""CTest observations preserve non-runs without becoming test verdicts."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

import ctest_nonruns as observer

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/scripts/ctest_nonruns.py"


class ObservationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.report = self.root / "ctest.junit.xml"

    def observe(self, xml):
        self.report.write_text(xml, encoding="utf-8")
        return observer.observe(self.report)

    def test_pass_and_failure_are_both_runs(self):
        report = self.observe('<testsuite tests="2"><testcase name="ok" status="run"/><testcase name="bad" status="fail"><failure/></testcase></testsuite>')
        self.assertEqual(report["observation"], "observed")
        self.assertEqual(report["counts"]["fail"], 1)
        self.assertEqual(report["nonruns"], [])

    def test_skip_and_disabled_keep_reason_labels_and_output(self):
        report = self.observe('<testsuite tests="2"><testcase name="gpu" status="notrun"><skipped message="no GPU"/><properties><property name="cmake_labels" value="gpu;slow"/></properties><system-out>first\nlast</system-out></testcase><testcase name="off" status="disabled"/></testsuite>')
        self.assertEqual(report["observation"], "observed")
        self.assertEqual(report["nonruns"][0]["reason"], "no GPU")
        self.assertEqual(report["nonruns"][0]["labels"], "gpu;slow")
        self.assertEqual(report["nonruns"][0]["output"], "last")
        self.assertEqual(report["counts"]["disabled"], 1)

    def test_catch2_skip_reason_is_recovered_not_the_shared_mechanism(self):
        # Every Catch2 skip carries the same <skipped message>, so two cases that
        # did not run for entirely different reasons render identically unless the
        # author's message is recovered from the captured output.
        def row(name, where, message):
            return (
                f'<testcase name="{name}" status="notrun">'
                '<skipped message="SKIP_RETURN_CODE=4"/>'
                f'<system-out>{where}: SKIPPED:\nexplicitly with message:\n  {message}\n'
                '\nassertions: - none -</system-out></testcase>'
            )
        report = self.observe(
            '<testsuite tests="2">'
            + row("bidi", "test/test_text.cpp:140", "SheenBidi not linked")
            + row("cli", "test/test_cli.cpp:24", "pulp not built")
            + "</testsuite>"
        )
        self.assertEqual(report["observation"], "observed")
        reasons = [r["reason"] for r in report["nonruns"]]
        self.assertEqual(reasons, ["SheenBidi not linked", "pulp not built"])
        self.assertEqual(len(set(reasons)), 2)
        self.assertNotIn("SKIP_RETURN_CODE=4", reasons)
        # The fourth column becomes the source location rather than the trailing
        # `assertions: - none -` every skipped case ends with.
        self.assertEqual(
            [r["output"] for r in report["nonruns"]],
            ["test/test_text.cpp:140", "test/test_cli.cpp:24"],
        )

    def test_non_catch2_skip_falls_back_to_the_mechanism(self):
        # A script exiting SKIP_RETURN_CODE prints no Catch2 block; the row must
        # still say something rather than going blank.
        report = self.observe(
            '<testsuite tests="1"><testcase name="script" status="notrun">'
            '<skipped message="SKIP_RETURN_CODE=4"/>'
            "<system-out>no adapter</system-out></testcase></testsuite>"
        )
        self.assertEqual(report["nonruns"][0]["reason"], "SKIP_RETURN_CODE=4")
        self.assertEqual(report["nonruns"][0]["output"], "no adapter")

    def test_zero_tests_never_claims_every_test_ran(self):
        report = self.observe('<testsuite tests="0"/>')
        self.assertEqual(report["observation"], "incomplete")
        self.assertNotIn("Every", observer.markdown(report))

    def test_missing_file_is_observation_gap(self):
        report = observer.observe(self.report)
        self.assertEqual(report["observation"], "unavailable")
        self.assertIsNone(report["counts"])

    def test_unknown_xml_encoding_is_structured_unavailable_evidence(self):
        self.report.write_bytes(b'<?xml version="1.0" encoding="not-a-codec"?><testsuite tests="0"/>')
        for flags in (["--json"], []):
            with self.subTest(flags=flags):
                result = subprocess.run([sys.executable, str(SCRIPT), str(self.report), *flags], capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 2)
                self.assertNotIn("Traceback", result.stderr)
                if flags:
                    self.assertEqual(json.loads(result.stdout)["observation"], "unavailable")
                else:
                    self.assertIn("Observation:", result.stdout)

    def test_malformed_and_wrong_dialects_are_not_clean(self):
        for xml in ('', '<testsuite>', '<testsuites/>', '<testsuite tests="1"><testsuite><testcase/></testsuite></testsuite>', '<testsuite tests="wat"/>'):
            with self.subTest(xml=xml):
                self.assertEqual(self.observe(xml)["observation"], "unavailable")

    def test_unknown_missing_and_contradictory_outcomes_remain_visible(self):
        for attrs, child in (('name="a" status="future"', ''), ('status="run"', ''), ('name="a" status="run"', '<skipped/>'), ('name="a" status="disabled"', '<failure/>')):
            with self.subTest(attrs=attrs, child=child):
                report = self.observe(f'<testsuite tests="1"><testcase {attrs}>{child}</testcase></testsuite>')
                self.assertEqual(report["observation"], "incomplete")
                self.assertNotIn("Every", observer.markdown(report))

    def test_count_contradiction_is_not_clean(self):
        self.assertEqual(self.observe('<testsuite tests="5"><testcase name="a" status="run"/></testsuite>')["observation"], "incomplete")

    def test_summary_failure_cannot_hide_behind_run_status(self):
        report = self.observe('<testsuite tests="1" failures="1"><testcase name="a" status="run"/></testsuite>')
        self.assertEqual(report["observation"], "incomplete")
        self.assertNotIn("Every", observer.markdown(report))

    def test_workflow_uses_only_the_shared_observer_without_changing_gate(self):
        text = (ROOT / ".github/workflows/build.yml").read_text()
        step = text.split("- name: Observe ctest non-runs (non-Windows)", 1)[1].split("- name:", 1)[0]
        self.assertIn("continue-on-error: true", step)
        self.assertIn("always() && runner.os != 'Windows'", step)
        self.assertIn('python3 tools/scripts/ctest_nonruns.py "$junit"', step)
        self.assertIn('--json-output "$PULP_BUILD_DIR/ctest.nonruns.json"', step)
        self.assertIn('>> "$GITHUB_STEP_SUMMARY"', step)
        self.assertNotIn("ET.parse", step)
        self.assertNotIn("pip install", step)
        upload = text.split("- name: Upload ctest logs and JUnit report", 1)[1].split("- name:", 1)[0]
        self.assertIn("ctest.nonruns.json", upload)

    def test_duplicate_names_are_not_collapsed(self):
        report = self.observe('<testsuite tests="2">' + '<testcase name="a" status="notrun"/>' * 2 + '</testsuite>')
        self.assertEqual([r["index"] for r in report["nonruns"]], [1, 2])

    def test_baseline_comparison_is_duplicate_safe_and_bounded(self):
        baseline = self.root / "baseline.xml"
        baseline.write_text('<testsuite tests="7"><testcase name="same" status="run"/><testcase name="same" status="notrun"/><testcase name="ambiguous" status="run"/><testcase name="ambiguous" status="notrun"/><testcase name="new-skip" status="run"/><testcase name="recovered" status="notrun"/><testcase name="failure" status="fail"><failure/></testcase></testsuite>', encoding="utf-8")
        self.report.write_text('<testsuite tests="7"><testcase name="same" status="notrun"/><testcase name="same" status="run"/><testcase name="ambiguous" status="notrun"/><testcase name="ambiguous" status="notrun"/><testcase name="new-skip" status="notrun"/><testcase name="recovered" status="run"/><testcase name="failure" status="run"/></testsuite>', encoding="utf-8")
        report, cases = observer.observe_with_cases(self.report)
        observer.compare(report, cases, baseline)
        comparison = report["comparison"]
        self.assertEqual(comparison["status"], "observed")
        self.assertEqual(comparison["transition_counts"], {"newly_failed": 0, "new_nonrun": 1, "changed": 0, "failure_cleared": 1, "recovered": 1, "ambiguous_duplicate_groups": 1})
        self.assertFalse(any(row["name"] == "same" for row in comparison["transitions"]))
        self.assertEqual(comparison["ambiguous_duplicate_groups"][0]["name"], "ambiguous")
        self.assertIn("Changes from baseline", observer.markdown(report))
        self.assertIn("ambiguous duplicate groups: 1", observer.markdown(report))

    def test_invalid_baseline_makes_comparison_incomplete(self):
        baseline = self.root / "baseline.xml"
        baseline.write_text('<testsuites/>', encoding="utf-8")
        self.report.write_text('<testsuite tests="1"><testcase name="ok" status="run"/></testsuite>', encoding="utf-8")
        result = subprocess.run([sys.executable, str(SCRIPT), str(self.report), "--baseline", str(baseline), "--json"], capture_output=True, text=True, timeout=10)
        report = json.loads(result.stdout)
        self.assertEqual(result.returncode, 2)
        self.assertEqual(report["comparison"]["status"], "unavailable")
        self.assertEqual(report["observation"], "incomplete")

    def test_invalid_baseline_preserves_omitted_issue_count(self):
        baseline = self.root / "baseline.xml"
        count = observer.MAX_ROWS + 5
        baseline.write_text(f'<testsuite tests="{count}">' + ''.join(f'<testcase name="bad-{i}" status="future"/>' for i in range(count)) + '</testsuite>', encoding="utf-8")
        self.report.write_text('<testsuite tests="1"><testcase name="ok" status="run"/></testsuite>', encoding="utf-8")
        report, cases = observer.observe_with_cases(self.report)
        observer.compare(report, cases, baseline)
        self.assertEqual(len(report["comparison"]["baseline"]["issues"]), observer.MAX_ROWS)
        self.assertEqual(report["comparison"]["baseline"]["omitted_issues"], 5)

    def test_comparison_budget_prioritizes_new_nonrun_over_recoveries(self):
        baseline = self.root / "baseline.xml"
        recoveries = observer.MAX_ROWS
        baseline.write_text(f'<testsuite tests="{recoveries + 2}"><testcase name="important-new-skip" status="run"/><testcase name="important-new-failure" status="notrun"/>' + ''.join(f'<testcase name="recovered-{i}" status="notrun"/>' for i in range(recoveries)) + '</testsuite>', encoding="utf-8")
        self.report.write_text(f'<testsuite tests="{recoveries + 2}"><testcase name="important-new-skip" status="notrun"/><testcase name="important-new-failure" status="fail"><failure/></testcase>' + ''.join(f'<testcase name="recovered-{i}" status="run"/>' for i in range(recoveries)) + '</testsuite>', encoding="utf-8")
        report, cases = observer.observe_with_cases(self.report)
        observer.compare(report, cases, baseline)
        comparison = report["comparison"]
        self.assertTrue(any(row["name"] == "important-new-skip" for row in comparison["transitions"]))
        failure = next(row for row in comparison["transitions"] if row["name"] == "important-new-failure")
        self.assertEqual(failure["kind"], "newly_failed")
        self.assertEqual(comparison["omitted_transitions"], 2)

    def test_comparison_has_one_global_row_budget(self):
        baseline = self.root / "baseline.xml"
        count = observer.MAX_ROWS + 20
        baseline.write_text(f'<testsuite tests="{count}">' + ''.join(f'<testcase name="base-{i}" status="run"/>' for i in range(count)) + '</testsuite>', encoding="utf-8")
        self.report.write_text(f'<testsuite tests="{count}">' + ''.join(f'<testcase name="current-{i}" status="run"/>' for i in range(count)) + '</testsuite>', encoding="utf-8")
        report, cases = observer.observe_with_cases(self.report)
        observer.compare(report, cases, baseline)
        comparison = report["comparison"]
        shown = (len(comparison["transitions"]) + len(comparison["ambiguous_duplicate_groups"])
                 + len(comparison["current_only"]) + len(comparison["baseline_only"]))
        self.assertEqual(shown, observer.MAX_ROWS)
        self.assertEqual(comparison["current_only_count"], count)
        self.assertEqual(comparison["baseline_only_count"], count)
        self.assertEqual(comparison["omitted_current_only"] + comparison["omitted_baseline_only"], count * 2 - observer.MAX_ROWS)
        rendered = observer.markdown(report)
        self.assertIn("current only", rendered)
        self.assertIn("Omitted", rendered)

    def test_json_output_matches_stdout_and_rejects_symlink(self):
        output = self.root / "observation.json"
        self.report.write_text('<testsuite tests="1"><testcase name="ok" status="run"/></testsuite>', encoding="utf-8")
        result = subprocess.run([sys.executable, str(SCRIPT), str(self.report), "--json", "--json-output", str(output)], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(json.loads(result.stdout), json.loads(output.read_text()))
        output.unlink()
        output.symlink_to(self.report)
        result = subprocess.run([sys.executable, str(SCRIPT), str(self.report), "--json-output", str(output)], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 2)
        self.assertIn("JSON output unavailable", result.stderr)
        self.assertTrue(output.is_symlink())

    def test_json_output_cannot_replace_input(self):
        original = '<testsuite tests="1"><testcase name="ok" status="run"/></testsuite>'
        self.report.write_text(original, encoding="utf-8")
        result = subprocess.run([sys.executable, str(SCRIPT), str(self.report), "--json-output", str(self.report)], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 2)
        self.assertEqual(self.report.read_text(), original)
        self.assertIn("must not replace an input", result.stderr)

    def test_json_output_cannot_replace_baseline_or_hardlink(self):
        baseline = self.root / "baseline.xml"
        current = '<testsuite tests="1"><testcase name="ok" status="run"/></testsuite>'
        baseline.write_text(current, encoding="utf-8")
        self.report.write_text(current, encoding="utf-8")
        for output in (baseline, self.root / "baseline-hardlink.xml"):
            if output != baseline:
                os.link(baseline, output)
            with self.subTest(output=output.name):
                result = subprocess.run([sys.executable, str(SCRIPT), str(self.report), "--baseline", str(baseline), "--json-output", str(output)], capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 2)
                self.assertEqual(baseline.read_text(), current)
                self.assertIn("must not replace an input", result.stderr)
            if output != baseline:
                output.unlink()

    def test_output_bound_does_not_truncate_counts(self):
        count = observer.MAX_ROWS + 7
        report = self.observe(f'<testsuite tests="{count}">' + '<testcase name="a" status="notrun"/>' * count + '</testsuite>')
        self.assertEqual(report["counts"]["notrun"], count)
        self.assertEqual(len(report["nonruns"]), observer.MAX_ROWS)
        self.assertEqual(report["omitted_nonruns"], 7)

    def test_doctype_rejected_in_both_encodings(self):
        xml = '<!DOCTYPE testsuite [<!ENTITY e "injected">]><testsuite tests="1"><testcase name="&e;" status="run"/></testsuite>'
        for encoding in ("utf-8", "utf-16"):
            with self.subTest(encoding=encoding):
                self.report.write_bytes(xml.encode(encoding))
                self.assertEqual(observer.observe(self.report)["observation"], "unavailable")

    def test_oversize_and_directory_rejected(self):
        self.report.write_bytes(b"x" * 10)
        with patch.object(observer, "MAX_BYTES", 9):
            self.assertEqual(observer.observe(self.report)["observation"], "unavailable")
        self.assertEqual(observer.observe(self.root)["observation"], "unavailable")

    @unittest.skipUnless(hasattr(os, "mkfifo"), "POSIX FIFO")
    def test_fifo_does_not_block(self):
        os.mkfifo(self.report)
        result = subprocess.run([sys.executable, str(SCRIPT), str(self.report), "--json"], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 2)
        self.assertEqual(json.loads(result.stdout)["observation"], "unavailable")

    def test_markdown_cannot_inject_table_or_html(self):
        rendered = observer.cell('<script>|`\n\x1b')
        self.assertNotIn("<script>", rendered)
        self.assertNotIn("|", rendered)
        self.assertNotIn("`", rendered)
        self.assertNotIn("\x1b", rendered)

    def test_json_from_external_cwd_is_same_observation(self):
        report = self.observe('<testsuite tests="1"><testcase name="fail" status="fail"><failure/></testcase></testsuite>')
        result = subprocess.run([sys.executable, str(SCRIPT), str(self.report), "--json"], cwd=self.root, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(json.loads(result.stdout), report)
        self.assertIn("observation=observed", result.stderr)

    def test_missing_json_exits_two_not_test_failure(self):
        result = subprocess.run([sys.executable, str(SCRIPT), str(self.report), "--json"], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 2)
        self.assertEqual(json.loads(result.stdout)["observation"], "unavailable")

    @unittest.skipUnless(shutil.which("ctest"), "real CTest not installed")
    def test_real_ctest_pass_fail_skip_disabled(self):
        python = Path(sys.executable).as_posix()
        self.root.joinpath("CTestTestfile.cmake").write_text(
            f'add_test(pass "{python}" "-c" "raise SystemExit(0)")\n'
            f'add_test(fail "{python}" "-c" "raise SystemExit(1)")\n'
            f'add_test(skip "{python}" "-c" "raise SystemExit(77)")\n'
            'set_tests_properties(skip PROPERTIES SKIP_RETURN_CODE 77)\n'
            f'add_test(disabled "{python}" "-c" "raise SystemExit(1)")\n'
            'set_tests_properties(disabled PROPERTIES DISABLED TRUE)\n', encoding="utf-8")
        result = subprocess.run([shutil.which("ctest"), "--test-dir", str(self.root), "-j1", "--output-junit", str(self.report)], capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 8, result.stdout + result.stderr)
        report = observer.observe(self.report)
        self.assertEqual(report["observation"], "observed", self.report.read_text())
        self.assertEqual(report["counts"], dict(run=1, fail=1, notrun=1, disabled=1, unknown=0))


if __name__ == "__main__":
    unittest.main()

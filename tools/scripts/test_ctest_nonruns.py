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
        self.assertIn('>> "$GITHUB_STEP_SUMMARY"', step)
        self.assertNotIn("ET.parse", step)
        self.assertNotIn("pip install", step)

    def test_duplicate_names_are_not_collapsed(self):
        report = self.observe('<testsuite tests="2">' + '<testcase name="a" status="notrun"/>' * 2 + '</testsuite>')
        self.assertEqual([r["index"] for r in report["nonruns"]], [1, 2])

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

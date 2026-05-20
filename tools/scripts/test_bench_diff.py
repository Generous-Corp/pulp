#!/usr/bin/env python3
"""Coverage-lane tests for tools/scripts/bench_diff.py."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import pathlib
import runpy
import sys
import tempfile
import unittest
from unittest import mock


SCRIPT = pathlib.Path(__file__).resolve().parent / "bench_diff.py"
spec = importlib.util.spec_from_file_location("bench_diff", SCRIPT)
assert spec and spec.loader
bd = importlib.util.module_from_spec(spec)
sys.modules["bench_diff"] = bd
spec.loader.exec_module(bd)


BASELINE = {
    "host": "baseline-host",
    "pulp_commit": "1234567890abcdef",
    "platform": "darwin-arm64",
    "widget": "oscilloscope",
    "seconds": 10,
    "samples": 600,
    "per_frame_us": {
        "gpu_dispatch_us": 40.0,
        "total_frame_us": 120.0,
    },
    "per_frame_bytes": {
        "cpu_to_gpu_bytes": 4096.0,
        "gpu_to_cpu_bytes": 0.0,
    },
    "memory_bandwidth_fraction": 0.12,
}

CURRENT = {
    "host": "current-host",
    "pulp_commit": "fedcba0987654321",
    "platform": "linux-x64",
    "widget": "oscilloscope",
    "seconds": 12,
    "samples": 720,
    "per_frame_us": {
        "gpu_dispatch_us": 30.0,
        "total_frame_us": 100.0,
        "new_metric_us": 5.0,
    },
    "per_frame_bytes": {
        "cpu_to_gpu_bytes": 2048.0,
        "new_bytes": 2_000_000.0,
    },
    "memory_bandwidth_fraction": 0.06,
}


class FormatterTests(unittest.TestCase):
    def test_formatters_cover_units_and_delta_edges(self) -> None:
        self.assertEqual(bd.fmt_us(1.25).strip(), "1.25 \u00b5s")
        self.assertEqual(bd.fmt_bytes(999.0), "   999 B ")
        self.assertEqual(bd.fmt_bytes(12_345.0), " 12.35 KB")
        self.assertEqual(bd.fmt_bytes(2_500_000.0), "  2.50 MB")
        self.assertEqual(bd.fmt_pct(0.1234), "12.34%")
        self.assertEqual(bd.fmt_delta(0.0, 0.0), "  =   ")
        self.assertEqual(bd.fmt_delta(0.0, 5.0), "  new ")
        self.assertIn("- 50.0%", bd.fmt_delta(10.0, 5.0))
        self.assertIn("+ 50.0%", bd.fmt_delta(10.0, 15.0, lower_is_better=False))

    def test_diff_section_unions_sorted_metric_keys(self) -> None:
        lines = bd.diff_section(
            "Latency",
            {"z_before": 1.0, "shared": 2.0},
            {"a_after": 3.0, "shared": 1.0},
            bd.fmt_us,
        )

        self.assertEqual(lines[:3], ["## Latency", "", "| Metric | Baseline | Current | \u0394 |"])
        self.assertIn("| a_after |    0.00 \u00b5s |    3.00 \u00b5s |   new  |", lines)
        self.assertLess(lines.index("| a_after |    0.00 \u00b5s |    3.00 \u00b5s |   new  |"), lines.index("| shared |    2.00 \u00b5s |    1.00 \u00b5s | \u2193 - 50.0% |"))


class MainTests(unittest.TestCase):
    def _write_json(self, root: pathlib.Path, name: str, payload: dict[str, object]) -> pathlib.Path:
        path = root / name
        path.write_text(json.dumps(payload), encoding="utf-8")
        return path

    def _run_main(
        self,
        baseline: dict[str, object],
        current: dict[str, object],
        *extra_args: str,
    ) -> tuple[int, str, str]:
        stdout = io.StringIO()
        stderr = io.StringIO()
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            baseline_path = self._write_json(root, "baseline.json", baseline)
            current_path = self._write_json(root, "current.json", current)
            argv = ["bench_diff.py", *extra_args, str(baseline_path), str(current_path)]
            with contextlib.redirect_stdout(stdout), \
                 contextlib.redirect_stderr(stderr):
                old_argv = sys.argv
                try:
                    sys.argv = argv
                    rc = bd.main()
                finally:
                    sys.argv = old_argv
        return rc, stdout.getvalue(), stderr.getvalue()

    def test_main_renders_markdown_improvement_report(self) -> None:
        rc, stdout, stderr = self._run_main(BASELINE, CURRENT, "--threshold", "0.10")

        self.assertEqual(rc, 0)
        self.assertEqual(stderr, "")
        self.assertIn("# Bench diff: oscilloscope", stdout)
        self.assertIn("- **Baseline:** baseline.json (12345678 on baseline-host)", stdout)
        self.assertIn("- **Current:** current.json (fedcba09 on current-host)", stdout)
        self.assertIn("| new_metric_us |    0.00", stdout)
        self.assertIn("| new_bytes |      0 B  |   2.00 MB |   new", stdout)
        self.assertIn("**Baseline** exceeds 10.00% threshold", stdout)
        self.assertIn("**Current** reduced memory-bandwidth fraction by 50.00%", stdout)

    def test_main_warns_for_widget_mismatch_and_plain_text_strips_tables(self) -> None:
        current = dict(CURRENT)
        current["widget"] = "scope-alt"
        current["memory_bandwidth_fraction"] = 0.20

        rc, stdout, stderr = self._run_main(BASELINE, current, "--format", "text")

        self.assertEqual(rc, 0)
        self.assertIn("warning: widget mismatch", stderr)
        self.assertIn("baseline=oscilloscope, current=scope-alt", stderr)
        self.assertIn("Metric    Baseline    Current", stdout)
        self.assertNotIn("| Metric |", stdout)
        self.assertIn("**Current** regressed", stdout)

    def test_main_reports_missing_baseline_and_current_bandwidth(self) -> None:
        baseline = dict(BASELINE)
        current = dict(CURRENT)
        baseline.pop("memory_bandwidth_fraction")
        current.pop("memory_bandwidth_fraction")

        rc, stdout, stderr = self._run_main(baseline, current)

        self.assertEqual(rc, 0)
        self.assertEqual(stderr, "")
        self.assertIn("- Baseline: (not reported)", stdout)
        self.assertIn("- Current:  (not reported)", stdout)
        self.assertNotIn("- \u0394:", stdout)
        self.assertIn("threshold evaluation skipped", stdout)
        self.assertIn("no before/after comparison possible", stdout)

    def test_main_requests_fresh_baseline_when_only_current_reports_bandwidth(self) -> None:
        baseline = dict(BASELINE)
        baseline.pop("memory_bandwidth_fraction")

        rc, stdout, stderr = self._run_main(baseline, CURRENT)

        self.assertEqual(rc, 0)
        self.assertEqual(stderr, "")
        self.assertIn("Current reported the fraction but baseline didn't", stdout)

    def test_main_reports_below_threshold_and_no_change(self) -> None:
        baseline = dict(BASELINE)
        current = dict(CURRENT)
        baseline["memory_bandwidth_fraction"] = 0.02
        current["memory_bandwidth_fraction"] = 0.02

        rc, stdout, stderr = self._run_main(baseline, current)

        self.assertEqual(rc, 0)
        self.assertEqual(stderr, "")
        self.assertIn("**Baseline** below", stdout)
        self.assertIn("5.00% threshold", stdout)
        self.assertIn("No change in memory-bandwidth fraction.", stdout)

    def test_main_reports_malformed_input(self) -> None:
        stdout = io.StringIO()
        stderr = io.StringIO()
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            baseline_path = root / "bad.json"
            baseline_path.write_text("{bad", encoding="utf-8")
            current_path = self._write_json(root, "current.json", CURRENT)
            argv = ["bench_diff.py", str(baseline_path), str(current_path)]
            with contextlib.redirect_stdout(stdout), \
                 contextlib.redirect_stderr(stderr):
                old_argv = sys.argv
                try:
                    sys.argv = argv
                    rc = bd.main()
                finally:
                    sys.argv = old_argv

        self.assertEqual(rc, 1)
        self.assertEqual(stdout.getvalue(), "")
        self.assertIn("error:", stderr.getvalue())

    def test_script_entrypoint_exits_zero(self) -> None:
        stdout = io.StringIO()
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            baseline_path = self._write_json(root, "baseline.json", BASELINE)
            current_path = self._write_json(root, "current.json", CURRENT)
            with mock.patch.object(sys, "argv", [str(SCRIPT), str(baseline_path), str(current_path)]), \
                 contextlib.redirect_stdout(stdout):
                with self.assertRaises(SystemExit) as cm:
                    runpy.run_path(str(SCRIPT), run_name="__main__")

        self.assertEqual(cm.exception.code, 0)
        self.assertIn("# Bench diff: oscilloscope", stdout.getvalue())


if __name__ == "__main__":
    unittest.main()

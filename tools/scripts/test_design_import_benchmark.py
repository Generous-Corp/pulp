#!/usr/bin/env python3
"""Tests for tools/scripts/design_import_benchmark.py."""

from __future__ import annotations

import importlib.util
import pathlib
import sys
import tempfile
import unittest


SCRIPT = pathlib.Path(__file__).resolve().parent / "design_import_benchmark.py"
spec = importlib.util.spec_from_file_location("design_import_benchmark", SCRIPT)
assert spec and spec.loader
dib = importlib.util.module_from_spec(spec)
sys.modules["design_import_benchmark"] = dib
spec.loader.exec_module(dib)


def write_sized(path: pathlib.Path, size: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"x" * size)


class BinarySizeTests(unittest.TestCase):
    def test_measure_binary_sizes_sums_live_runtime_objects_and_gate(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            build = root / "build"
            bench = build / "tools" / "import-design" / "pulp-design-import-bench"
            write_sized(build / "core" / "view" / "libpulp-view.a", 10_000)
            write_sized(bench, 40_000)
            write_sized(build / "obj-a.o", 1_500)
            write_sized(build / "obj-b.o", 2_500)

            result = dib.measure_binary_sizes(
                build,
                bench,
                object_paths=["obj-a.o", "obj-b.o", "missing.o"],
            )

        self.assertEqual(result["pulp_view_archive_bytes"], 10_000)
        self.assertEqual(result["benchmark_app_bytes"], 40_000)
        self.assertEqual(result["pulp_view_archive_linked_bytes"], 10_000)
        self.assertEqual(result["benchmark_app_linked_bytes"], 40_000)
        self.assertEqual(result["live_runtime_estimated_object_bytes"], 4_000)
        self.assertEqual(result["live_runtime_estimated_linked_bytes"], 4_000)
        self.assertEqual(result["hypothetical_baked_only_pulp_view_linked_bytes"], 6_000)
        self.assertAlmostEqual(result["delta_percent_of_pulp_view_archive"], 0.4)
        self.assertAlmostEqual(result["delta_percent_of_benchmark_app"], 0.1)
        self.assertTrue(result["phase9_threshold_met"])
        self.assertFalse(result["objects"][2]["present"])

    def test_measure_binary_sizes_gate_can_defer_when_threshold_not_met(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            build = root / "build"
            bench = build / "tools" / "import-design" / "pulp-design-import-bench"
            write_sized(build / "core" / "view" / "libpulp-view.a", 10_000)
            write_sized(bench, 10_000)
            write_sized(build / "obj-a.o", 1_000)

            result = dib.measure_binary_sizes(build, bench, object_paths=["obj-a.o"])

        self.assertEqual(result["live_runtime_estimated_object_bytes"], 1_000)
        self.assertEqual(result["live_runtime_estimated_linked_bytes"], 1_000)
        self.assertAlmostEqual(result["delta_percent_of_pulp_view_archive"], 0.1)
        self.assertFalse(result["phase9_threshold_met"])


class ReportTests(unittest.TestCase):
    def test_comparison_and_markdown_record_phase9_gate_inputs(self) -> None:
        live = {
            "fixture": "fixture",
            "target_fps": 60,
            "startup": {"first_frame_ms": 100.0, "build_ms": 20.0, "first_frame_render_ms": 5.0},
            "idle": {
                "samples": 10,
                "cpu_ms": 20.0,
                "cpu_frame_ms_median": 0.1,
                "cpu_frame_ms_p99": 0.2,
                "frame_ms_median": 1.0,
                "frame_ms_p99": 2.0,
                "rss_median_bytes": 900,
                "rss_p99_bytes": 1024,
                "rss_peak_bytes": 1024,
            },
            "interactive": {
                "samples": 10,
                "cpu_ms": 40.0,
                "cpu_frame_ms_median": 0.3,
                "cpu_frame_ms_p99": 0.4,
                "frame_ms_median": 2.0,
                "frame_ms_p99": 4.0,
                "rss_median_bytes": 1800,
                "rss_p99_bytes": 2048,
                "rss_peak_bytes": 2048,
                "js_evaluations_total": 10,
            },
        }
        baked = {
            "fixture": "fixture",
            "target_fps": 60,
            "startup": {"first_frame_ms": 60.0, "build_ms": 10.0, "first_frame_render_ms": 3.0},
            "idle": {
                "samples": 10,
                "cpu_ms": 10.0,
                "cpu_frame_ms_median": 0.05,
                "cpu_frame_ms_p99": 0.1,
                "frame_ms_median": 0.5,
                "frame_ms_p99": 1.0,
                "rss_median_bytes": 450,
                "rss_p99_bytes": 512,
                "rss_peak_bytes": 512,
            },
            "interactive": {
                "samples": 10,
                "cpu_ms": 15.0,
                "cpu_frame_ms_median": 0.1,
                "cpu_frame_ms_p99": 0.2,
                "frame_ms_median": 1.0,
                "frame_ms_p99": 2.0,
                "rss_median_bytes": 900,
                "rss_p99_bytes": 1024,
                "rss_peak_bytes": 1024,
                "js_evaluations_total": 0,
            },
        }
        binary = {
            "pulp_view_archive_bytes": 10_000,
            "pulp_view_archive_linked_bytes": 10_000,
            "benchmark_app_bytes": 40_000,
            "benchmark_app_linked_bytes": 40_000,
            "live_runtime_estimated_object_bytes": 4_000,
            "live_runtime_estimated_linked_bytes": 4_000,
            "hypothetical_baked_only_pulp_view_linked_bytes": 6_000,
            "delta_percent_of_pulp_view_archive": 0.4,
            "delta_percent_of_benchmark_app": 0.1,
            "phase9_gate_percent_denominator": "pulp-view linked section bytes",
            "measurement_method": "test method",
            "phase9_threshold_met": True,
            "objects": [{"path": "obj.o", "linked_bytes": 4_000, "file_bytes": 5_000, "present": True}],
        }

        report = dib.build_report(live, baked, binary, pathlib.Path("/tmp/build"))
        self.assertAlmostEqual(report["comparison"]["interactive_cpu_delta_ratio"], -0.625)
        self.assertAlmostEqual(report["comparison"]["interactive_cpu_frame_p99_delta_ratio"], -0.5)
        self.assertAlmostEqual(report["comparison"]["interactive_rss_p99_delta_ratio"], -0.5)
        self.assertEqual(report["comparison"]["live_interactive_js_evaluations"], 10)

        markdown = dib.render_markdown(report)
        self.assertIn("Phase 9 gate: **MET**", markdown)
        self.assertIn("CPU frame p99", markdown)
        self.assertIn("RSS p99", markdown)
        self.assertIn("Estimated live-runtime linked footprint", markdown)
        self.assertIn("pulp-view linked text+data", markdown)
        self.assertIn("live interactive evaluations=10", markdown)

    def test_formatters_are_ascii_and_stable(self) -> None:
        self.assertEqual(dib.format_bytes(512), "512 B")
        self.assertEqual(dib.format_bytes(2048), "2.00 KiB")
        self.assertEqual(dib.format_bytes(3 * 1024 * 1024), "3.00 MiB")
        self.assertEqual(dib.format_pct(0.125), "12.50%")
        self.assertEqual(dib.format_ms(1.234), "1.23 ms")


if __name__ == "__main__":
    unittest.main()

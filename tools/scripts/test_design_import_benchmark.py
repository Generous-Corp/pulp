#!/usr/bin/env python3
"""Tests for tools/scripts/design_import_benchmark.py."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import os
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
    def test_default_paths_prefer_existing_build_outputs(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            build = pathlib.Path(td) / "build"
            exe_name = "pulp-design-import-bench.exe" if os.name == "nt" else "pulp-design-import-bench"
            debug_exe = build / "tools" / "import-design" / "Debug" / exe_name
            primary_exe = build / "tools" / "import-design" / exe_name
            write_sized(debug_exe, 1)

            self.assertEqual(dib.default_bench_exe(build), debug_exe)

            write_sized(primary_exe, 1)
            self.assertEqual(dib.default_bench_exe(build), primary_exe)

            release_archive = build / "core" / "view" / "Release" / "pulp-view.lib"
            primary_archive = build / "core" / "view" / "libpulp-view.a"
            write_sized(release_archive, 1)
            self.assertEqual(dib.default_pulp_view_archive(build), release_archive)

            write_sized(primary_archive, 1)
            self.assertEqual(dib.default_pulp_view_archive(build), primary_archive)

    def test_object_lookup_prefers_pulp_view_obj_variant(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            build = pathlib.Path(td) / "build"
            preferred = build / "core" / "view" / "CMakeFiles" / "pulp-view.dir" / "src" / "widget_bridge.cpp.obj"
            fallback = build / "other" / "widget_bridge.cpp.obj"
            direct = build / "direct" / "object.obj"
            write_sized(fallback, 10)
            write_sized(preferred, 20)
            write_sized(direct, 30)

            variants = dib.object_name_variants("core/view/CMakeFiles/pulp-view.dir/src/widget_bridge.cpp.o")
            obj_variants = dib.object_name_variants("core/view/CMakeFiles/pulp-view.dir/src/widget_bridge.cpp.obj")
            found = dib.find_build_object(build, "core/view/CMakeFiles/pulp-view.dir/src/widget_bridge.cpp.o")
            direct_found = dib.find_build_object(build, "direct/object.obj")
            missing = dib.find_build_object(build, "missing-object.o")

        self.assertIn("widget_bridge.cpp.o", variants)
        self.assertIn("widget_bridge.cpp.obj", variants)
        self.assertIn("widget_bridge.cpp.o", obj_variants)
        self.assertEqual(found, preferred)
        self.assertEqual(direct_found, direct)
        self.assertIsNone(missing)

    def test_parse_size_output_ignores_headers_and_non_numeric_rows(self) -> None:
        output = """\
   text    data     bss     dec     hex filename
     10      20       1      31      1f object-a.o
not-a-row
      5       7       2      14       e object-b.o
      4    nope       0       4       4 object-c.o
"""
        self.assertEqual(dib.parse_size_output(output), 42)

    def test_linked_size_falls_back_to_stat_when_size_tool_is_missing(self) -> None:
        old_tool = os.environ.get("PULP_SIZE_TOOL")
        try:
            with tempfile.TemporaryDirectory() as td:
                path = pathlib.Path(td) / "object.o"
                write_sized(path, 1234)
                os.environ["PULP_SIZE_TOOL"] = str(pathlib.Path(td) / "missing-size-tool")

                self.assertEqual(dib.linked_size_bytes(path), 1234)
        finally:
            if old_tool is None:
                os.environ.pop("PULP_SIZE_TOOL", None)
            else:
                os.environ["PULP_SIZE_TOOL"] = old_tool

    def test_linked_size_uses_parsed_text_and_data_when_size_tool_succeeds(self) -> None:
        old_tool = os.environ.get("PULP_SIZE_TOOL")
        try:
            with tempfile.TemporaryDirectory() as td:
                root = pathlib.Path(td)
                path = root / "object.o"
                tool = root / "fake-size"
                write_sized(path, 1234)
                tool.write_text(
                    "#!/bin/sh\n"
                    "printf '   text    data     bss filename\\n'\n"
                    "printf '     12      34       0 %s\\n' \"$1\"\n",
                    encoding="utf-8",
                )
                tool.chmod(0o755)
                os.environ["PULP_SIZE_TOOL"] = str(tool)

                self.assertEqual(dib.linked_size_bytes(path), 46)
        finally:
            if old_tool is None:
                os.environ.pop("PULP_SIZE_TOOL", None)
            else:
                os.environ["PULP_SIZE_TOOL"] = old_tool

    def test_linked_size_falls_back_when_size_tool_output_has_no_sections(self) -> None:
        old_tool = os.environ.get("PULP_SIZE_TOOL")
        try:
            with tempfile.TemporaryDirectory() as td:
                root = pathlib.Path(td)
                path = root / "object.o"
                tool = root / "fake-size-empty"
                write_sized(path, 321)
                tool.write_text("#!/bin/sh\nprintf 'not numeric\\n'\n", encoding="utf-8")
                tool.chmod(0o755)
                os.environ["PULP_SIZE_TOOL"] = str(tool)

                self.assertEqual(dib.linked_size_bytes(path), 321)
        finally:
            if old_tool is None:
                os.environ.pop("PULP_SIZE_TOOL", None)
            else:
                os.environ["PULP_SIZE_TOOL"] = old_tool

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
    def test_no_launch_env_sets_headless_flags_and_preserves_existing_env(self) -> None:
        old_value = os.environ.get("PULP_BENCH_EXISTING")
        try:
            os.environ["PULP_BENCH_EXISTING"] = "keep-me"
            env = dib.no_launch_env()
        finally:
            if old_value is None:
                os.environ.pop("PULP_BENCH_EXISTING", None)
            else:
                os.environ["PULP_BENCH_EXISTING"] = old_value

        self.assertEqual(env["PULP_DISABLE_PLUGIN_EDITOR"], "1")
        self.assertEqual(env["PULP_HEADLESS"], "1")
        self.assertEqual(env["PULP_TEST_MODE"], "1")
        self.assertEqual(env["PULP_INSPECTOR_NO_LAUNCH"], "1")
        self.assertEqual(env["PULP_BENCH_EXISTING"], "keep-me")

    def test_delta_ratio_handles_missing_or_zero_baseline(self) -> None:
        self.assertIsNone(dib.delta_ratio(1, 0))
        self.assertIsNone(dib.delta_ratio(1, None))
        self.assertIsNone(dib.delta_ratio(None, 1))
        self.assertEqual(dib.delta_ratio(75, 100), -0.25)

    def test_run_lane_builds_command_and_reads_json_output(self) -> None:
        calls: list[dict[str, object]] = []
        old_run = dib.subprocess.run

        def fake_run(cmd: list[str], **kwargs: object) -> object:
            calls.append({"cmd": cmd, **kwargs})
            out_arg = next(arg for arg in cmd if arg.startswith("--output="))
            out_path = pathlib.Path(out_arg.split("=", 1)[1])
            out_path.write_text('{"lane": "live", "samples": 7}\n', encoding="utf-8")
            return object()

        try:
            with tempfile.TemporaryDirectory() as td:
                dib.subprocess.run = fake_run  # type: ignore[assignment]
                result = dib.run_lane(pathlib.Path("/tmp/fake-bench"), "live", 10, 20, 30, pathlib.Path(td))
        finally:
            dib.subprocess.run = old_run

        self.assertEqual(result["lane"], "live")
        self.assertEqual(result["samples"], 7)
        self.assertEqual(calls[0]["cmd"][1:5], [
            "--lane=live",
            "--idle-ms=10",
            "--interactive-ms=20",
            "--target-fps=30",
        ])
        self.assertEqual(calls[0]["check"], True)
        self.assertEqual(calls[0]["timeout"], 30)
        self.assertEqual(calls[0]["env"]["PULP_HEADLESS"], "1")  # type: ignore[index]

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

    def test_main_skip_run_writes_json_and_markdown_without_benchmark(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            output_json = root / "out" / "report.json"
            output_md = root / "out" / "report.md"

            rc = dib.main([
                "--build-dir", str(root / "build"),
                "--bench-exe", str(root / "missing-bench"),
                "--skip-run",
                "--target-fps", "144",
                "--output-json", str(output_json),
                "--output-md", str(output_md),
                "--object", "missing-object.o",
            ])

            report = json.loads(output_json.read_text(encoding="utf-8"))
            markdown = output_md.read_text(encoding="utf-8")

        self.assertEqual(rc, 0)
        self.assertEqual(report["schema"], "pulp-design-import-benchmark-summary-v1")
        self.assertEqual(report["lanes"]["live"]["fixture"], "not-run")
        self.assertEqual(report["lanes"]["baked-native"]["target_fps"], 144)
        self.assertEqual(report["binary_size"]["benchmark_app_bytes"], 0)
        self.assertEqual(report["binary_size"]["objects"][0]["path"], "missing-object.o")
        self.assertIn("Phase 9 gate: **NOT MET**", markdown)

    def test_main_reports_missing_benchmark_without_skip_run(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            err = io.StringIO()
            missing = pathlib.Path(td) / "missing-bench"
            with contextlib.redirect_stderr(err):
                rc = dib.main([
                    "--build-dir", str(pathlib.Path(td) / "build"),
                    "--bench-exe", str(missing),
                ])

        self.assertEqual(rc, 1)
        self.assertIn("benchmark executable not found", err.getvalue())

    def test_main_runs_lanes_and_prints_markdown_when_no_outputs_are_requested(self) -> None:
        old_run_lane = dib.run_lane

        def fake_run_lane(
            bench_exe: pathlib.Path,
            lane: str,
            idle_ms: int,
            interactive_ms: int,
            target_fps: int,
            output_dir: pathlib.Path,
        ) -> dict[str, object]:
            self.assertTrue(bench_exe.exists())
            self.assertEqual(idle_ms, 3)
            self.assertEqual(interactive_ms, 4)
            self.assertEqual(target_fps, 5)
            self.assertTrue(output_dir.exists())
            return {
                "lane": lane,
                "fixture": "fake",
                "target_fps": target_fps,
                "startup": {"first_frame_ms": 1.0},
                "idle": {"samples": 0},
                "interactive": {"samples": 0, "js_evaluations_total": 0},
            }

        try:
            with tempfile.TemporaryDirectory() as td:
                root = pathlib.Path(td)
                bench = root / "bench"
                write_sized(bench, 1)
                out = io.StringIO()
                dib.run_lane = fake_run_lane
                with contextlib.redirect_stdout(out):
                    rc = dib.main([
                        "--build-dir", str(root / "build"),
                        "--bench-exe", str(bench),
                        "--idle-ms", "3",
                        "--interactive-ms", "4",
                        "--target-fps", "5",
                    ])
        finally:
            dib.run_lane = old_run_lane

        self.assertEqual(rc, 0)
        self.assertIn("# Design-Import Benchmark Results", out.getvalue())
        self.assertIn("Fixture: `fake`", out.getvalue())


if __name__ == "__main__":
    unittest.main()

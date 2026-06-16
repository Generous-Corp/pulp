#!/usr/bin/env python3
"""Unit tests for summarize_daw_bench_results.py."""

from __future__ import annotations

import importlib.util
import contextlib
import io
import json
import pathlib
import sys
import tempfile
import unittest


SCRIPT_DIR = pathlib.Path(__file__).resolve().parent

checker_spec = importlib.util.spec_from_file_location(
    "check_daw_bench_evidence",
    SCRIPT_DIR / "check_daw_bench_evidence.py",
)
assert checker_spec and checker_spec.loader
checker = importlib.util.module_from_spec(checker_spec)
sys.modules["check_daw_bench_evidence"] = checker
checker_spec.loader.exec_module(checker)

summary_spec = importlib.util.spec_from_file_location(
    "summarize_daw_bench_results",
    SCRIPT_DIR / "summarize_daw_bench_results.py",
)
assert summary_spec and summary_spec.loader
summary = importlib.util.module_from_spec(summary_spec)
sys.modules["summarize_daw_bench_results"] = summary
summary_spec.loader.exec_module(summary)


def _write(path: pathlib.Path, body: str) -> pathlib.Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(body, encoding="utf-8")
    return path


def _manifest(**overrides: object) -> dict[str, object]:
    data: dict[str, object] = {
        "schema_version": 1,
        "host": "REAPER",
        "format": "VST3",
        "daw_version": "7.74/macOS-arm64",
        "os": "macOS 26.5.1 arm64",
        "date": "2026-06-12",
        "script": "06-reaper-vst3.md",
        "pulp_commit": "19b3d7c32",
        "plugin_version": "1.0.0",
        "result_markdown": "06-reaper-vst3.md",
        "logs": ["logs/Reaper-VST3-20260612T120000Z-pid42.log"],
        "capabilities": [
            {
                "capability": "load",
                "observed": "Confirmed",
                "notes": "session_start appeared in the checked-in log.",
            },
            {
                "capability": "params",
                "observed": "Confirmed",
                "notes": "define_parameters appeared in the checked-in log.",
            },
        ],
        "quirks": [
            {
                "flag": "reaper_process_while_bypassed",
                "row": "R1",
                "observed": "Confirmed",
                "notes": "process_without_prepare appeared after bypass toggle",
            },
            {
                "flag": "reaper_midsession_setstate",
                "row": "R6",
                "observed": "Not Triggered",
                "notes": "No deserialize_plugin_state event was observed.",
            },
        ],
    }
    data.update(overrides)
    return data


class DawBenchSummaryTests(unittest.TestCase):
    def _repo(self) -> tuple[tempfile.TemporaryDirectory[str], pathlib.Path, pathlib.Path]:
        tmp_ctx = tempfile.TemporaryDirectory()
        root = pathlib.Path(tmp_ctx.name)
        result_dir = root / "docs" / "validation" / "daw-bench" / "results" / "2026-06-12"
        _write(root / "docs" / "validation" / "daw-bench" / "06-reaper-vst3.md",
               "# 06 — REAPER (VST3)\n")
        _write(root / "docs" / "validation" / "daw-bench" / "01-logic-pro-au.md",
               "# 01 — Logic Pro (AU v2)\n")
        _write(root / "docs" / "validation" / "daw-bench" / "08-aum-auv3.md",
               "# 08 — AUM (iOS, AU v3)\n")
        _write(result_dir / "06-reaper-vst3.md",
               "# Filled REAPER result\n")
        _write(result_dir / "logs" / "Reaper-VST3-20260612T120000Z-pid42.log",
               "2026-06-12T12:00:00Z\tsession_start\n"
               "2026-06-12T12:00:00Z\tdefine_parameters\n"
               "2026-06-12T12:00:00Z\tserialize_plugin_state\n"
               "2026-06-12T12:00:00Z\tprocess_without_prepare\n")
        return tmp_ctx, root, result_dir

    def test_load_summaries_validates_and_sorts_manifests(self) -> None:
        tmp_ctx, root, result_dir = self._repo()
        with tmp_ctx:
            path = result_dir / "reaper-vst3.daw-bench.json"
            path.write_text(json.dumps(_manifest()), encoding="utf-8")
            summaries, results = summary.load_summaries([result_dir], repo_root=root)
            self.assertTrue(all(result.ok for result in results))
            self.assertEqual(len(summaries), 1)
            self.assertEqual(summaries[0].host, "REAPER")
            self.assertEqual(summaries[0].format, "VST3")
            self.assertEqual(summaries[0].confirmed, ("reaper_process_while_bypassed",))
            self.assertEqual(summaries[0].not_triggered, ("reaper_midsession_setstate",))
            self.assertEqual(summaries[0].confirmed_capabilities, ("load", "params"))

    def test_load_scripted_lanes_normalizes_script_headings(self) -> None:
        tmp_ctx, root, _result_dir = self._repo()
        with tmp_ctx:
            lanes = summary.load_scripted_lanes(
                root / "docs" / "validation" / "daw-bench",
                repo_root=root,
            )
            self.assertIn(summary.PlannedLane(
                host="Logic Pro",
                format="AU",
                script=pathlib.Path("docs/validation/daw-bench/01-logic-pro-au.md"),
            ), lanes)
            self.assertIn(summary.PlannedLane(
                host="AUM",
                format="AUv3",
                script=pathlib.Path("docs/validation/daw-bench/08-aum-auv3.md"),
            ), lanes)

    def test_markdown_report_includes_run_and_confirmed_quirk_table(self) -> None:
        tmp_ctx, root, result_dir = self._repo()
        with tmp_ctx:
            path = result_dir / "reaper-vst3.daw-bench.json"
            path.write_text(json.dumps(_manifest()), encoding="utf-8")
            summaries, _results = summary.load_summaries([result_dir], repo_root=root)
            planned_lanes = summary.load_scripted_lanes(
                root / "docs" / "validation" / "daw-bench",
                repo_root=root,
            )
            markdown = summary.render_markdown(
                summaries,
                repo_root=root,
                planned_lanes=planned_lanes,
            )
            self.assertIn("- Manifests: 1", markdown)
            self.assertIn("- Confirmed capability observations: 2", markdown)
            self.assertIn("| 2026-06-12 | REAPER | VST3 |", markdown)
            self.assertIn("`load`, `params`", markdown)
            self.assertIn("`reaper_process_while_bypassed`", markdown)
            self.assertIn("docs/validation/daw-bench/results/2026-06-12/reaper-vst3.daw-bench.json", markdown)
            self.assertIn("## Scripted Lanes Without Checked-In Manifests", markdown)
            self.assertIn("| Host | Format | Local Status | Script |", markdown)
            self.assertIn("| Logic Pro | AU | - | `docs/validation/daw-bench/01-logic-pro-au.md` |", markdown)
            self.assertIn("| AUM | AUv3 | - | `docs/validation/daw-bench/08-aum-auv3.md` |", markdown)

    def test_json_report_is_machine_readable(self) -> None:
        tmp_ctx, root, result_dir = self._repo()
        with tmp_ctx:
            path = result_dir / "reaper-vst3.daw-bench.json"
            path.write_text(json.dumps(_manifest()), encoding="utf-8")
            summaries, _results = summary.load_summaries([result_dir], repo_root=root)
            planned_lanes = summary.load_scripted_lanes(
                root / "docs" / "validation" / "daw-bench",
                repo_root=root,
            )
            data = json.loads(summary.render_json(
                summaries,
                repo_root=root,
                planned_lanes=planned_lanes,
            ))
            self.assertEqual(data["manifest_count"], 1)
            self.assertEqual(data["host_format_count"], 1)
            self.assertEqual(data["scripted_lane_count"], 3)
            self.assertEqual(data["covered_scripted_lane_count"], 1)
            self.assertEqual(data["missing_scripted_lane_count"], 2)
            self.assertEqual(data["latest_result_date"], "2026-06-12")
            self.assertEqual(data["confirmed_quirk_observations"], 1)
            self.assertEqual(data["confirmed_capability_observations"], 2)
            self.assertEqual(data["runs"][0]["confirmed"], ["reaper_process_while_bypassed"])
            self.assertEqual(data["runs"][0]["confirmed_capabilities"], ["load", "params"])
            self.assertEqual(data["scripted_lanes_without_manifests"], [
                {
                    "format": "AU",
                    "host": "Logic Pro",
                    "script": "docs/validation/daw-bench/01-logic-pro-au.md",
                },
                {
                    "format": "AUv3",
                    "host": "AUM",
                    "script": "docs/validation/daw-bench/08-aum-auv3.md",
                },
            ])

    def test_local_host_availability_detects_macos_apps(self) -> None:
        tmp_ctx, root, _result_dir = self._repo()
        with tmp_ctx:
            apps = root / "Applications"
            (apps / "Logic Pro.app").mkdir(parents=True)
            lanes = summary.load_scripted_lanes(
                root / "docs" / "validation" / "daw-bench",
                repo_root=root,
            )
            availability = summary.local_host_availability(lanes, applications_dir=apps)
            self.assertEqual(availability["Logic Pro"].status, "available")
            self.assertIn("Logic Pro.app", availability["Logic Pro"].detail)
            self.assertEqual(availability["AUM"].status, "unavailable")
            self.assertIn("iOS/iPadOS", availability["AUM"].detail)

    def test_local_host_availability_detects_versioned_daw_apps(self) -> None:
        tmp_ctx = tempfile.TemporaryDirectory()
        with tmp_ctx:
            root = pathlib.Path(tmp_ctx.name)
            apps = root / "Applications"
            (apps / "Ableton Live 12 Suite.app").mkdir(parents=True)
            (apps / "Studio One 6.app").mkdir(parents=True)
            (apps / "WaveLab 12.app").mkdir(parents=True)
            lanes = [
                summary.PlannedLane("Ableton Live", "VST3", pathlib.Path("live.md")),
                summary.PlannedLane("Studio One", "VST3", pathlib.Path("studio-one.md")),
                summary.PlannedLane("Wavelab", "VST3", pathlib.Path("wavelab.md")),
                summary.PlannedLane("Bitwig Studio", "VST3", pathlib.Path("bitwig.md")),
            ]
            availability = summary.local_host_availability(lanes, applications_dir=apps)
            self.assertEqual(availability["Ableton Live"].status, "available")
            self.assertIn("Ableton Live 12 Suite.app", availability["Ableton Live"].detail)
            self.assertEqual(availability["Studio One"].status, "available")
            self.assertIn("Studio One 6.app", availability["Studio One"].detail)
            self.assertEqual(availability["Wavelab"].status, "available")
            self.assertIn("WaveLab 12.app", availability["Wavelab"].detail)
            self.assertEqual(availability["Bitwig Studio"].status, "unavailable")

    def test_local_host_availability_accepts_explicit_host_app_override(self) -> None:
        tmp_ctx = tempfile.TemporaryDirectory()
        with tmp_ctx:
            root = pathlib.Path(tmp_ctx.name)
            custom_live = root / "External Apps" / "Ableton Live 12 Suite.app"
            custom_live.mkdir(parents=True)
            lanes = [
                summary.PlannedLane("Ableton Live", "VST3", pathlib.Path("live.md")),
                summary.PlannedLane("Bitwig Studio", "VST3", pathlib.Path("bitwig.md")),
            ]
            availability = summary.local_host_availability(
                lanes,
                applications_dir=root / "Applications",
                host_app_overrides={
                    "ableton live": custom_live,
                    "bitwig studio": root / "Missing" / "Bitwig Studio.app",
                },
            )
            self.assertEqual(availability["Ableton Live"].status, "available")
            self.assertIn("override", availability["Ableton Live"].detail)
            self.assertIn("Ableton Live 12 Suite.app", availability["Ableton Live"].detail)
            self.assertEqual(availability["Bitwig Studio"].status, "unavailable")
            self.assertIn("override missing", availability["Bitwig Studio"].detail)

    def test_parse_host_app_overrides_rejects_bad_values(self) -> None:
        self.assertEqual(
            summary.parse_host_app_overrides(["Ableton Live=/Applications/Live.app"]),
            {"ableton live": pathlib.Path("/Applications/Live.app")},
        )
        with self.assertRaises(ValueError):
            summary.parse_host_app_overrides(["Ableton Live"])
        with self.assertRaises(ValueError):
            summary.parse_host_app_overrides(["=/Applications/Live.app"])

    def test_json_report_can_include_local_host_availability(self) -> None:
        tmp_ctx, root, result_dir = self._repo()
        with tmp_ctx:
            (root / "Applications" / "Logic Pro.app").mkdir(parents=True)
            path = result_dir / "reaper-vst3.daw-bench.json"
            path.write_text(json.dumps(_manifest()), encoding="utf-8")
            summaries, _results = summary.load_summaries([result_dir], repo_root=root)
            planned_lanes = summary.load_scripted_lanes(
                root / "docs" / "validation" / "daw-bench",
                repo_root=root,
            )
            availability = summary.local_host_availability(
                planned_lanes,
                applications_dir=root / "Applications",
            )
            data = json.loads(summary.render_json(
                summaries,
                repo_root=root,
                planned_lanes=planned_lanes,
                host_availability=availability,
            ))
            missing = data["scripted_lanes_without_manifests"]
            self.assertEqual(missing[0]["host"], "Logic Pro")
            self.assertEqual(missing[0]["local_host_status"], "available")
            self.assertIn("Logic Pro.app", missing[0]["local_host_detail"])
            self.assertEqual(missing[1]["host"], "AUM")
            self.assertEqual(missing[1]["local_host_status"], "unavailable")

    def test_cli_json_report_can_include_host_app_override(self) -> None:
        tmp_ctx, root, result_dir = self._repo()
        with tmp_ctx:
            custom_logic = root / "External Apps" / "Logic Pro.app"
            custom_logic.mkdir(parents=True)
            path = result_dir / "reaper-vst3.daw-bench.json"
            path.write_text(json.dumps(_manifest()), encoding="utf-8")
            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                rc = summary.main([
                    str(result_dir),
                    "--repo-root", str(root),
                    "--scripts-dir", str(root / "docs" / "validation" / "daw-bench"),
                    "--format", "json",
                    "--require-any",
                    "--include-local-host-availability",
                    "--host-app", f"Logic Pro={custom_logic}",
                ])
            self.assertEqual(rc, 0)
            data = json.loads(stdout.getvalue())
            missing = data["scripted_lanes_without_manifests"]
            self.assertEqual(missing[0]["host"], "Logic Pro")
            self.assertEqual(missing[0]["local_host_status"], "available")
            self.assertIn("override", missing[0]["local_host_detail"])

    def test_cli_rejects_malformed_host_app_override(self) -> None:
        tmp_ctx, root, result_dir = self._repo()
        with tmp_ctx:
            path = result_dir / "reaper-vst3.daw-bench.json"
            path.write_text(json.dumps(_manifest()), encoding="utf-8")
            with contextlib.redirect_stdout(io.StringIO()), \
                 contextlib.redirect_stderr(io.StringIO()) as stderr:
                rc = summary.main([
                    str(result_dir),
                    "--repo-root", str(root),
                    "--scripts-dir", str(root / "docs" / "validation" / "daw-bench"),
                    "--include-local-host-availability",
                    "--host-app", "bad-override",
                ])
            self.assertEqual(rc, 2)
            self.assertIn("--host-app must be HOST=", stderr.getvalue())

    def test_invalid_manifest_blocks_summary(self) -> None:
        tmp_ctx, root, result_dir = self._repo()
        with tmp_ctx:
            path = result_dir / "bad.daw-bench.json"
            path.write_text(json.dumps(_manifest(plugin_version="TBD")), encoding="utf-8")
            summaries, results = summary.load_summaries([result_dir], repo_root=root)
            self.assertEqual(summaries, [])
            self.assertTrue(any(not result.ok for result in results))

    def test_require_complete_scripted_lanes_fails_when_backlog_remains(self) -> None:
        tmp_ctx, root, result_dir = self._repo()
        with tmp_ctx:
            path = result_dir / "reaper-vst3.daw-bench.json"
            path.write_text(json.dumps(_manifest()), encoding="utf-8")
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                rc = summary.main([
                    str(result_dir),
                    "--repo-root", str(root),
                    "--scripts-dir", str(root / "docs" / "validation" / "daw-bench"),
                    "--require-any",
                    "--require-complete-scripted-lanes",
                ])
            self.assertEqual(rc, 1)

    def test_require_complete_scripted_lanes_passes_when_all_scripts_have_manifests(self) -> None:
        tmp_ctx, root, result_dir = self._repo()
        with tmp_ctx:
            (result_dir / "reaper-vst3.daw-bench.json").write_text(
                json.dumps(_manifest()),
                encoding="utf-8",
            )
            _write(result_dir / "01-logic-pro-au.md", "# Filled Logic result\n")
            _write(result_dir / "logs" / "LogicPro-AU-20260612T120000Z-pid43.log",
                   "2026-06-12T12:00:00Z\tsession_start\n"
                   "2026-06-12T12:00:00Z\tprepare\n")
            (result_dir / "logic-pro-au.daw-bench.json").write_text(
                json.dumps(_manifest(
                    host="Logic Pro",
                    format="AU",
                    script="01-logic-pro-au.md",
                    result_markdown="01-logic-pro-au.md",
                    logs=["logs/LogicPro-AU-20260612T120000Z-pid43.log"],
                    capabilities=[
                        {
                            "capability": "load",
                            "observed": "Confirmed",
                            "notes": "session_start appeared in the checked-in log.",
                        }
                    ],
                    quirks=[
                        {
                            "flag": "logic_au_tail_time_conversion",
                            "row": "L1",
                            "observed": "Confirmed",
                            "notes": "prepare appeared in the checked-in log.",
                        }
                    ],
                )),
                encoding="utf-8",
            )
            _write(result_dir / "08-aum-auv3.md", "# Filled AUM result\n")
            _write(result_dir / "logs" / "AUM-AUv3-20260612T120000Z-pid44.log",
                   "2026-06-12T12:00:00Z\tsession_start\n")
            (result_dir / "aum-auv3.daw-bench.json").write_text(
                json.dumps(_manifest(
                    host="AUM",
                    format="AUv3",
                    script="08-aum-auv3.md",
                    result_markdown="08-aum-auv3.md",
                    logs=["logs/AUM-AUv3-20260612T120000Z-pid44.log"],
                    capabilities=[
                        {
                            "capability": "load",
                            "observed": "Confirmed",
                            "notes": "session_start appeared in the checked-in log.",
                        }
                    ],
                    quirks=[
                        {
                            "flag": "aum_auv3_load",
                            "row": "A1",
                            "observed": "Confirmed",
                            "notes": "session_start appeared in the checked-in log.",
                        }
                    ],
                )),
                encoding="utf-8",
            )
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                rc = summary.main([
                    str(result_dir),
                    "--repo-root", str(root),
                    "--scripts-dir", str(root / "docs" / "validation" / "daw-bench"),
                    "--require-any",
                    "--require-complete-scripted-lanes",
                ])
            self.assertEqual(rc, 0)


if __name__ == "__main__":
    unittest.main()

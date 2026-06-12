#!/usr/bin/env python3
"""Unit tests for summarize_daw_bench_results.py."""

from __future__ import annotations

import importlib.util
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
               "# REAPER script\n")
        _write(result_dir / "06-reaper-vst3.md",
               "# Filled REAPER result\n")
        _write(result_dir / "logs" / "Reaper-VST3-20260612T120000Z-pid42.log",
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

    def test_markdown_report_includes_run_and_confirmed_quirk_table(self) -> None:
        tmp_ctx, root, result_dir = self._repo()
        with tmp_ctx:
            path = result_dir / "reaper-vst3.daw-bench.json"
            path.write_text(json.dumps(_manifest()), encoding="utf-8")
            summaries, _results = summary.load_summaries([result_dir], repo_root=root)
            markdown = summary.render_markdown(summaries, repo_root=root)
            self.assertIn("- Manifests: 1", markdown)
            self.assertIn("| 2026-06-12 | REAPER | VST3 |", markdown)
            self.assertIn("`reaper_process_while_bypassed`", markdown)
            self.assertIn("docs/validation/daw-bench/results/2026-06-12/reaper-vst3.daw-bench.json", markdown)

    def test_json_report_is_machine_readable(self) -> None:
        tmp_ctx, root, result_dir = self._repo()
        with tmp_ctx:
            path = result_dir / "reaper-vst3.daw-bench.json"
            path.write_text(json.dumps(_manifest()), encoding="utf-8")
            summaries, _results = summary.load_summaries([result_dir], repo_root=root)
            data = json.loads(summary.render_json(summaries, repo_root=root))
            self.assertEqual(data["manifest_count"], 1)
            self.assertEqual(data["host_format_count"], 1)
            self.assertEqual(data["latest_result_date"], "2026-06-12")
            self.assertEqual(data["confirmed_quirk_observations"], 1)
            self.assertEqual(data["runs"][0]["confirmed"], ["reaper_process_while_bypassed"])

    def test_invalid_manifest_blocks_summary(self) -> None:
        tmp_ctx, root, result_dir = self._repo()
        with tmp_ctx:
            path = result_dir / "bad.daw-bench.json"
            path.write_text(json.dumps(_manifest(plugin_version="TBD")), encoding="utf-8")
            summaries, results = summary.load_summaries([result_dir], repo_root=root)
            self.assertEqual(summaries, [])
            self.assertTrue(any(not result.ok for result in results))


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Unit tests for check_daw_bench_evidence.py."""

from __future__ import annotations

import importlib.util
import json
import pathlib
import sys
import tempfile
import unittest


SCRIPT = pathlib.Path(__file__).resolve().parent / "check_daw_bench_evidence.py"
spec = importlib.util.spec_from_file_location("check_daw_bench_evidence", SCRIPT)
assert spec and spec.loader
checker = importlib.util.module_from_spec(spec)
sys.modules["check_daw_bench_evidence"] = checker
spec.loader.exec_module(checker)


def _write(path: pathlib.Path, body: str) -> pathlib.Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(body, encoding="utf-8")
    return path


def _manifest(**overrides: object) -> dict[str, object]:
    data: dict[str, object] = {
        "schema_version": 1,
        "host": "REAPER",
        "format": "VST3",
        "daw_version": "7.16",
        "os": "macOS 15.5",
        "date": "2026-06-12",
        "script": "06-reaper-vst3.md",
        "pulp_commit": "33dc6cfd1f1f",
        "plugin_version": "0.395.0",
        "result_markdown": "06-reaper-vst3.md",
        "logs": ["logs/Reaper-VST3-20260612T120000Z-pid42.log"],
        "quirks": [
            {
                "flag": "reaper_process_while_bypassed",
                "row": "R2",
                "observed": "Confirmed",
                "notes": "process_without_prepare appeared after bypass toggle",
            }
        ],
    }
    data.update(overrides)
    return data


class DawBenchEvidenceTests(unittest.TestCase):
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

    def test_valid_manifest_passes(self) -> None:
        tmp_ctx, root, result_dir = self._repo()
        with tmp_ctx:
            path = result_dir / "reaper-vst3.daw-bench.json"
            path.write_text(json.dumps(_manifest()), encoding="utf-8")
            result = checker.validate_manifest(path, repo_root=root)
            self.assertEqual(result.errors, ())

    def test_rejects_placeholders_and_bad_observed_status(self) -> None:
        tmp_ctx, root, result_dir = self._repo()
        with tmp_ctx:
            manifest = _manifest(
                plugin_version="TBD",
                quirks=[
                    {
                        "flag": "reaper_process_while_bypassed",
                        "row": "<row>",
                        "observed": "Maybe",
                        "notes": "paste here",
                    }
                ],
            )
            path = result_dir / "bad.daw-bench.json"
            path.write_text(json.dumps(manifest), encoding="utf-8")
            errors = checker.validate_manifest(path, repo_root=root).errors
            self.assertIn("plugin_version still contains a placeholder", errors)
            self.assertIn("quirks[0].row must identify the script/catalog row", errors)
            self.assertTrue(any("quirks[0].observed" in error for error in errors))
            self.assertIn("quirks[0].notes must describe the observed evidence", errors)

    def test_requires_log_or_external_url(self) -> None:
        tmp_ctx, root, result_dir = self._repo()
        with tmp_ctx:
            path = result_dir / "no-log.daw-bench.json"
            path.write_text(json.dumps(_manifest(logs=[])), encoding="utf-8")
            errors = checker.validate_manifest(path, repo_root=root).errors
            self.assertIn("provide at least one checked-in log or external_log_url", errors)

    def test_external_url_can_replace_checked_in_logs(self) -> None:
        tmp_ctx, root, result_dir = self._repo()
        with tmp_ctx:
            path = result_dir / "external-log.daw-bench.json"
            path.write_text(
                json.dumps(_manifest(logs=[], external_log_url="https://example.invalid/log")),
                encoding="utf-8",
            )
            result = checker.validate_manifest(path, repo_root=root)
            self.assertEqual(result.errors, ())

    def test_confirmed_known_flag_requires_matching_log_event(self) -> None:
        tmp_ctx, root, result_dir = self._repo()
        with tmp_ctx:
            path = result_dir / "missing-event.daw-bench.json"
            path.write_text(
                json.dumps(_manifest(
                    quirks=[
                        {
                            "flag": "reaper_midsession_setstate",
                            "row": "R6",
                            "observed": "Confirmed",
                            "notes": "State load was claimed but the log lacks the event.",
                        }
                    ]
                )),
                encoding="utf-8",
            )
            errors = checker.validate_manifest(path, repo_root=root).errors
            self.assertTrue(any("deserialize_plugin_state" in error for error in errors))

    def test_not_triggered_known_flag_rejects_matching_log_event(self) -> None:
        tmp_ctx, root, result_dir = self._repo()
        with tmp_ctx:
            path = result_dir / "stale-not-triggered.daw-bench.json"
            path.write_text(
                json.dumps(_manifest(
                    quirks=[
                        {
                            "flag": "reaper_process_while_bypassed",
                            "row": "R1",
                            "observed": "Not Triggered",
                            "notes": "This stale table contradicts the checked-in log.",
                        }
                    ]
                )),
                encoding="utf-8",
            )
            errors = checker.validate_manifest(path, repo_root=root).errors
            self.assertTrue(any("process_without_prepare" in error for error in errors))

    def test_directory_scan_finds_only_manifest_suffix(self) -> None:
        tmp_ctx, _root, result_dir = self._repo()
        with tmp_ctx:
            manifest = result_dir / "reaper-vst3.daw-bench.json"
            manifest.write_text(json.dumps(_manifest()), encoding="utf-8")
            _write(result_dir / "ignored.json", "{}")
            self.assertEqual(checker.find_manifests([result_dir]), [manifest])

    def test_default_empty_scan_is_advisory(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            empty = pathlib.Path(d)
            self.assertEqual(checker.find_manifests([empty]), [])
            self.assertIn(
                "no manifests found",
                checker.render_results([], scanned=1),
            )

    def test_invalid_json_reports_line(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            path = pathlib.Path(d) / "broken.daw-bench.json"
            path.write_text("{\n", encoding="utf-8")
            errors = checker.validate_manifest(path, repo_root=pathlib.Path(d)).errors
            self.assertTrue(any("invalid JSON" in error for error in errors))


if __name__ == "__main__":
    unittest.main()

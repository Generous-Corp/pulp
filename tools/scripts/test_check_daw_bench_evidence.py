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
        "plugin_version": "1.0.0",
        "result_markdown": "06-reaper-vst3.md",
        "logs": ["logs/Reaper-VST3-20260612T120000Z-pid42.log"],
        "capabilities": [
            {
                "capability": "load",
                "observed": "Confirmed",
                "notes": "session_start appeared in the checked-in log.",
            }
        ],
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
               "2026-06-12T12:00:00Z\tsession_start\tpulp_bench_plugin=1.0.0\n"
               "2026-06-12T12:00:00Z\tprocessor_construct\tplugin_version=1.0.0\n"
               "2026-06-12T12:00:00Z\tserialize_plugin_state\n"
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

    def test_plugin_version_must_match_checked_in_logs(self) -> None:
        tmp_ctx, root, result_dir = self._repo()
        with tmp_ctx:
            path = result_dir / "stale-plugin-version.daw-bench.json"
            path.write_text(json.dumps(_manifest(plugin_version="2.0.0")), encoding="utf-8")
            errors = checker.validate_manifest(path, repo_root=root).errors
            self.assertIn(
                "plugin_version 2.0.0 must match all checked-in log version(s): 1.0.0",
                errors,
            )

    def test_plugin_version_rejects_mixed_checked_in_log_versions(self) -> None:
        tmp_ctx, root, result_dir = self._repo()
        with tmp_ctx:
            _write(
                result_dir / "logs" / "Reaper-VST3-extra.log",
                "2026-06-12T12:00:01Z\tprocessor_construct\tplugin_version=2.0.0\n",
            )
            path = result_dir / "mixed-plugin-version.daw-bench.json"
            path.write_text(
                json.dumps(_manifest(logs=[
                    "logs/Reaper-VST3-20260612T120000Z-pid42.log",
                    "logs/Reaper-VST3-extra.log",
                ])),
                encoding="utf-8",
            )
            errors = checker.validate_manifest(path, repo_root=root).errors
            self.assertIn(
                "plugin_version 1.0.0 must match all checked-in log version(s): 1.0.0, 2.0.0",
                errors,
            )

    def test_preflight_report_is_optional_diagnostic_context(self) -> None:
        tmp_ctx, root, result_dir = self._repo()
        with tmp_ctx:
            _write(
                result_dir / "preflight" / "logic-au.json",
                json.dumps(
                    {
                        "ok": False,
                        "checks": [
                            {
                                "label": "auval -v aumf PHBn Pulp",
                                "ok": False,
                                "detail": "Cannot get Component's Name strings",
                            }
                        ],
                    }
                ),
            )
            path = result_dir / "with-preflight.daw-bench.json"
            path.write_text(
                json.dumps(_manifest(preflight_reports=["preflight/logic-au.json"])),
                encoding="utf-8",
            )
            result = checker.validate_manifest(path, repo_root=root)
            self.assertEqual(result.errors, ())

    def test_preflight_report_path_must_exist(self) -> None:
        tmp_ctx, root, result_dir = self._repo()
        with tmp_ctx:
            path = result_dir / "missing-preflight.daw-bench.json"
            path.write_text(
                json.dumps(_manifest(preflight_reports=["preflight/missing.json"])),
                encoding="utf-8",
            )
            errors = checker.validate_manifest(path, repo_root=root).errors
            self.assertIn(
                "preflight_reports[0] must reference a checked-in preflight JSON file",
                errors,
            )

    def test_preflight_report_shape_is_validated(self) -> None:
        tmp_ctx, root, result_dir = self._repo()
        with tmp_ctx:
            _write(
                result_dir / "preflight" / "broken.json",
                json.dumps({"ok": "yes", "checks": [{"label": "", "ok": "no"}]}),
            )
            path = result_dir / "bad-preflight.daw-bench.json"
            path.write_text(
                json.dumps(_manifest(preflight_reports=["preflight/broken.json"])),
                encoding="utf-8",
            )
            errors = checker.validate_manifest(path, repo_root=root).errors
            self.assertIn("preflight_reports[0].ok must be a boolean", errors)
            self.assertIn(
                "preflight_reports[0].checks[0].label must be a non-empty string",
                errors,
            )
            self.assertIn("preflight_reports[0].checks[0].ok must be a boolean", errors)
            self.assertIn("preflight_reports[0].checks[0].detail must be a string", errors)

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

    def test_reaper_clap_transport_edges_requires_playing_edge(self) -> None:
        tmp_ctx, root, result_dir = self._repo()
        with tmp_ctx:
            _write(
                result_dir / "logs" / "Reaper-CLAP-20260612T120000Z-pid43.log",
                "2026-06-12T12:00:00Z\tsession_start\n"
                "2026-06-12T12:00:00Z\tprocess_is_playing_edge\tis_playing=true\n",
            )
            _write(root / "docs" / "validation" / "daw-bench" / "07-reaper-clap.md",
                   "# REAPER CLAP script\n")
            _write(result_dir / "07-reaper-clap.md",
                   "# Filled REAPER CLAP result\n")
            path = result_dir / "reaper-clap.daw-bench.json"
            path.write_text(
                json.dumps(_manifest(
                    format="CLAP",
                    script="07-reaper-clap.md",
                    result_markdown="07-reaper-clap.md",
                    logs=["logs/Reaper-CLAP-20260612T120000Z-pid43.log"],
                    quirks=[
                        {
                            "flag": "reaper_clap_transport_edges",
                            "row": "R7",
                            "observed": "Confirmed",
                            "notes": "Transport edge was observed in REAPER CLAP.",
                        }
                    ],
                )),
                encoding="utf-8",
            )
            self.assertEqual(checker.validate_manifest(path, repo_root=root).errors, ())

    def test_confirmed_capability_requires_matching_log_event(self) -> None:
        tmp_ctx, root, result_dir = self._repo()
        with tmp_ctx:
            path = result_dir / "bad-capability.daw-bench.json"
            path.write_text(
                json.dumps(_manifest(
                    capabilities=[
                        {
                            "capability": "midi",
                            "observed": "Confirmed",
                            "notes": "MIDI was claimed but no midi_in event exists.",
                        }
                    ]
                )),
                encoding="utf-8",
            )
            errors = checker.validate_manifest(path, repo_root=root).errors
            self.assertTrue(any("midi is Confirmed" in error for error in errors))

    def test_params_capability_requires_definition_and_state_events(self) -> None:
        tmp_ctx, root, result_dir = self._repo()
        with tmp_ctx:
            _write(
                result_dir / "logs" / "Reaper-VST3-20260612T120000Z-pid42.log",
                "2026-06-12T12:00:00Z\tsession_start\n"
                "2026-06-12T12:00:00Z\tdefine_parameters\n"
                "2026-06-12T12:00:00Z\tprocess_without_prepare\n",
            )
            path = result_dir / "params-missing-state.daw-bench.json"
            path.write_text(
                json.dumps(_manifest(
                    capabilities=[
                        {
                            "capability": "params",
                            "observed": "Confirmed",
                            "notes": "Parameter list alone is not enough for params coverage.",
                        }
                    ]
                )),
                encoding="utf-8",
            )
            errors = checker.validate_manifest(path, repo_root=root).errors
            self.assertTrue(any("serialize_plugin_state" in error for error in errors))

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

#!/usr/bin/env python3
"""Unit tests for check_host_matrix_evidence.py."""

from __future__ import annotations

import importlib.util
import json
import pathlib
import sys
import tempfile
import textwrap
import unittest


SCRIPT_DIR = pathlib.Path(__file__).resolve().parent

checker_spec = importlib.util.spec_from_file_location(
    "check_daw_bench_evidence",
    SCRIPT_DIR / "check_daw_bench_evidence.py",
)
assert checker_spec and checker_spec.loader
daw_checker = importlib.util.module_from_spec(checker_spec)
sys.modules["check_daw_bench_evidence"] = daw_checker
checker_spec.loader.exec_module(daw_checker)

matrix_spec = importlib.util.spec_from_file_location(
    "check_host_matrix_evidence",
    SCRIPT_DIR / "check_host_matrix_evidence.py",
)
assert matrix_spec and matrix_spec.loader
matrix_checker = importlib.util.module_from_spec(matrix_spec)
sys.modules["check_host_matrix_evidence"] = matrix_checker
matrix_spec.loader.exec_module(matrix_checker)


def _write(path: pathlib.Path, body: str) -> pathlib.Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(body, encoding="utf-8")
    return path


def _manifest(*, host: str = "REAPER", fmt: str = "VST3") -> dict[str, object]:
    return {
        "schema_version": 1,
        "host": host,
        "format": fmt,
        "daw_version": "7.74/macOS-arm64",
        "os": "macOS 26.5.1 arm64",
        "date": "2026-06-12",
        "script": "06-reaper-vst3.md",
        "pulp_commit": "c549bed15",
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
            }
        ],
    }


class HostMatrixEvidenceTests(unittest.TestCase):
    def _repo(self) -> tuple[tempfile.TemporaryDirectory[str], pathlib.Path, pathlib.Path]:
        tmp_ctx = tempfile.TemporaryDirectory()
        root = pathlib.Path(tmp_ctx.name)
        result_dir = root / "docs" / "validation" / "daw-bench" / "results" / "2026-06-12"
        _write(root / "docs" / "validation" / "daw-bench" / "06-reaper-vst3.md",
               "# REAPER script\n")
        _write(result_dir / "06-reaper-vst3.md",
               "# Filled REAPER result\n")
        _write(result_dir / "logs" / "Reaper-VST3-20260612T120000Z-pid42.log",
               "2026-06-12T12:00:00Z\tsession_start\n"
               "2026-06-12T12:00:00Z\tdefine_parameters\n"
               "2026-06-12T12:00:00Z\tserialize_plugin_state\n"
               "2026-06-12T12:00:00Z\tprocess_without_prepare\n")
        (result_dir / "reaper-vst3.daw-bench.json").write_text(
            json.dumps(_manifest()),
            encoding="utf-8",
        )
        matrix = root / "docs" / "guides" / "host-matrix.md"
        return tmp_ctx, root, matrix

    def test_promoted_row_with_matching_manifest_passes(self) -> None:
        tmp_ctx, root, matrix = self._repo()
        with tmp_ctx:
            _write(matrix, textwrap.dedent("""\
                # Host Compatibility Matrix

                ## VST3

                | Host | Version | Load | Params | Notes |
                |------|---------|------|--------|-------|
                | Reaper | 7.74 | 🟡 | 🟡 | VST3 bench evidence: `docs/validation/daw-bench/results/2026-06-12/` |
                | Live | 12 | — | — | |
                """))
            check = matrix_checker.validate_matrix(matrix, repo_root=root)
            self.assertEqual(check.errors, ())

    def test_promoted_row_requires_evidence_citation(self) -> None:
        tmp_ctx, root, matrix = self._repo()
        with tmp_ctx:
            _write(matrix, textwrap.dedent("""\
                # Host Compatibility Matrix

                ## VST3

                | Host | Version | Load | Params | Notes |
                |------|---------|------|--------|-------|
                | Reaper | 7.74 | 🟡 | — | No durable evidence yet |
                """))
            errors = matrix_checker.validate_matrix(matrix, repo_root=root).errors
            self.assertTrue(any("no DAW-bench result folder" in error for error in errors))

    def test_cited_manifest_must_match_host_and_format(self) -> None:
        tmp_ctx, root, matrix = self._repo()
        with tmp_ctx:
            result_dir = root / "docs" / "validation" / "daw-bench" / "results" / "2026-06-12"
            (result_dir / "reaper-vst3.daw-bench.json").write_text(
                json.dumps(_manifest(host="Bitwig", fmt="CLAP")),
                encoding="utf-8",
            )
            _write(matrix, textwrap.dedent("""\
                # Host Compatibility Matrix

                ## VST3

                | Host | Version | Load | Params | Notes |
                |------|---------|------|--------|-------|
                | Reaper | 7.74 | 🟡 | — | VST3 bench evidence: `docs/validation/daw-bench/results/2026-06-12/` |
                """))
            errors = matrix_checker.validate_matrix(matrix, repo_root=root).errors
            self.assertTrue(any("no valid DAW-bench manifest matches VST3/Reaper" in error for error in errors))

    def test_promoted_cell_requires_matching_capability_evidence(self) -> None:
        tmp_ctx, root, matrix = self._repo()
        with tmp_ctx:
            _write(matrix, textwrap.dedent("""\
                # Host Compatibility Matrix

                ## VST3

                | Host | Version | Load | Params | MIDI | Notes |
                |------|---------|------|--------|------|-------|
                | Reaper | 7.74 | 🟡 | 🟡 | 🟡 | VST3 bench evidence: `docs/validation/daw-bench/results/2026-06-12/` |
                """))
            errors = matrix_checker.validate_matrix(matrix, repo_root=root).errors
            self.assertTrue(any("capability MIDI" in error for error in errors))

    def test_broken_promoted_cell_accepts_refuted_capability_evidence(self) -> None:
        tmp_ctx, root, matrix = self._repo()
        with tmp_ctx:
            result_dir = root / "docs" / "validation" / "daw-bench" / "results" / "2026-06-12"
            manifest = _manifest()
            manifest["capabilities"] = [
                {
                    "capability": "midi",
                    "observed": "Refuted",
                    "notes": "MIDI was exercised but did not reach the plugin.",
                }
            ]
            (result_dir / "reaper-vst3.daw-bench.json").write_text(
                json.dumps(manifest),
                encoding="utf-8",
            )
            _write(matrix, textwrap.dedent("""\
                # Host Compatibility Matrix

                ## VST3

                | Host | Version | MIDI | Notes |
                |------|---------|------|-------|
                | Reaper | 7.74 | 🔴 | VST3 bench evidence: `docs/validation/daw-bench/results/2026-06-12/` |
                """))
            check = matrix_checker.validate_matrix(matrix, repo_root=root)
            self.assertEqual(check.errors, ())


if __name__ == "__main__":
    unittest.main()

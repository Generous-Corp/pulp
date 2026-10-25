#!/usr/bin/env python3
"""Tests for windows_runner_selector.py, the dispatch-input guard that keeps a
Windows job off the macOS gate pool and the Linux pool.

Run:  python3 tools/scripts/test_windows_runner_selector.py
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = HERE / "windows_runner_selector.py"
sys.path.insert(0, str(HERE))

import windows_runner_selector as selector  # noqa: E402

MACPRO_WINDOWS = ["self-hosted", "Windows", "X64", "pulp-build-windows-x64", "pulp-host-macpro"]


class ResolveTests(unittest.TestCase):
    def test_empty_selector_keeps_the_hosted_fallback(self) -> None:
        self.assertEqual(selector.resolve(""), '"windows-latest"')
        self.assertEqual(selector.resolve("   "), '"windows-latest"')

    def test_macpro_windows_slot_is_accepted_verbatim(self) -> None:
        self.assertEqual(json.loads(selector.resolve(json.dumps(MACPRO_WINDOWS))), MACPRO_WINDOWS)

    def test_hosted_windows_image_is_accepted(self) -> None:
        self.assertEqual(selector.resolve('"windows-2022"'), '"windows-2022"')

    def test_macos_gate_selectors_are_refused(self) -> None:
        for raw in (
            '["self-hosted","macOS","ARM64","pulp-build-vm","pulp-gate-fast"]',
            '["self-hosted","Windows","pulp-gate-fast"]',
            '["self-hosted","Windows","pulp-build-vm"]',
            '"macos-15"',
            '["self-hosted","Windows","OSX"]',
            '["self-hosted","Windows","darwin"]',
        ):
            with self.subTest(raw=raw), self.assertRaises(selector.SelectorError):
                selector.resolve(raw)

    def test_linux_selectors_are_refused(self) -> None:
        for raw in (
            '["self-hosted","Linux","X64","pulp-build-linux-x64","pulp-host-macpro"]',
            '["self-hosted","Windows","X64","pulp-build-linux-x64"]',
            '["self-hosted","Linux","X64","pulp-preamble-macpro"]',
            '"ubuntu-latest"',
        ):
            with self.subTest(raw=raw), self.assertRaises(selector.SelectorError):
                selector.resolve(raw)

    def test_a_list_must_name_windows_and_self_hosted(self) -> None:
        for raw in ('["self-hosted","X64"]', '["Windows","X64"]', "[]"):
            with self.subTest(raw=raw), self.assertRaises(selector.SelectorError):
                selector.resolve(raw)

    def test_malformed_input_is_refused(self) -> None:
        for raw in ("not json", '{"labels":["Windows"]}', '["self-hosted","Windows","a b"]',
                    '["self-hosted","Windows","Windows"]', '"self-hosted"', "[1]"):
            with self.subTest(raw=raw), self.assertRaises(selector.SelectorError):
                selector.resolve(raw)


class CliTests(unittest.TestCase):
    def run_cli(self, *args: str, env: dict[str, str] | None = None) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, str(SCRIPT), *args],
            capture_output=True, text=True, encoding="utf-8",
            env={**os.environ, **(env or {})},
        )

    def test_github_output_receives_the_resolved_value(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp) / "out"
            result = self.run_cli("--selector", json.dumps(MACPRO_WINDOWS), "--github-output",
                                  env={"GITHUB_OUTPUT": str(out)})
            self.assertEqual(result.returncode, 0, result.stderr)
            line = out.read_text(encoding="utf-8").strip()
            self.assertEqual(json.loads(line.split("=", 1)[1]), MACPRO_WINDOWS)

    def test_refusal_exits_2_and_names_the_label(self) -> None:
        result = self.run_cli("--selector", '["self-hosted","macOS","pulp-gate-fast"]')
        self.assertEqual(result.returncode, 2)
        self.assertIn("refused", result.stderr)
        self.assertEqual(result.stdout, "")


if __name__ == "__main__":
    unittest.main()

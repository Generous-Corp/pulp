#!/usr/bin/env python3
"""Regression tests for rendered-panel gate orchestration."""

from __future__ import annotations

import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "tools" / "import-validation" / "verify_rendered_panel.py"
SPEC = importlib.util.spec_from_file_location("verify_rendered_panel", SOURCE)
assert SPEC and SPEC.loader
MOD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MOD)


class RenderedPanelPreflightTests(unittest.TestCase):
    def test_complete_binary_string_satisfies_capability_check(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            artifact = root / "ui.js"
            binary = root / "pulp-screenshot"
            artifact.write_text("setColorToken('accent', '#fff');\n")
            binary.write_bytes(b"prefix\0setColorToken\0suffix")

            MOD.assert_renderer_can_execute(binary, artifact)

    def test_capability_name_must_be_a_complete_binary_string(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            artifact = root / "ui.js"
            binary = root / "pulp-screenshot"
            artifact.write_text("setColorToken('accent', '#fff');\n")
            binary.write_bytes(b"prefix\0setColorTokenLegacy\0suffix")

            with self.assertRaisesRegex(SystemExit, str(MOD.EX_HARNESS)):
                MOD.assert_renderer_can_execute(binary, artifact)

    def test_capability_inspection_failure_is_a_harness_error(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            artifact = root / "ui.js"
            artifact.write_text("setColorToken('accent', '#fff');\n")

            with self.assertRaisesRegex(SystemExit, str(MOD.EX_HARNESS)):
                MOD.assert_renderer_can_execute(root / "missing-renderer", artifact)

    def test_renderer_capability_is_checked_before_rendering(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            artifact = root / "ui.js"
            reference = root / "reference.png"
            binary = root / "pulp-screenshot"
            artifact.write_text("setColorToken('accent', '#fff');\n")
            reference.write_bytes(b"reference")
            binary.write_bytes(b"binary")

            calls: list[str] = []

            def stop_at_preflight(*_args: object) -> None:
                calls.append("preflight")
                raise SystemExit(MOD.EX_HARNESS)

            def unexpected_render(*_args: object) -> None:
                calls.append("render")

            argv = [
                str(SOURCE),
                "--artifact", str(artifact),
                "--reference", str(reference),
                "--width", "100",
                "--height", "80",
                "--screenshot-bin", str(binary),
            ]
            with mock.patch.object(sys, "argv", argv), \
                    mock.patch.object(MOD, "png_size", return_value=(100, 80)), \
                    mock.patch.object(MOD, "assert_renderer_can_execute",
                                      side_effect=stop_at_preflight), \
                    mock.patch.object(MOD, "render", side_effect=unexpected_render):
                with self.assertRaisesRegex(SystemExit, str(MOD.EX_HARNESS)):
                    MOD.main()

            self.assertEqual(calls, ["preflight"])


if __name__ == "__main__":
    unittest.main(verbosity=2)

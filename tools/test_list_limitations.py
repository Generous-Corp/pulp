#!/usr/bin/env python3
"""Unit tests for tools/list_limitations.py."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import pathlib
import sys
import tempfile
import unittest
from unittest import mock


SCRIPT = pathlib.Path(__file__).parent / "list_limitations.py"
spec = importlib.util.spec_from_file_location("list_limitations", SCRIPT)
assert spec and spec.loader
ll = importlib.util.module_from_spec(spec)
sys.modules["list_limitations"] = ll
spec.loader.exec_module(ll)


MATRIX = """
runtime:
  audio:
    hosting:
      status: partial
  midi:
    status: stable

limitations:
  runtime.audio.hosting:
    - text: "AUv3 sidechain discovery is incomplete."
      tracked_in: "https://example.test/issues/123"
    - text: "LV2 scanning is experimental."
  runtime.midi:
    - text: "UMP profile negotiation is simulated."
      tracked_in: ""
"""


class ExtractLimitationsTests(unittest.TestCase):
    def test_extract_limitations_parses_items_and_optional_tracking(self) -> None:
        entries = ll.extract_limitations(MATRIX)

        self.assertEqual(
            entries,
            [
                (
                    "runtime.audio.hosting",
                    [
                        {
                            "text": "AUv3 sidechain discovery is incomplete.",
                            "tracked_in": "https://example.test/issues/123",
                        },
                        {
                            "text": "LV2 scanning is experimental.",
                            "tracked_in": "",
                        },
                    ],
                ),
                (
                    "runtime.midi",
                    [
                        {
                            "text": "UMP profile negotiation is simulated.",
                            "tracked_in": "",
                        },
                    ],
                ),
            ],
        )

    def test_extract_limitations_returns_empty_when_block_missing(self) -> None:
        self.assertEqual(ll.extract_limitations("runtime:\n  midi:\n"), [])


class CapabilityPathTests(unittest.TestCase):
    def test_capability_exists_walks_nested_yaml_keys(self) -> None:
        self.assertTrue(ll.capability_exists(MATRIX, "runtime.audio.hosting"))
        self.assertTrue(ll.capability_exists(MATRIX, "runtime.midi"))

    def test_capability_exists_rejects_missing_or_misordered_paths(self) -> None:
        self.assertFalse(ll.capability_exists(MATRIX, "runtime.audio.render"))
        self.assertFalse(ll.capability_exists(MATRIX, "audio.runtime.hosting"))


class MainTests(unittest.TestCase):
    def test_main_renders_markdown_table(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            matrix = pathlib.Path(td) / "support-matrix.yaml"
            matrix.write_text(MATRIX, encoding="utf-8")

            out = io.StringIO()
            with mock.patch.object(ll, "MATRIX_PATH", matrix):
                with contextlib.redirect_stdout(out):
                    rc = ll.main()

        self.assertEqual(rc, 0)
        rendered = out.getvalue()
        self.assertIn("Known limitations (3 items across 2 capabilities)", rendered)
        self.assertIn("| `runtime.audio.hosting` | AUv3 sidechain discovery is incomplete. | [link](https://example.test/issues/123) |", rendered)
        self.assertIn("| `runtime.audio.hosting` | LV2 scanning is experimental. | — |", rendered)

    def test_main_reports_unresolved_paths(self) -> None:
        bad_matrix = MATRIX + "\n  runtime.missing:\n    - text: \"Missing.\"\n"
        with tempfile.TemporaryDirectory() as td:
            matrix = pathlib.Path(td) / "support-matrix.yaml"
            matrix.write_text(bad_matrix, encoding="utf-8")

            err = io.StringIO()
            with mock.patch.object(ll, "MATRIX_PATH", matrix):
                with contextlib.redirect_stderr(err):
                    rc = ll.main()

        self.assertEqual(rc, 1)
        self.assertIn("runtime.missing", err.getvalue())


if __name__ == "__main__":
    unittest.main(verbosity=2)

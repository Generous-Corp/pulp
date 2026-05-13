#!/usr/bin/env python3
"""Unit tests for validation_profile_select.

Run with:
    python3 tools/scripts/test_validation_profile_select.py
"""

from __future__ import annotations

import io
import json
import sys
import unittest
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

import validation_profile_select as vps  # noqa: E402


class PathMatchingTests(unittest.TestCase):
    def test_import_design_tool_paths_match(self):
        self.assertTrue(vps.is_parser_only_path("tools/import-design/pulp_import_design.cpp"))
        self.assertTrue(vps.is_parser_only_path("tools/import-design/catalogs/yoga.tsv"))

    def test_import_validation_scripts_match(self):
        self.assertTrue(vps.is_parser_only_path("tools/import-validation/spectr-roundtrip.sh"))
        self.assertTrue(vps.is_parser_only_path("tools/import-validation/check_label_coverage.sh"))

    def test_import_ir_package_matches(self):
        self.assertTrue(vps.is_parser_only_path("packages/pulp-import-ir/src/diff.ts"))
        self.assertTrue(vps.is_parser_only_path("packages/pulp-import-ir/test/anchors.test.ts"))

    def test_parser_test_files_match(self):
        self.assertTrue(vps.is_parser_only_path("test/test_design_import.cpp"))
        self.assertTrue(vps.is_parser_only_path("test/test_design_import_claude_bundle.cpp"))
        self.assertTrue(vps.is_parser_only_path("test/test_cli_import_design.cpp"))
        self.assertTrue(vps.is_parser_only_path("test/test_widget_promotion.cpp"))

    def test_core_view_design_import_paths_match(self):
        self.assertTrue(vps.is_parser_only_path("core/view/src/design_import.cpp"))
        self.assertTrue(vps.is_parser_only_path("core/view/include/pulp/view/design_import.hpp"))
        self.assertTrue(vps.is_parser_only_path("core/view/js/import-runtime.js"))

    def test_unrelated_paths_do_not_match(self):
        self.assertFalse(vps.is_parser_only_path("core/format/src/vst3_adapter.cpp"))
        self.assertFalse(vps.is_parser_only_path("core/audio/src/buffer_view.cpp"))
        self.assertFalse(vps.is_parser_only_path("examples/pulp-gain/CMakeLists.txt"))
        self.assertFalse(vps.is_parser_only_path(".github/workflows/build.yml"))
        # CMakeLists at repo root is broad — always falls back to default.
        self.assertFalse(vps.is_parser_only_path("CMakeLists.txt"))

    def test_unrelated_core_view_paths_do_not_match(self):
        # The parser-only scope covers `design_import*` and the import
        # runtime JS — generic view code is broader.
        self.assertFalse(vps.is_parser_only_path("core/view/src/widget.cpp"))
        self.assertFalse(vps.is_parser_only_path("core/view/include/pulp/view/widget.hpp"))


class ClassifyTests(unittest.TestCase):
    def test_all_parser_only_returns_parser(self):
        result = vps.classify([
            "tools/import-design/pulp_import_design.cpp",
            "test/test_design_import.cpp",
            "test/fixtures/imports/stitch/example.html",
        ])
        self.assertEqual(result.profile, "parser")
        self.assertEqual(len(result.unmatched), 0)

    def test_any_unrelated_path_returns_default(self):
        result = vps.classify([
            "tools/import-design/pulp_import_design.cpp",
            "core/format/src/vst3_adapter.cpp",
        ])
        self.assertEqual(result.profile, "default")
        self.assertEqual(len(result.unmatched), 1)
        self.assertIn("core/format/src/vst3_adapter.cpp", result.unmatched)

    def test_empty_diff_defaults_to_default(self):
        result = vps.classify([])
        self.assertEqual(result.profile, "default")

    def test_whitespace_lines_are_ignored(self):
        result = vps.classify([
            "  ",
            "tools/import-design/pulp_import_design.cpp",
            "",
        ])
        self.assertEqual(result.profile, "parser")
        self.assertEqual(result.matched, ("tools/import-design/pulp_import_design.cpp",))


class CliTests(unittest.TestCase):
    def test_paths_from_stdin_plain_output(self):
        stdin = io.StringIO("tools/import-design/foo.cpp\ntest/test_design_import.cpp\n")
        buf = io.StringIO()
        old_stdin, sys.stdin = sys.stdin, stdin
        try:
            with redirect_stdout(buf):
                rc = vps.main(["--paths-from", "-"])
        finally:
            sys.stdin = old_stdin
        self.assertEqual(rc, 0)
        self.assertEqual(buf.getvalue().strip(), "parser")

    def test_paths_from_stdin_json_output(self):
        stdin = io.StringIO("core/format/src/vst3_adapter.cpp\n")
        buf = io.StringIO()
        old_stdin, sys.stdin = sys.stdin, stdin
        try:
            with redirect_stdout(buf):
                rc = vps.main(["--paths-from", "-", "--json"])
        finally:
            sys.stdin = old_stdin
        self.assertEqual(rc, 0)
        payload = json.loads(buf.getvalue())
        self.assertEqual(payload["profile"], "default")
        self.assertEqual(payload["unmatched"], ["core/format/src/vst3_adapter.cpp"])
        self.assertEqual(payload["matched"], [])


if __name__ == "__main__":
    unittest.main()

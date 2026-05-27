#!/usr/bin/env python3
"""Unit tests for verify_create_templates.py.

Run directly:
    python3 tools/scripts/test_verify_create_templates.py
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

# Allow running this file standalone without packaging.
_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE))
import verify_create_templates as vct  # noqa: E402


class TestScanTemplateVars(unittest.TestCase):
    def test_finds_simple_var(self) -> None:
        self.assertEqual(vct.scan_template_vars("Hello {{NAME}}"), {"NAME"})

    def test_finds_multiple_vars(self) -> None:
        text = "{{PLUGIN_NAME}} by {{MANUFACTURER}} ({{VERSION}})"
        self.assertEqual(
            vct.scan_template_vars(text),
            {"PLUGIN_NAME", "MANUFACTURER", "VERSION"},
        )

    def test_ignores_single_braces(self) -> None:
        self.assertEqual(vct.scan_template_vars("not a {VAR} ref"), set())

    def test_ignores_lowercase(self) -> None:
        # Substitution is uppercase-only by convention.
        self.assertEqual(vct.scan_template_vars("{{plugin_name}}"), set())

    def test_allows_digits_after_first_char(self) -> None:
        self.assertEqual(vct.scan_template_vars("{{VST3_UID}}"), {"VST3_UID"})


class TestCheckTemplateDir(unittest.TestCase):
    def _write(self, root: Path, name: str, content: str) -> None:
        (root / name).write_text(content, encoding="utf-8")

    def test_passes_when_all_required_files_and_known_vars(self) -> None:
        with TemporaryDirectory() as td:
            root = Path(td)
            for name in vct.REQUIRED_FILES_BY_TYPE["app"]:
                self._write(root, name, "// {{PLUGIN_NAME}} {{VERSION}}\n")
            failures = vct.check_template_dir("app", root)
            self.assertEqual(failures, [])

    def test_flags_missing_required_file(self) -> None:
        with TemporaryDirectory() as td:
            root = Path(td)
            self._write(
                root, "CMakeLists.txt.template",
                "# {{PLUGIN_NAME}}\n",
            )
            # Missing processor.hpp.template and test.cpp.template.
            failures = vct.check_template_dir("app", root)
            self.assertEqual(len(failures), 1)
            self.assertIn("missing required template files", failures[0])
            self.assertIn("processor.hpp.template", failures[0])
            self.assertIn("test.cpp.template", failures[0])

    def test_flags_unknown_template_var(self) -> None:
        with TemporaryDirectory() as td:
            root = Path(td)
            for name in vct.REQUIRED_FILES_BY_TYPE["bare"]:
                self._write(root, name, "// {{PLUGIN_NAME}}\n")
            self._write(
                root, "extra.cpp.template",
                "// {{TOTALLY_NEW_THING}}\n",
            )
            failures = vct.check_template_dir("bare", root)
            self.assertEqual(len(failures), 1)
            self.assertIn("TOTALLY_NEW_THING", failures[0])
            self.assertIn("unknown template variable", failures[0])

    def test_skips_required_check_for_unmapped_type(self) -> None:
        # `android` is intentionally NOT in REQUIRED_FILES_BY_TYPE — it
        # is a partial template tree. Vars are still validated, but the
        # required-files check is skipped.
        with TemporaryDirectory() as td:
            root = Path(td)
            self._write(
                root, "one_file.template",
                "// {{PLUGIN_NAME}}\n",
            )
            failures = vct.check_template_dir("android", root)
            self.assertEqual(failures, [])

    def test_flags_unknown_var_in_nested_template(self) -> None:
        # Regression: Codex PR #3002 review (P1 + P2). The previous
        # implementation called `glob("*.template")` and missed nested
        # template trees like `tools/templates/android/app/src/main/...`
        # and `tools/templates/standalone/<type>/CMakeLists.txt.template`.
        # A bad placeholder in those files would silently ship to users.
        # This test plants a nested template with an unknown var and
        # asserts the checker now catches it.
        with TemporaryDirectory() as td:
            root = Path(td)
            nested_dir = root / "app" / "src" / "main"
            nested_dir.mkdir(parents=True)
            (nested_dir / "AndroidManifest.xml.template").write_text(
                "<!-- {{TOTALLY_NEW_NESTED_PLACEHOLDER}} -->\n",
                encoding="utf-8",
            )
            failures = vct.check_template_dir("android", root)
            self.assertEqual(len(failures), 1)
            self.assertIn("TOTALLY_NEW_NESTED_PLACEHOLDER", failures[0])
            self.assertIn("unknown template variable", failures[0])
            # The diagnostic path should be relative to the type-dir so
            # the user can locate the broken file.
            self.assertIn("app/src/main/AndroidManifest.xml.template",
                          failures[0])

    def test_discovers_subtree_without_top_level_templates(self) -> None:
        # Regression: Codex PR #3002 P1. `tools/templates/standalone/`
        # contains no top-level *.template files (only nested subdirs).
        # The previous iterdir filter (`any(d.glob("*.template"))`)
        # excluded it entirely, leaving the standalone subtree unchecked.
        with TemporaryDirectory() as td:
            root = Path(td)
            # Build a fake `standalone/` shaped like the real one.
            standalone = root / "standalone"
            (standalone / "app").mkdir(parents=True)
            (standalone / "app" / "CMakeLists.txt.template").write_text(
                "# {{UNKNOWN_STANDALONE_PLACEHOLDER}}\n",
                encoding="utf-8",
            )
            rc = vct.main(["--templates-root", str(root)])
            self.assertEqual(rc, 1)


class TestMain(unittest.TestCase):
    def test_pass_run_on_real_pulp_templates(self) -> None:
        # The committed template tree must pass at HEAD; that is the
        # whole point of the smoke. If this fails, either:
        #   a) a template introduced a `{{VAR}}` not in KNOWN_TEMPLATE_VARS,
        #   b) a template directory lost a required file, or
        #   c) the substitution map in tools/cli/cmd_create.cpp changed
        #      and KNOWN_TEMPLATE_VARS here is stale.
        templates_root = _HERE.parent / "templates"
        if not templates_root.is_dir():
            self.skipTest(
                f"tools/templates not present at {templates_root} "
                "(running outside Pulp checkout)"
            )
        rc = vct.main(["--templates-root", str(templates_root)])
        self.assertEqual(rc, 0, "verify_create_templates must pass at HEAD")

    def test_fails_on_missing_root(self) -> None:
        rc = vct.main(["--templates-root", "/nonexistent/path/xyz"])
        self.assertEqual(rc, 1)

    def test_fails_on_empty_root(self) -> None:
        with TemporaryDirectory() as td:
            rc = vct.main(["--templates-root", td])
            self.assertEqual(rc, 1)


if __name__ == "__main__":
    unittest.main()

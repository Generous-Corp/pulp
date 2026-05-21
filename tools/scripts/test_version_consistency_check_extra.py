#!/usr/bin/env python3
"""Direct in-process coverage for version_consistency_check.py."""

from __future__ import annotations

import importlib.util
import io
import json
import pathlib
import runpy
import shutil
import tempfile
import textwrap
import unittest
from contextlib import redirect_stdout


MODULE_PATH = pathlib.Path(__file__).with_name("version_consistency_check.py")


def load_module():
    spec = importlib.util.spec_from_file_location("version_consistency_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def write_layout(
    root: pathlib.Path,
    *,
    cmake: str = "1.2.3",
    plugin: str = "0.5.0",
    market_top: str = "0.5.0",
    market_nested: str = "0.5.0",
    changelog: str = "1.2.3",
) -> None:
    (root / "CMakeLists.txt").write_text(f"project(Pulp VERSION {cmake})\n", encoding="utf-8")
    (root / ".claude-plugin").mkdir()
    (root / ".claude-plugin" / "plugin.json").write_text(
        json.dumps({"version": plugin}),
        encoding="utf-8",
    )
    (root / ".claude-plugin" / "marketplace.json").write_text(
        json.dumps({"version": market_top, "plugins": [{"version": market_nested}]}),
        encoding="utf-8",
    )
    (root / "CHANGELOG.md").write_text(
        textwrap.dedent(
            f"""\
            # Changelog

            ## [{changelog}] - 2026-05-20

            - entry
            """
        ),
        encoding="utf-8",
    )


class VersionConsistencyDirectTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmpdir.name)
        self.mod = load_module()
        self.mod.REPO_ROOT = self.root

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def run_main(self) -> tuple[int, str]:
        out = io.StringIO()
        with redirect_stdout(out):
            rc = self.mod.main()
        return rc, out.getvalue()

    def test_readers_return_versions_from_repo_root(self) -> None:
        write_layout(self.root, cmake="2.3.4", plugin="0.6.0", market_top="0.6.0", market_nested="0.6.0")

        self.assertEqual(self.mod.read_cmake_version(), "2.3.4")
        self.assertEqual(self.mod.read_plugin_versions(), ("0.6.0", "0.6.0", "0.6.0"))
        self.assertEqual(self.mod.read_top_changelog_version(), "1.2.3")

    def test_missing_cmake_version_raises_clear_error(self) -> None:
        write_layout(self.root)
        (self.root / "CMakeLists.txt").write_text("project(Pulp)\n", encoding="utf-8")

        with self.assertRaisesRegex(RuntimeError, "no VERSION line"):
            self.mod.read_cmake_version()

    def test_missing_changelog_heading_raises_clear_error(self) -> None:
        write_layout(self.root)
        (self.root / "CHANGELOG.md").write_text("# Changelog\n\n## Unreleased\n", encoding="utf-8")

        with self.assertRaisesRegex(RuntimeError, "no '## \\[X.Y.Z\\]' heading"):
            self.mod.read_top_changelog_version()

    def test_changelog_reader_skips_unreleased_heading(self) -> None:
        write_layout(self.root)
        (self.root / "CHANGELOG.md").write_text(
            "# Changelog\n\n## Unreleased\n\n## [3.4.5] - 2026-05-20\n",
            encoding="utf-8",
        )

        self.assertEqual(self.mod.read_top_changelog_version(), "3.4.5")

    def test_missing_plugin_fields_read_as_empty_strings(self) -> None:
        write_layout(self.root)
        (self.root / ".claude-plugin" / "plugin.json").write_text("{}", encoding="utf-8")
        (self.root / ".claude-plugin" / "marketplace.json").write_text("{}", encoding="utf-8")

        self.assertEqual(self.mod.read_plugin_versions(), ("", "", ""))

    def test_main_reports_success_for_aligned_versions(self) -> None:
        write_layout(self.root)

        rc, output = self.run_main()

        self.assertEqual(rc, 0)
        self.assertIn("version consistency OK", output)

    def test_main_flags_plugin_marketplace_and_nested_drift(self) -> None:
        write_layout(self.root, plugin="0.5.0", market_top="0.4.0", market_nested="0.3.0")

        rc, output = self.run_main()

        self.assertEqual(rc, 1)
        self.assertIn("plugin.json (0.5.0) != marketplace.json top-level", output)
        self.assertIn("plugin.json (0.5.0) != marketplace.json plugins[0].version", output)
        self.assertIn("marketplace.json top-level version (0.4.0) != plugins[0].version", output)

    def test_main_flags_changelog_ahead_of_cmake(self) -> None:
        write_layout(self.root, cmake="1.2.3", changelog="1.2.4")

        rc, output = self.run_main()

        self.assertEqual(rc, 1)
        self.assertIn("CHANGELOG top entry [1.2.4] advertises a version", output)
        self.assertIn("CMakeLists VERSION:", output)

    def test_main_allows_cmake_ahead_of_changelog(self) -> None:
        write_layout(self.root, cmake="1.2.4", changelog="1.2.3")

        rc, output = self.run_main()

        self.assertEqual(rc, 0)
        self.assertIn("SDK 1.2.4", output)

    def test_script_entrypoint_exits_with_main_return_code(self) -> None:
        write_layout(self.root)
        script = self.root / "tools" / "scripts" / "version_consistency_check.py"
        script.parent.mkdir(parents=True)
        shutil.copyfile(MODULE_PATH, script)

        with redirect_stdout(io.StringIO()) as output:
            with self.assertRaises(SystemExit) as ctx:
                runpy.run_path(str(script), run_name="__main__")

        self.assertEqual(ctx.exception.code, 0)
        self.assertIn("version consistency OK", output.getvalue())

    def test_repo_script_entrypoint_is_covered_in_process(self) -> None:
        with redirect_stdout(io.StringIO()) as output:
            with self.assertRaises(SystemExit) as ctx:
                runpy.run_path(str(MODULE_PATH), run_name="__main__")

        self.assertEqual(ctx.exception.code, 0)
        self.assertIn("version consistency OK", output.getvalue())


if __name__ == "__main__":
    unittest.main()

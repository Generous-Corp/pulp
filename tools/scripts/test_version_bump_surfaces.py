#!/usr/bin/env python3
"""Fixture tests for the version_bump Surface cluster.

Config loading + version-file readers/writers. Mirrors
`version_bump_surfaces.py`. Split from `test_gates.py` (P9-NEW refactor,
2026-05); test bodies are byte-identical to their previous definitions.

Runs standalone (`python3 tools/scripts/test_version_bump_surfaces.py`)
or as part of the aggregate suite via `test_gates.py`.
"""

from __future__ import annotations

import json
import subprocess
import unittest
from unittest import mock

from gate_test_support import GateFixtureTestCase, VBC, _run


class VersionBumpSurfacesTests(GateFixtureTestCase):
    """version_bump_check Surface-cluster fixtures."""

    def test_version_file_helpers_cover_supported_kinds(self) -> None:
        """Direct coverage for the version-file readers/writers that
        integration fixtures only hit through the CMake + JSON paths."""
        vbc = self._import_gate_module("version_bump_check")

        cases = [
            (
                "package.json",
                '{"name": "demo", "version": "1.2.3"}\n',
                vbc.VersionFile("package.json", "json_field", "version"),
                "1.2.3",
                "2.0.0",
                '"version": "2.0.0"',
            ),
            (
                "pyproject.toml",
                '[project]\nname = "demo"\nversion = "0.4.0"\n',
                vbc.VersionFile("pyproject.toml", "pyproject_version"),
                "0.4.0",
                "0.5.0",
                'version = "0.5.0"',
            ),
            (
                "pkg/__init__.py",
                '__version__ = "3.2.1"\n',
                vbc.VersionFile("pkg/__init__.py", "python_dunder_version"),
                "3.2.1",
                "3.2.2",
                '__version__ = "3.2.2"',
            ),
            (
                "VERSION.txt",
                "tool_version = 7.8.9\n",
                vbc.VersionFile("VERSION.txt", "regex",
                                pattern=r"tool_version\s*=\s*(\d+\.\d+\.\d+)"),
                "7.8.9",
                "8.0.0",
                "tool_version = 8.0.0",
            ),
        ]

        for rel, text, vf, old, new, expected in cases:
            self.f.write(rel, text)
            self.assertEqual(vbc.read_version(self.tmp, vf), old)
            self.assertTrue(vbc.write_version(self.tmp, vf, new))
            written = (self.tmp / rel).read_text()
            self.assertIn(expected, written)

        self.f.write("bad.json", "{not json}\n")
        self.assertIsNone(
            vbc.read_version(self.tmp, vbc.VersionFile("bad.json", "json_field", "version"))
        )
        self.assertFalse(
            vbc.write_version(
                self.tmp,
                vbc.VersionFile("missing.toml", "pyproject_version"),
                "1.0.0",
            )
        )

    def test_json_path_read_write_and_walk_guards(self) -> None:
        vbc = self._import_gate_module("version_bump_check")

        self.f.write(
            "manifest.json",
            json.dumps({
                "plugins": [
                    {"name": "alpha", "version": "1.0.0"},
                    {"name": "beta", "version": "2.0.0"},
                ],
                "metadata": {"tool": {"version": "3.0.0"}},
            }, indent=2) + "\n",
        )

        plugin_vf = vbc.VersionFile("manifest.json", "json_path", "plugins.1.version")
        self.assertEqual(vbc.read_version(self.tmp, plugin_vf), "2.0.0")
        self.assertTrue(vbc.write_version(self.tmp, plugin_vf, "2.1.0"))
        manifest = json.loads((self.tmp / "manifest.json").read_text())
        self.assertEqual(manifest["plugins"][1]["version"], "2.1.0")
        self.assertTrue((self.tmp / "manifest.json").read_text().endswith("\n"))

        nested_vf = vbc.VersionFile("manifest.json", "json_path", "metadata.tool.version")
        self.assertEqual(vbc.read_version(self.tmp, nested_vf), "3.0.0")

        self.assertIsNone(vbc._json_walk_get(manifest, ""))
        self.assertIsNone(vbc._json_walk_get(manifest, "plugins..version"))
        self.assertIsNone(vbc._json_walk_get(manifest, "metadata.missing.version"))
        self.assertIsNone(vbc._json_walk_get(manifest, "plugins.nope.version"))
        self.assertIsNone(vbc._json_walk_get(manifest, "plugins.4.version"))
        self.assertIsNone(vbc._json_walk_get(manifest, "metadata.tool.version.patch"))

        self.assertFalse(vbc._json_walk_set(manifest, "", "x"))
        self.assertFalse(vbc._json_walk_set(manifest, "plugins..version", "x"))
        self.assertFalse(vbc._json_walk_set(manifest, "plugins.nope.version", "x"))
        self.assertFalse(vbc._json_walk_set(manifest, "plugins.4.version", "x"))
        self.assertFalse(vbc._json_walk_set(manifest, "metadata.tool.version.patch", "x"))
        self.assertFalse(vbc._json_walk_set(manifest, "plugins.0.missing.0", "x"))
        self.assertFalse(vbc._json_walk_set(manifest, "plugins.0.version.patch", "x"))
        self.assertFalse(vbc._json_walk_set(manifest, "plugins.0.version.patch.leaf", "x"))

        root_list = [{"version": "1.0.0"}]
        self.assertTrue(vbc._json_walk_set(root_list, "0", {"version": "1.1.0"}))
        self.assertEqual(root_list[0]["version"], "1.1.0")
        self.assertFalse(vbc._json_walk_set(root_list, "nope", "x"))
        self.assertFalse(vbc._json_walk_set(root_list, "4", "x"))

    def test_version_file_failure_modes_are_fail_closed(self) -> None:
        vbc = self._import_gate_module("version_bump_check")
        vbs = self._import_gate_module("version_bump_surfaces")

        self.assertIsNone(
            vbc.read_version(self.tmp, vbc.VersionFile("missing.json", "json_field", "version"))
        )
        with mock.patch.object(vbs, "_vbc", return_value=object()):
            self.assertIs(vbs._h("already_bumped"), vbs.already_bumped)
        self.assertIsNone(
            vbc._extract_version_from_text(
                "{not json}\n",
                vbc.VersionFile("manifest.json", "json_path", "plugins.0.version"),
            )
        )
        self.assertEqual(
            vbc._extract_version_from_text(
                '[project]\nversion = "4.5.6"\n',
                vbc.VersionFile("pyproject.toml", "pyproject_version"),
            ),
            "4.5.6",
        )
        self.assertEqual(
            vbc._extract_version_from_text(
                '__version__ = "7.8.9"\n',
                vbc.VersionFile("pkg/__init__.py", "python_dunder_version"),
            ),
            "7.8.9",
        )
        self.assertIsNone(
            vbc._extract_version_from_text(
                "version=1.0.0\n",
                vbc.VersionFile("VERSION.txt", "regex",
                                pattern=r"version=(\d+\.\d+\.\d+)"),
            )
        )

        self.f.write("bad-field.json", "{not json}\n")
        self.f.write("bad-path.json", "{not json}\n")
        self.f.write("empty-path.json", '{"plugins": []}\n')
        self.f.write("not-project.cmake", "cmake_minimum_required(VERSION 3.25)\n")
        self.f.write("plain.txt", "no version here\n")

        self.assertIsNone(
            vbc.read_version(
                self.tmp,
                vbc.VersionFile("not-project.cmake", "cmake_project_version"),
            )
        )
        self.assertIsNone(
            vbc.read_version(self.tmp, vbc.VersionFile("bad-path.json", "json_path", "a.b"))
        )
        self.assertIsNone(
            vbc.read_version(
                self.tmp,
                vbc.VersionFile("plain.txt", "unknown_kind"),
            )
        )
        self.assertFalse(
            vbc.write_version(
                self.tmp,
                vbc.VersionFile("bad-field.json", "json_field", "version"),
                "1.0.0",
            )
        )
        self.assertFalse(
            vbc.write_version(
                self.tmp,
                vbc.VersionFile("bad-path.json", "json_path", "a.b"),
                "1.0.0",
            )
        )
        self.assertFalse(
            vbc.write_version(
                self.tmp,
                vbc.VersionFile("empty-path.json", "json_path", "plugins.4.version"),
                "1.0.0",
            )
        )
        self.assertFalse(
            vbc.write_version(
                self.tmp,
                vbc.VersionFile("plain.txt", "regex", pattern=r"missing=(\d+\.\d+\.\d+)"),
                "1.0.0",
            )
        )
        self.assertFalse(
            vbc.write_version(
                self.tmp,
                vbc.VersionFile("plain.txt", "unknown_kind"),
                "1.0.0",
            )
        )

    def test_base_version_helpers_cover_missing_and_equal_paths(self) -> None:
        vbc = self._import_gate_module("version_bump_check")
        vbs = self._import_gate_module("version_bump_surfaces")

        vf = vbc.VersionFile("CMakeLists.txt", "cmake_project_version")
        with mock.patch.object(vbc, "version_at_base", return_value="0.1.0"):
            self.assertFalse(vbc.already_bumped("origin/main", vf, self.tmp))
        with mock.patch.object(
            vbs.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(
                ["git"], 0, stdout="project(Pulp VERSION 9.8.7)\n",
            ),
        ):
            self.assertEqual(vbc.version_at_base("origin/main", vf), "9.8.7")
        self.assertIsNone(
            vbc.version_at_base(
                "origin/main",
                vbc.VersionFile("missing.json", "json_field", "version"),
            )
        )

        self.assertTrue(vbc.write_version(self.tmp, vf, "0.2.0"))
        with mock.patch.object(vbc, "version_at_base", return_value="0.1.0"):
            self.assertTrue(vbc.already_bumped("origin/main", vf, self.tmp))
        with mock.patch.object(vbc, "version_at_base", return_value="0.2.0"):
            self.assertFalse(vbc.already_bumped("origin/main", vf, self.tmp))
        with mock.patch.object(vbc, "version_at_base", return_value=None):
            self.assertFalse(vbc.already_bumped("origin/main", vf, self.tmp))
        with mock.patch.object(vbc, "version_at_base", return_value="1.0.0"):
            self.assertFalse(
                vbc.already_bumped(
                    "origin/main",
                    vbc.VersionFile("missing.txt", "regex",
                                    pattern=r"version=(\d+\.\d+\.\d+)"),
                    self.tmp,
                )
            )

        self.f.write("broken.json", "{not json}\n")
        with mock.patch.object(vbc, "version_at_base", return_value="1.0.0"):
            self.assertFalse(
                vbc.already_bumped(
                    "origin/main",
                    vbc.VersionFile("broken.json", "json_field", "version"),
                    self.tmp,
                )
            )

    def test_version_config_loader_strips_metadata(self) -> None:
        vbc = self._import_gate_module("version_bump_check")

        cfg_path = self.tmp / "custom-versioning.json"
        cfg_path.write_text(json.dumps({
            "$schema": "https://example.invalid/schema.json",
            "_comment": "ignored top-level metadata",
            "generated_globs": ["build/**"],
            "trailers": {"version_bump": "Custom-Bump"},
            "surfaces": {
                "sdk": {
                    "_comment": "ignored surface metadata",
                    "label": "SDK",
                    "version_files": [{
                        "_note": "ignored version-file metadata",
                        "path": "CMakeLists.txt",
                        "kind": "cmake_project_version",
                    }],
                    "trigger_paths": ["core/**"],
                    "public_api_paths": ["core/**/include/**"],
                    "internal_only_paths": ["core/**/src/**"],
                    "changelog": "CHANGELOG.md",
                },
            },
        }, indent=2) + "\n")

        cfg = vbc.load_config(cfg_path)
        self.assertEqual(cfg.generated_globs, ["build/**"])
        self.assertEqual(cfg.trailer_version_bump, "Custom-Bump")
        self.assertEqual(len(cfg.surfaces), 1)
        self.assertEqual(cfg.surfaces[0].name, "sdk")
        self.assertEqual(cfg.surfaces[0].version_files[0].path, "CMakeLists.txt")
        self.assertEqual(cfg.surfaces[0].changelog, "CHANGELOG.md")

    def test_version_bump_missing_config_returns_usage_error(self) -> None:
        code, out = _run(
            [
                "python3", str(VBC),
                "--base", "origin/main",
                "--config", str(self.tmp / "missing-versioning.json"),
            ],
            cwd=self.tmp,
        )
        self.assertEqual(code, 2, msg=out)
        self.assertIn("config not found", out)


if __name__ == "__main__":
    unittest.main(verbosity=2)

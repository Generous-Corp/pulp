#!/usr/bin/env python3
"""Extra focused unit tests for tools/scripts/run_python_coverage.py."""

from __future__ import annotations

import importlib.util
import pathlib
import runpy
import sys
import tempfile
import types
import unittest
from unittest import mock


SCRIPT = pathlib.Path(__file__).parent / "run_python_coverage.py"

spec = importlib.util.spec_from_file_location("run_python_coverage_extra", SCRIPT)
assert spec and spec.loader
rpc = importlib.util.module_from_spec(spec)
sys.modules["run_python_coverage_extra"] = rpc
spec.loader.exec_module(rpc)


class VersionGuardExtraTests(unittest.TestCase):
    def test_rejects_missing_coverage_module(self) -> None:
        with mock.patch.object(rpc, "coverage", None):
            with self.assertRaises(SystemExit) as ctx:
                rpc._require_supported_coverage()

        self.assertIn("requires coverage.py", str(ctx.exception))

    def test_accepts_unparseable_coverage_version(self) -> None:
        fake_coverage = mock.Mock(__version__="local-dev")
        with mock.patch.object(rpc, "coverage", fake_coverage):
            rpc._require_supported_coverage()

    def test_script_entrypoint_exits_with_main_status(self) -> None:
        fake_coverage = types.SimpleNamespace(__version__="7.10.0")
        with mock.patch.object(
            sys,
            "argv",
            [str(SCRIPT), "--pattern", "tools/scripts/test_definitely_missing.py"],
        ), mock.patch.dict(sys.modules, {"coverage": fake_coverage}):
            with self.assertRaises(SystemExit) as ctx:
                runpy.run_path(str(SCRIPT), run_name="__main__")

        self.assertEqual(ctx.exception.code, 1)


class DiscoveryExtraTests(unittest.TestCase):
    def test_discover_tests_dedupes_patterns_and_skips_directories(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            test_file = root / "tools/scripts/test_alpha.py"
            test_file.parent.mkdir(parents=True)
            test_file.write_text("print('ok')\n", encoding="utf-8")
            (root / "tools/scripts/test_dir.py").mkdir()

            with mock.patch.object(rpc, "REPO_ROOT", root):
                tests = rpc._discover_tests(
                    [
                        "tools/scripts/test_*.py",
                        "tools/scripts/test_alpha.py",
                    ]
                )

        self.assertEqual(tests, [test_file])

    def test_has_selected_test_surface_tracks_configured_patterns(self) -> None:
        self.assertTrue(
            rpc._has_selected_test_surface(
                [rpc.REPO_ROOT / "tools/scripts/test_alpha.py"]
            )
        )
        self.assertTrue(
            rpc._has_selected_test_surface(
                [rpc.REPO_ROOT / "tools/import-validation/test_source_contracts.py"]
            )
        )
        self.assertFalse(
            rpc._has_selected_test_surface(
                [rpc.REPO_ROOT / "tools/packages/test_registry.py"]
            )
        )


class SurfaceExtraTests(unittest.TestCase):
    def test_resolved_omit_globs_uses_test_globs_when_omit_globs_are_empty(self) -> None:
        surface = rpc.CoverageSurface(
            ("tools/scripts", "tools/deps"),
            ("tools/scripts/test_*.py",),
        )

        self.assertEqual(
            surface.resolved_omit_globs(),
            (
                "tools/scripts/test_*.py",
                "tools/scripts/_*.py",
                "tools/deps/_*.py",
            ),
        )

    def test_normalized_source_roots_keeps_siblings_when_parent_is_not_selected(self) -> None:
        surfaces = [
            rpc.CoverageSurface(("tools/scripts",), ("tools/scripts/test_*.py",)),
            rpc.CoverageSurface(("tools/deps",), ("tools/deps/test_*.py",)),
        ]

        self.assertEqual(
            rpc._normalized_source_roots(surfaces),
            ["tools/deps", "tools/scripts"],
        )

    def test_matches_any_glob_returns_false_for_empty_pattern_set(self) -> None:
        self.assertFalse(rpc._matches_any_glob("tools/scripts/run.py", []))

    def test_selected_surfaces_appends_always_include_surface(self) -> None:
        selected = rpc._selected_surfaces(
            [rpc.REPO_ROOT / "tools/scripts/test_alpha.py"]
        )

        self.assertGreaterEqual(len(selected), 2)
        self.assertEqual(selected[0].source_roots, ("tools/scripts",))
        self.assertTrue(selected[-1].always_include)

    def test_normalized_source_roots_drops_children_when_parent_selected(self) -> None:
        surfaces = [
            rpc.CoverageSurface(("tools",), ("tools/test_*.py",)),
            rpc.CoverageSurface(("tools/scripts",), ("tools/scripts/test_*.py",)),
            rpc.CoverageSurface(("core/view/js",), ("tools/test_*.py",)),
        ]

        self.assertEqual(rpc._normalized_source_roots(surfaces), ["tools", "core/view/js"])

    def test_report_source_files_respects_omit_globs_and_private_modules(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            script_dir = root / "tools" / "scripts"
            script_dir.mkdir(parents=True)
            keep = script_dir / "run_me.py"
            keep.write_text("print('keep')\n", encoding="utf-8")
            (script_dir / "test_run_me.py").write_text("print('test')\n", encoding="utf-8")
            (script_dir / "_private.py").write_text("print('private')\n", encoding="utf-8")

            with mock.patch.object(rpc, "REPO_ROOT", root):
                files = rpc._report_source_files(
                    ["tools/scripts"],
                    ["tools/scripts/test_*.py", "tools/scripts/_*.py"],
                )

        self.assertEqual(files, [keep])

    def test_write_coveragerc_requires_selected_sources_and_writes_omit(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            out = pathlib.Path(td)
            with mock.patch.object(rpc, "OUTPUT_DIR", out), \
                 mock.patch.object(rpc, "HTML_DIR", out / "html"), \
                 mock.patch.object(rpc, "XML_FILE", out / "coverage.xml"), \
                 mock.patch.object(rpc, "RCFILE", out / ".coveragerc"):
                with self.assertRaises(ValueError):
                    rpc._write_coveragerc([])

                rpc._write_coveragerc(
                    [rpc.CoverageSurface(("tools/scripts",), ("tools/scripts/test_*.py",))]
                )
                text = (out / ".coveragerc").read_text(encoding="utf-8")

        self.assertIn("source =\n    tools/scripts", text)
        self.assertIn("omit =\n    tools/scripts/test_*.py", text)


if __name__ == "__main__":
    unittest.main()

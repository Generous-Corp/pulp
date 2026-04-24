#!/usr/bin/env python3
"""Unit tests for tools/scripts/run_python_coverage.py."""

from __future__ import annotations

import importlib.util
import pathlib
import sys
import tempfile
import unittest
from unittest import mock


SCRIPT = pathlib.Path(__file__).parent / "run_python_coverage.py"

spec = importlib.util.spec_from_file_location("run_python_coverage", SCRIPT)
assert spec and spec.loader
rpc = importlib.util.module_from_spec(spec)
sys.modules["run_python_coverage"] = rpc
spec.loader.exec_module(rpc)


class VersionGuardTests(unittest.TestCase):
    def test_rejects_coverage_older_than_710(self) -> None:
        fake_coverage = mock.Mock(__version__="7.9.9")
        with mock.patch.object(rpc, "coverage", fake_coverage):
            with self.assertRaises(SystemExit) as ctx:
                rpc._require_supported_coverage()
        self.assertIn(">= 7.10", str(ctx.exception))

    def test_accepts_coverage_710_and_newer(self) -> None:
        for version in ("7.10.0", "7.13.5", "8.0.0"):
            with self.subTest(version=version):
                fake_coverage = mock.Mock(__version__=version)
                with mock.patch.object(rpc, "coverage", fake_coverage):
                    rpc._require_supported_coverage()


class CoveragercTests(unittest.TestCase):
    def test_write_coveragerc_tracks_selected_surfaces(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            output = root / "out"
            html = output / "html"
            xml = output / "coverage.python.xml"
            rcfile = output / ".coveragerc"
            surfaces = [
                rpc.CoverageSurface(("tools/scripts",), ("tools/scripts/test_*.py",)),
                rpc.CoverageSurface(("tools/deps",), ("tools/deps/test_*.py",)),
                rpc.CoverageSurface(("tools/local-ci",), ("tools/local-ci/test_*.py",)),
                rpc.CoverageSurface(
                    ("tools", "core/view/js"),
                    (),
                    (
                        "tools/test_*.py",
                        "tools/scripts/test_*.py",
                        "tools/deps/test_*.py",
                        "tools/local-ci/test_*.py",
                        "tools/packages/test_*.py",
                    ),
                    always_include=True,
                ),
            ]
            with mock.patch.object(rpc, "OUTPUT_DIR", output), \
                 mock.patch.object(rpc, "HTML_DIR", html), \
                 mock.patch.object(rpc, "XML_FILE", xml), \
                 mock.patch.object(rpc, "RCFILE", rcfile):
                rpc._write_coveragerc(surfaces)
            text = rcfile.read_text(encoding="utf-8")
            self.assertIn("patch =", text)
            self.assertIn("subprocess", text)
            self.assertIn(html.as_posix(), text)
            self.assertIn(xml.as_posix(), text)
            self.assertIn("\n    tools\n", text)
            self.assertIn("core/view/js", text)
            self.assertIn("tools/test_*.py", text)
            self.assertIn("tools/scripts/test_*.py", text)
            self.assertIn("tools/local-ci/test_*.py", text)
            self.assertIn("tools/deps/_*.py", text)
            self.assertIn("tools/packages/test_*.py", text)
            self.assertIn("core/view/js/_*.py", text)

    def test_selected_surfaces_follow_discovered_tests(self) -> None:
        tests = [
            rpc.REPO_ROOT / "tools/scripts/test_alpha.py",
            rpc.REPO_ROOT / "tools/local-ci/test_local_ci.py",
        ]
        surfaces = rpc._selected_surfaces(tests)
        self.assertEqual(
            [surface.source_roots[0] for surface in surfaces],
            ["tools/scripts", "tools/local-ci", "tools"],
        )

    def test_default_test_globs_keep_top_level_tools_tests_out_of_default_run(self) -> None:
        self.assertNotIn("tools/test_*.py", rpc.DEFAULT_TEST_GLOBS)

    def test_normalized_source_roots_drop_nested_tools_paths(self) -> None:
        surfaces = rpc._selected_surfaces(
            [
                rpc.REPO_ROOT / "tools/scripts/test_alpha.py",
                rpc.REPO_ROOT / "tools/deps/test_audit.py",
                rpc.REPO_ROOT / "tools/local-ci/test_local_ci.py",
            ]
        )
        self.assertEqual(rpc._normalized_source_roots(surfaces), ["tools", "core/view/js"])


class MainFlowTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tests = [
            rpc.REPO_ROOT / "tools/scripts/test_alpha.py",
            rpc.REPO_ROOT / "tools/deps/test_audit.py",
            rpc.REPO_ROOT / "tools/local-ci/test_local_ci.py",
        ]

    def test_returns_one_when_no_tests_match(self) -> None:
        with mock.patch.object(rpc, "_require_supported_coverage"), \
             mock.patch.object(rpc, "_discover_tests", return_value=[]):
            rc = rpc.main(["--pattern", "tools/scripts/test_none.py"])
        self.assertEqual(rc, 1)

    def test_sets_coverage_env_and_surfaces_failures(self) -> None:
        envs: list[dict[str, str]] = []
        returns = iter([0, 0, 3])

        def fake_run(test_path: pathlib.Path, env: dict[str, str]) -> int:
            envs.append(env.copy())
            self.assertIn(test_path, self.tests)
            return next(returns)

        with mock.patch.object(rpc, "_require_supported_coverage"), \
             mock.patch.object(rpc, "_discover_tests", return_value=self.tests), \
             mock.patch.object(rpc, "_write_coveragerc") as write_coveragerc, \
             mock.patch.object(rpc.shutil, "rmtree"), \
             mock.patch.object(rpc, "_run_test", side_effect=fake_run), \
             mock.patch.object(rpc, "_build_reports"):
            rc = rpc.main([])

        self.assertEqual(rc, 1)
        self.assertEqual(len(envs), 3)
        for env in envs:
            self.assertEqual(env["COVERAGE_PROCESS_START"], str(rpc.RCFILE))
            self.assertEqual(env["COVERAGE_FILE"], str(rpc.DATA_FILE))
        surfaces = write_coveragerc.call_args.args[0]
        self.assertEqual(
            [surface.source_roots[0] for surface in surfaces],
            ["tools/scripts", "tools/deps", "tools/local-ci", "tools"],
        )

    def test_returns_one_when_report_builder_has_no_data(self) -> None:
        fake_coverage = mock.Mock()
        fake_coverage.exceptions = mock.Mock()
        fake_coverage.exceptions.NoDataError = RuntimeError
        with mock.patch.object(rpc, "_require_supported_coverage"), \
             mock.patch.object(rpc, "_discover_tests", return_value=[self.tests[0]]), \
             mock.patch.object(rpc, "_write_coveragerc"), \
             mock.patch.object(rpc.shutil, "rmtree"), \
             mock.patch.object(rpc, "_run_test", return_value=0), \
             mock.patch.object(rpc, "coverage", fake_coverage), \
             mock.patch.object(
                 rpc,
                 "_build_reports",
                 side_effect=RuntimeError("empty"),
             ):
            rc = rpc.main([])
        self.assertEqual(rc, 1)

    def test_returns_one_when_tests_are_outside_configured_surfaces(self) -> None:
        with mock.patch.object(rpc, "_require_supported_coverage"), \
             mock.patch.object(
                 rpc,
                 "_discover_tests",
                 return_value=[rpc.REPO_ROOT / "scripts/test_smoke.py"],
             ), \
             mock.patch.object(rpc.shutil, "rmtree"), \
             mock.patch.object(rpc, "_build_reports"):
            rc = rpc.main([])
        self.assertEqual(rc, 1)

    def test_returns_zero_on_success(self) -> None:
        with mock.patch.object(rpc, "_require_supported_coverage"), \
             mock.patch.object(rpc, "_discover_tests", return_value=[self.tests[0]]), \
             mock.patch.object(rpc, "_write_coveragerc") as write_coveragerc, \
             mock.patch.object(rpc.shutil, "rmtree"), \
             mock.patch.object(rpc, "_run_test", return_value=0), \
             mock.patch.object(rpc, "_build_reports"):
            rc = rpc.main([])
        self.assertEqual(rc, 0)
        surfaces = write_coveragerc.call_args.args[0]
        self.assertEqual(
            [surface.source_roots[0] for surface in surfaces],
            ["tools/scripts", "tools"],
        )

    def test_broader_slice_is_selected_even_without_top_level_tools_tests(self) -> None:
        tests = [rpc.REPO_ROOT / "tools/scripts/test_alpha.py"]
        surfaces = rpc._selected_surfaces(tests)
        self.assertEqual(
            [surface.source_roots for surface in surfaces],
            [("tools/scripts",), ("tools", "core/view/js")],
        )

    def test_main_includes_broader_slice_for_default_tooling_tests(self) -> None:
        tests = [rpc.REPO_ROOT / "tools/scripts/test_alpha.py"]
        with mock.patch.object(rpc, "_require_supported_coverage"), \
             mock.patch.object(rpc, "_discover_tests", return_value=tests), \
             mock.patch.object(rpc, "_write_coveragerc") as write_coveragerc, \
             mock.patch.object(rpc.shutil, "rmtree"), \
             mock.patch.object(rpc, "_run_test", return_value=0), \
             mock.patch.object(rpc, "_build_reports"):
            rc = rpc.main([])

        self.assertEqual(rc, 0)
        surfaces = write_coveragerc.call_args.args[0]
        self.assertEqual(
            [surface.source_roots for surface in surfaces],
            [("tools/scripts",), ("tools", "core/view/js")],
        )


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Unit tests for tools/scripts/run_python_coverage.py."""

from __future__ import annotations

import importlib.util
import pathlib
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
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

    def test_default_test_globs_include_targeted_top_level_tooling_tests(self) -> None:
        self.assertIn("tools/test_check_format_validation.py", rpc.DEFAULT_TEST_GLOBS)
        self.assertIn("tools/test_check_status_ladder.py", rpc.DEFAULT_TEST_GLOBS)
        self.assertIn(
            "tools/test_motion_visual_analyze_sequence.py",
            rpc.DEFAULT_TEST_GLOBS,
        )
        self.assertIn(
            "tools/test_motion_visual_capture_sim_frames.py",
            rpc.DEFAULT_TEST_GLOBS,
        )
        self.assertIn(
            "tools/import-validation/test_diff_against_reference.py",
            rpc.DEFAULT_TEST_GLOBS,
        )
        self.assertIn(
            "tools/import-validation/test_diff_against_reference_regions.py",
            rpc.DEFAULT_TEST_GLOBS,
        )
        self.assertIn(
            "tools/import-validation/test_source_contracts.py",
            rpc.DEFAULT_TEST_GLOBS,
        )
        self.assertIn(
            "tools/harness/tests/test_verifier.py",
            rpc.DEFAULT_TEST_GLOBS,
        )
        self.assertIn(
            "tools/harness/tests/test_rn_adapter.py",
            rpc.DEFAULT_TEST_GLOBS,
        )
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

    def test_report_source_files_recurses_into_non_package_tooling_dirs(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            for rel_path in (
                "tools/audit.py",
                "tools/test_audit.py",
                "tools/packages/freshness_check.py",
                "tools/packages/validate_registry.py",
                "tools/packages/_private.py",
                "core/view/js/embed_js.py",
            ):
                path = root / rel_path
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("print('covered by inventory')\n", encoding="utf-8")

            with mock.patch.object(rpc, "REPO_ROOT", root):
                files = rpc._report_source_files(
                    ["tools", "core/view/js"],
                    [
                        "tools/test_*.py",
                        "tools/packages/_*.py",
                        "core/view/js/_*.py",
                    ],
                )

            self.assertEqual(
                [path.relative_to(root).as_posix() for path in files],
                [
                    "tools/audit.py",
                    "tools/packages/freshness_check.py",
                    "tools/packages/validate_registry.py",
                    "core/view/js/embed_js.py",
                ],
            )

    def test_report_source_files_accepts_file_roots_and_dedupes(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            source = root / "tools/audit.py"
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_text("print('source')\n", encoding="utf-8")
            not_python = root / "tools/README.txt"
            not_python.write_text("not python\n", encoding="utf-8")

            with mock.patch.object(rpc, "REPO_ROOT", root):
                files = rpc._report_source_files(
                    ["tools/audit.py", "tools", "tools/README.txt"],
                    [],
                )

            self.assertEqual(
                [path.relative_to(root).as_posix() for path in files],
                ["tools/audit.py"],
            )

    def test_touch_report_source_files_marks_unexecuted_sources_measured(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            source = root / "tools/packages/freshness_check.py"
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_text("print('zero hit source')\n", encoding="utf-8")
            omitted = root / "tools/packages/_private.py"
            omitted.write_text("print('not reportable')\n", encoding="utf-8")

            touched: list[str] = []
            fake_data = mock.Mock()
            fake_data.touch_file.side_effect = touched.append
            fake_cov = mock.Mock()
            fake_cov.get_data.return_value = fake_data

            with mock.patch.object(rpc, "REPO_ROOT", root):
                rpc._touch_report_source_files(
                    fake_cov,
                    ["tools"],
                    ["tools/packages/_*.py"],
                )

            self.assertEqual(touched, ["tools/packages/freshness_check.py"])

    def test_repo_relative_xml_filename_handles_all_resolution_modes(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            source = root / "tools/audit.py"
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_text("print('source')\n", encoding="utf-8")

            outside = pathlib.Path(td).parent / "outside_coverage_source.py"
            outside.write_text("print('outside')\n", encoding="utf-8")
            self.addCleanup(lambda: outside.unlink(missing_ok=True))

            with mock.patch.object(rpc, "REPO_ROOT", root):
                self.assertEqual(
                    rpc._repo_relative_xml_filename(str(source), []),
                    "tools/audit.py",
                )
                self.assertEqual(
                    rpc._repo_relative_xml_filename(str(outside), []),
                    str(outside),
                )
                self.assertEqual(
                    rpc._repo_relative_xml_filename("tools/audit.py", []),
                    "tools/audit.py",
                )
                self.assertEqual(
                    rpc._repo_relative_xml_filename("audit.py", [str(root / "tools")]),
                    "tools/audit.py",
                )
                self.assertEqual(
                    rpc._repo_relative_xml_filename("missing.py", [str(outside.parent)]),
                    "missing.py",
                )

    def test_rewrite_cobertura_filenames_uses_repo_relative_paths(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            for rel_path in (
                "tools/audit.py",
                "tools/deps/audit.py",
                "core/view/js/embed_js.py",
            ):
                path = root / rel_path
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("print('source')\n", encoding="utf-8")

            xml = root / "coverage.xml"
            xml.write_text(
                """<?xml version="1.0" ?>
<coverage>
  <sources>
    <source>core/view/js</source>
    <source>tools</source>
  </sources>
  <packages>
    <package name="">
      <classes>
        <class name="audit.py" filename="audit.py" />
        <class name="deps/audit.py" filename="deps/audit.py" />
        <class name="embed_js.py" filename="embed_js.py" />
      </classes>
    </package>
  </packages>
</coverage>
""",
                encoding="utf-8",
            )

            with mock.patch.object(rpc, "REPO_ROOT", root):
                rpc._rewrite_cobertura_filenames(xml)

            tree = ET.parse(xml)
            self.assertEqual(
                [source.text for source in tree.findall("./sources/source")],
                ["."],
            )
            self.assertEqual(
                [node.get("filename") for node in tree.findall(".//class")],
                [
                    "tools/audit.py",
                    "tools/deps/audit.py",
                    "core/view/js/embed_js.py",
                ],
            )

    def test_rewrite_cobertura_filenames_handles_missing_sources_and_filenames(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            xml = root / "coverage.xml"
            xml.write_text(
                """<?xml version="1.0" ?>
<coverage>
  <packages>
    <package name="">
      <classes>
        <class name="without-filename" />
        <class name="unknown.py" filename="unknown.py" />
      </classes>
    </package>
  </packages>
</coverage>
""",
                encoding="utf-8",
            )

            with mock.patch.object(rpc, "REPO_ROOT", root):
                rpc._rewrite_cobertura_filenames(xml)

            tree = ET.parse(xml)
            classes = tree.findall(".//class")
            self.assertIsNone(classes[0].get("filename"))
            self.assertEqual(classes[1].get("filename"), "unknown.py")

    def test_write_coveragerc_rejects_empty_surface_selection(self) -> None:
        with self.assertRaisesRegex(ValueError, "no coverage surfaces selected"):
            rpc._write_coveragerc([])


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

    def test_import_validation_slice_runs_its_tests_and_omits_test_modules(self) -> None:
        tests = [rpc.REPO_ROOT / "tools/import-validation/test_source_contracts.py"]
        surfaces = rpc._selected_surfaces(tests)
        self.assertEqual(
            [surface.source_roots for surface in surfaces],
            [("tools/import-validation",), ("tools", "core/view/js")],
        )
        self.assertIn(
            "tools/import-validation/test_*.py",
            rpc._resolved_omit_globs(surfaces),
        )

    def test_broader_slice_is_not_duplicated_when_targeted_test_matches(self) -> None:
        tests = [rpc.REPO_ROOT / "tools/test_check_status_ladder.py"]
        surfaces = rpc._selected_surfaces(tests)
        self.assertEqual(
            [surface.source_roots for surface in surfaces],
            [("tools", "core/view/js")],
        )

    def test_run_test_invokes_coverage_cli(self) -> None:
        fake_proc = mock.Mock(returncode=7)
        with mock.patch.object(rpc.subprocess, "run", return_value=fake_proc) as run:
            rc = rpc._run_test(
                rpc.REPO_ROOT / "tools/scripts/test_alpha.py",
                {"COVERAGE_FILE": "data"},
            )

        self.assertEqual(rc, 7)
        self.assertEqual(run.call_args.kwargs["cwd"], rpc.REPO_ROOT)
        self.assertEqual(run.call_args.kwargs["env"], {"COVERAGE_FILE": "data"})
        self.assertEqual(
            run.call_args.args[0],
            [
                sys.executable,
                "-m",
                "coverage",
                "run",
                "--rcfile",
                str(rpc.RCFILE),
                "--parallel-mode",
                "tools/scripts/test_alpha.py",
            ],
        )

    def test_build_reports_combines_touches_and_rewrites_outputs(self) -> None:
        fake_cov = mock.Mock()
        fake_coverage = mock.Mock()
        fake_coverage.Coverage.return_value = fake_cov

        def fake_report(file, show_missing: bool) -> None:
            self.assertTrue(show_missing)
            file.write("summary\n")

        fake_cov.report.side_effect = fake_report
        surfaces = [rpc.CoverageSurface(("tools",), ("tools/test_*.py",))]

        with tempfile.TemporaryDirectory() as td:
            output = pathlib.Path(td)
            summary = output / "summary.txt"
            xml = output / "coverage.xml"
            html = output / "html"

            with mock.patch.object(rpc, "OUTPUT_DIR", output), \
                 mock.patch.object(rpc, "SUMMARY_FILE", summary), \
                 mock.patch.object(rpc, "XML_FILE", xml), \
                 mock.patch.object(rpc, "HTML_DIR", html), \
                 mock.patch.object(rpc, "coverage", fake_coverage), \
                 mock.patch.object(rpc, "_touch_report_source_files") as touch, \
                 mock.patch.object(rpc, "_rewrite_cobertura_filenames") as rewrite:
                rpc._build_reports(surfaces)

        fake_coverage.Coverage.assert_called_once_with(
            config_file=str(rpc.RCFILE),
            data_file=str(rpc.DATA_FILE),
        )
        fake_cov.combine.assert_called_once_with(data_paths=[str(output)], strict=True)
        fake_cov.save.assert_called_once_with()
        fake_cov.html_report.assert_called_once_with(directory=str(html))
        fake_cov.xml_report.assert_called_once_with(outfile=str(xml))
        touch.assert_called_once_with(fake_cov, ["tools"], ["tools/test_*.py", "tools/_*.py"])
        rewrite.assert_called_once_with(xml)

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

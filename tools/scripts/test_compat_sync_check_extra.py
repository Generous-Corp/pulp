#!/usr/bin/env python3
"""Additional coverage-lane tests for compat_sync_check.py."""

from __future__ import annotations

import contextlib
import io
import json
import pathlib
import runpy
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "tools" / "scripts"))
import compat_sync_check as csc  # noqa: E402


class GitHelperTests(unittest.TestCase):
    def test_repo_root_and_diff_names_delegate_to_git(self) -> None:
        completed = subprocess.CompletedProcess([], 0, stdout="/repo\n")
        with mock.patch.object(csc.subprocess, "run", return_value=completed) as run:
            self.assertEqual(csc.repo_root(), pathlib.Path("/repo"))

        run.assert_called_once_with(
            ["git", "rev-parse", "--show-toplevel"],
            check=True,
            capture_output=True,
            text=True,
        )

        diff = subprocess.CompletedProcess([], 0, stdout="a.cpp\n\nb.cpp\n")
        with mock.patch.object(csc.subprocess, "run", return_value=diff):
            self.assertEqual(csc.git_diff_names("base", "head"), ["a.cpp", "b.cpp"])

    def test_git_range_trailers_merges_all_commit_trailers(self) -> None:
        def fake_run(cmd: list[str], **kwargs) -> subprocess.CompletedProcess[str]:
            if cmd[:2] == ["git", "log"]:
                return subprocess.CompletedProcess(cmd, 0, stdout="commit 1\0commit 2\0\n")
            if cmd[:2] == ["git", "interpret-trailers"]:
                return subprocess.CompletedProcess(
                    cmd,
                    0,
                    stdout=(
                        'Compat-Update: skip prefix=css reason="rename"\n'
                        "Malformed trailer line\n"
                        "Other: value\n"
                    ),
                )
            raise AssertionError(cmd)

        with mock.patch.object(csc.subprocess, "run", side_effect=fake_run):
            trailers = csc.git_range_trailers("base", "head")

        self.assertEqual(
            trailers,
            {
                "compat-update": [
                    'skip prefix=css reason="rename"',
                    'skip prefix=css reason="rename"',
                ],
                "other": ["value", "value"],
            },
        )

    def test_git_range_trailers_tolerates_bad_range(self) -> None:
        with mock.patch.object(
            csc.subprocess,
            "run",
            side_effect=subprocess.CalledProcessError(128, ["git"]),
        ):
            self.assertEqual(csc.git_range_trailers("bad", "head"), {})


class ConfigAndGlobTests(unittest.TestCase):
    def test_strip_comments_leaves_non_dict_values_unchanged(self) -> None:
        raw = ["not", "a", "mapping"]
        self.assertIs(csc._strip_comments(raw), raw)

    def test_load_compat_map_strips_comments_and_unknown_requirements(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            path = pathlib.Path(td) / "compat_path_map.json"
            path.write_text(
                json.dumps(
                    {
                        "$schema": "ignored",
                        "_comment": "ignored",
                        "paths": {
                            "src.cpp": [
                                {"kind": "compat-json", "prefix": "css/"},
                                {"kind": "doc", "path": "docs/css.md"},
                                {"kind": "test", "glob": "test/test_css*.cpp"},
                                {"kind": "unknown"},
                            ]
                        },
                    }
                ),
                encoding="utf-8",
            )

            compat_map = csc.load_compat_map(path)

        reqs = compat_map.paths["src.cpp"]
        self.assertEqual([r.kind for r in reqs], ["compat-json", "doc", "test"])
        self.assertEqual(reqs[0].prefix, "css")
        self.assertEqual(reqs[1].path, "docs/css.md")
        self.assertEqual(reqs[2].glob, "test/test_css*.cpp")

    def test_load_compat_json_tolerates_missing_invalid_and_non_object(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            self.assertEqual(csc.load_compat_json(root / "missing.json"), {})

            invalid = root / "invalid.json"
            invalid.write_text("{bad", encoding="utf-8")
            self.assertEqual(csc.load_compat_json(invalid), {})

            list_json = root / "list.json"
            list_json.write_text("[]", encoding="utf-8")
            self.assertEqual(csc.load_compat_json(list_json), {})

            obj = root / "obj.json"
            obj.write_text('{"css": {}}', encoding="utf-8")
            self.assertEqual(csc.load_compat_json(obj), {"css": {}})

    def test_glob_matching_covers_starstar_and_question_patterns(self) -> None:
        self.assertTrue(csc._matches_any("core/view/src/widget.cpp", ["core/**"]))
        self.assertTrue(csc._matches_any("core/view/src/widget.cpp", ["**"]))
        self.assertTrue(
            csc._matches_any("core/view/src/widget.cpp", ["**/widget.cpp"])
        )
        self.assertTrue(
            csc._matches_any("core/view/src/widget.cpp", ["core/**/widget.cpp"])
        )
        self.assertTrue(csc._matches_any("test/test_a.cpp", ["test/test_?.cpp"]))
        self.assertFalse(csc._matches_any("test/test_long.cpp", ["test/test_?.cpp"]))

    def test_glob_translation_handles_existing_slash_before_starstar(self) -> None:
        trailing = csc._glob_to_regex("core//**")
        self.assertTrue(trailing.match("core/"))
        self.assertTrue(trailing.match("core/view.cpp"))

        middle = csc._glob_to_regex("core//**/widget.cpp")
        self.assertTrue(middle.match("core/widget.cpp"))
        self.assertTrue(middle.match("core/view/src/widget.cpp"))

    def test_parse_compat_update_ignores_non_skip_and_accepts_unquoted_reason(self) -> None:
        trailers = {
            "compat-update": [
                'note prefix=css reason="ignored"',
                "skip prefix=html reason=manual",
            ],
        }
        self.assertEqual(
            csc.parse_compat_update_trailer(trailers),
            {"html": "manual"},
        )


class ResolutionAndApplyTests(unittest.TestCase):
    def test_prefix_resolution_direct_wildcard_and_fallback(self) -> None:
        direct = csc.Requirement("compat-json", "css", None, None)
        wildcard = csc.Requirement("compat-json", "*", None, None)

        self.assertEqual(csc._resolve_prefixes_for_source(direct, "src", {}), ["css"])
        self.assertEqual(
            csc._resolve_prefixes_for_source(
                wildcard,
                "src",
                {"css": {}, "unknown": {}, "html": {}},
            ),
            ["css", "html"],
        )
        self.assertEqual(
            csc._resolve_prefixes_for_source(wildcard, "src", {})[0],
            "canvas2d",
        )

    def test_effective_prefixes_dedupes_and_handles_no_compat_json_req(self) -> None:
        reqs = [
            csc.Requirement("compat-json", "css", None, None),
            csc.Requirement("compat-json", "css", None, None),
            csc.Requirement("compat-json", "html", None, None),
        ]
        self.assertEqual(csc._effective_prefixes_for_source(reqs, {}), ["css", "html"])
        self.assertEqual(
            csc._effective_prefixes_for_source(
                [csc.Requirement("doc", None, "docs/x.md", None)],
                {},
            ),
            ["*"],
        )

    def test_effective_prefixes_wildcard_falls_back_without_known_sections(self) -> None:
        reqs = [csc.Requirement("compat-json", "*", None, None)]
        self.assertEqual(
            csc._effective_prefixes_for_source(reqs, {"made-up": {}}),
            sorted(csc.KNOWN_PREFIXES),
        )

    def test_compute_findings_suppresses_duplicate_shared_rows(self) -> None:
        compat_map = csc.CompatMap(paths={
            "src.cpp": [
                csc.Requirement("compat-json", "css", None, None),
                csc.Requirement("compat-json", "html", None, None),
                csc.Requirement("doc", None, "docs/common.md", None),
                csc.Requirement("test", None, None, "test/test_common*.cpp"),
            ],
        })
        findings = csc.compute_findings(
            changed=["src.cpp", "docs/common.md", "test/test_common.cpp"],
            compat_map=compat_map,
            compat_data={"css": {}, "html": {}},
            bypasses={},
        )

        self.assertTrue(all(f.satisfied for f in findings))
        self.assertEqual(
            sorted(
                f.resolved_prefix
                for f in findings
                if f.requirement.kind == "compat-json"
            ),
            ["css", "html"],
        )
        self.assertEqual(
            [f.resolved_prefix for f in findings if f.requirement.kind == "doc"],
            ["css"],
        )
        self.assertEqual(
            [f.resolved_prefix for f in findings if f.requirement.kind == "test"],
            ["css"],
        )

    def test_compute_findings_ignores_unknown_requirement_kind(self) -> None:
        compat_map = csc.CompatMap(paths={
            "src.cpp": [
                csc.Requirement("mystery", None, None, None),
            ],
        })

        self.assertEqual(
            csc.compute_findings(
                changed=["src.cpp"],
                compat_map=compat_map,
                compat_data={},
                bypasses={},
            ),
            [],
        )

    def test_apply_stubs_creates_empty_file_sections_when_compat_json_missing(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            path = pathlib.Path(td) / "compat.json"
            added = csc.apply_stubs([], {}, path)
            data = json.loads(path.read_text(encoding="utf-8"))

        self.assertEqual(added, sorted(csc.KNOWN_PREFIXES))
        self.assertEqual(data["compat-schema-version"], "0.1")
        self.assertEqual(data["css"], {})

    def test_apply_stubs_adds_only_unsatisfied_missing_compat_sections(self) -> None:
        missing = csc.Finding(
            source_path="src.cpp",
            requirement=csc.Requirement("compat-json", "css", None, None),
            resolved_prefix="css",
            satisfied=False,
            bypass_reason=None,
            detail="missing",
        )
        satisfied = csc.Finding(
            source_path="src.cpp",
            requirement=csc.Requirement("compat-json", "html", None, None),
            resolved_prefix="html",
            satisfied=True,
            bypass_reason=None,
            detail="ok",
        )
        doc = csc.Finding(
            source_path="src.cpp",
            requirement=csc.Requirement("doc", None, "docs/css.md", None),
            resolved_prefix="css",
            satisfied=False,
            bypass_reason=None,
            detail="missing",
        )
        with tempfile.TemporaryDirectory() as td:
            path = pathlib.Path(td) / "compat.json"
            compat_data = {"html": {}}
            added = csc.apply_stubs([missing, satisfied, doc], compat_data, path)
            data = json.loads(path.read_text(encoding="utf-8"))

        self.assertEqual(added, ["css"])
        self.assertEqual(data, {"html": {}, "css": {}})

    def test_apply_stubs_noops_for_present_or_unknown_prefixes(self) -> None:
        present = csc.Finding(
            source_path="src.cpp",
            requirement=csc.Requirement("compat-json", "css", None, None),
            resolved_prefix="css",
            satisfied=False,
            bypass_reason=None,
            detail="missing",
        )
        unknown = csc.Finding(
            source_path="src.cpp",
            requirement=csc.Requirement("compat-json", "bogus", None, None),
            resolved_prefix="bogus",
            satisfied=False,
            bypass_reason=None,
            detail="missing",
        )
        with tempfile.TemporaryDirectory() as td:
            path = pathlib.Path(td) / "compat.json"
            added = csc.apply_stubs([present, unknown], {"css": {}}, path)

        self.assertEqual(added, [])
        self.assertFalse(path.exists())


class RenderAndMainTests(unittest.TestCase):
    def _finding(self, *, satisfied: bool, bypass: str | None = None) -> csc.Finding:
        return csc.Finding(
            source_path="src.cpp",
            requirement=csc.Requirement("doc", None, "docs/css.md", None),
            resolved_prefix="css",
            satisfied=satisfied,
            bypass_reason=bypass,
            detail="doc docs/css.md NOT updated",
        )

    def test_render_report_no_findings_and_hint_mode(self) -> None:
        text, code = csc.render_report([], "hint", enforce=True)
        self.assertEqual(code, 0)
        self.assertIn("nothing to verify", text)

    def test_render_report_hard_fail_advisory_and_bypass(self) -> None:
        text, code = csc.render_report([self._finding(satisfied=False)], "report", True)
        self.assertEqual(code, 1)
        self.assertIn("Compat-sync check FAILED", text)
        self.assertIn('Compat-Update: skip prefix=css reason="..."', text)

        text, code = csc.render_report([self._finding(satisfied=False)], "report", False)
        self.assertEqual(code, 0)
        self.assertIn("advisory", text)

        text, code = csc.render_report(
            [self._finding(satisfied=False, bypass="mechanical rename")],
            "report",
            True,
        )
        self.assertEqual(code, 0)
        self.assertIn("bypassed", text)

    def test_render_report_hint_lists_rows_without_failure_footer(self) -> None:
        text, code = csc.render_report([self._finding(satisfied=False)], "hint", True)
        self.assertEqual(code, 0)
        self.assertIn("doc docs/css.md NOT updated", text)
        self.assertNotIn("Compat-sync check FAILED", text)

    def test_render_report_dedupes_skip_suggestions_by_prefix(self) -> None:
        css_doc = self._finding(satisfied=False)
        css_test = csc.Finding(
            source_path="src.cpp",
            requirement=csc.Requirement("test", None, None, "test/test_css*.cpp"),
            resolved_prefix="css",
            satisfied=False,
            bypass_reason=None,
            detail="no test file matching test/test_css*.cpp",
        )
        html_doc = csc.Finding(
            source_path="html.cpp",
            requirement=csc.Requirement("doc", None, "docs/html.md", None),
            resolved_prefix="html",
            satisfied=False,
            bypass_reason=None,
            detail="doc docs/html.md NOT updated",
        )

        text, code = csc.render_report([css_doc, css_test, html_doc], "report", True)

        self.assertEqual(code, 1)
        self.assertEqual(text.count('Compat-Update: skip prefix=css reason="..."'), 1)
        self.assertEqual(text.count('Compat-Update: skip prefix=html reason="..."'), 1)

    def test_main_reports_repo_and_config_errors(self) -> None:
        stderr = io.StringIO()
        with mock.patch.object(
            csc,
            "repo_root",
            side_effect=subprocess.CalledProcessError(128, ["git"]),
        ), contextlib.redirect_stderr(stderr):
            self.assertEqual(csc.main([]), 2)
        self.assertIn("not in a git repo", stderr.getvalue())

        stderr = io.StringIO()
        with tempfile.TemporaryDirectory() as td, contextlib.redirect_stderr(stderr):
            rc = csc.main(["--repo-root", td, "--config", str(pathlib.Path(td) / "missing.json")])
        self.assertEqual(rc, 2)
        self.assertIn("config not found", stderr.getvalue())

    def test_main_self_check_and_git_diff_error_paths(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            cfg = root / "compat_path_map.json"
            compat_json = root / "compat.json"
            cfg.write_text(
                json.dumps(
                    {
                        "paths": {
                            "src.cpp": [
                                {"kind": "compat-json", "prefix": "bogus"},
                            ]
                        }
                    }
                ),
                encoding="utf-8",
            )
            compat_json.write_text("{}", encoding="utf-8")

            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                rc = csc.main(
                    [
                        "--repo-root",
                        str(root),
                        "--config",
                        str(cfg),
                        "--compat-json",
                        str(compat_json),
                        "--mode",
                        "report",
                        "--enforce",
                    ]
                )
            self.assertEqual(rc, 1)
            self.assertIn("bogus", stderr.getvalue())

            cfg.write_text('{"paths": {}}', encoding="utf-8")
            with mock.patch.object(
                csc,
                "git_diff_names",
                side_effect=subprocess.CalledProcessError(128, ["git"]),
            ):
                stderr = io.StringIO()
                with contextlib.redirect_stderr(stderr):
                    rc = csc.main(
                        [
                            "--repo-root",
                            str(root),
                            "--config",
                            str(cfg),
                            "--compat-json",
                            str(compat_json),
                        ]
                    )
            self.assertEqual(rc, 2)
            self.assertIn("git diff against", stderr.getvalue())

    def test_main_self_check_warning_continues_when_not_enforced(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            cfg = root / "compat_path_map.json"
            compat_json = root / "compat.json"
            cfg.write_text('{"paths": {}}', encoding="utf-8")
            compat_json.write_text('{"css": {}, "made-up": {}}', encoding="utf-8")

            stdout = io.StringIO()
            stderr = io.StringIO()
            with mock.patch.object(csc, "git_diff_names", return_value=[]), \
                 mock.patch.object(csc, "git_range_trailers", return_value={}), \
                 contextlib.redirect_stdout(stdout), \
                 contextlib.redirect_stderr(stderr):
                rc = csc.main(
                    [
                        "--repo-root",
                        str(root),
                        "--config",
                        str(cfg),
                        "--compat-json",
                        str(compat_json),
                        "--mode",
                        "report",
                    ]
                )

        self.assertEqual(rc, 0)
        self.assertIn("made-up", stderr.getvalue())
        self.assertIn("nothing to verify", stdout.getvalue())

    def test_main_apply_mode_blocks_self_check_errors(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            cfg = root / "compat_path_map.json"
            compat_json = root / "compat.json"
            cfg.write_text(
                json.dumps({"paths": {"src.cpp": [{"kind": "test"}]}}),
                encoding="utf-8",
            )
            compat_json.write_text("{}", encoding="utf-8")

            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                rc = csc.main(
                    [
                        "--repo-root",
                        str(root),
                        "--config",
                        str(cfg),
                        "--compat-json",
                        str(compat_json),
                        "--mode",
                        "apply",
                    ]
                )

        self.assertEqual(rc, 1)
        self.assertIn("no glob", stderr.getvalue())

    def test_main_env_enforcement_hard_fails_report_mode(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            cfg = root / "compat_path_map.json"
            compat_json = root / "compat.json"
            cfg.write_text(
                json.dumps(
                    {
                        "paths": {
                            "src.cpp": [
                                {"kind": "compat-json", "prefix": "css"},
                                {"kind": "doc", "path": "docs/css.md"},
                            ]
                        }
                    }
                ),
                encoding="utf-8",
            )
            compat_json.write_text('{"css": {}}', encoding="utf-8")

            stdout = io.StringIO()
            with mock.patch.object(csc, "git_diff_names", return_value=["src.cpp"]), \
                 mock.patch.object(csc, "git_range_trailers", return_value={}), \
                 mock.patch.dict(csc.os.environ, {"PULP_ENFORCE_PREPUSH": "1"}), \
                 contextlib.redirect_stdout(stdout):
                rc = csc.main(
                    [
                        "--repo-root",
                        str(root),
                        "--config",
                        str(cfg),
                        "--compat-json",
                        str(compat_json),
                        "--mode",
                        "report",
                    ]
                )

        self.assertEqual(rc, 1)
        self.assertIn("Compat-sync check FAILED", stdout.getvalue())

    def test_main_apply_mode_noops_when_no_stub_sections_needed(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            cfg = root / "compat_path_map.json"
            compat_json = root / "compat.json"
            cfg.write_text(
                json.dumps(
                    {
                        "paths": {
                            "src.cpp": [
                                {"kind": "compat-json", "prefix": "css"},
                                {"kind": "doc", "path": "docs/css.md"},
                            ]
                        }
                    }
                ),
                encoding="utf-8",
            )
            compat_json.write_text('{"css": {}}', encoding="utf-8")

            stdout = io.StringIO()
            with mock.patch.object(csc, "git_diff_names", return_value=["src.cpp"]), \
                 mock.patch.object(csc, "git_range_trailers", return_value={}), \
                 contextlib.redirect_stdout(stdout):
                rc = csc.main(
                    [
                        "--repo-root",
                        str(root),
                        "--config",
                        str(cfg),
                        "--compat-json",
                        str(compat_json),
                        "--mode",
                        "apply",
                    ]
                )

        self.assertEqual(rc, 0)
        self.assertNotIn("added stub sections", stdout.getvalue())
        self.assertIn("Compat-sync check FAILED", stdout.getvalue())

    def test_main_apply_mode_prints_added_sections_and_reevaluates(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            cfg = root / "compat_path_map.json"
            compat_json = root / "compat.json"
            cfg.write_text(
                json.dumps(
                    {
                        "paths": {
                            "src.cpp": [
                                {"kind": "compat-json", "prefix": "css"},
                                {"kind": "doc", "path": "docs/css.md"},
                            ]
                        }
                    }
                ),
                encoding="utf-8",
            )
            compat_json.write_text("{}", encoding="utf-8")

            stdout = io.StringIO()
            with mock.patch.object(csc, "git_diff_names", return_value=["src.cpp"]), \
                 mock.patch.object(csc, "git_range_trailers", return_value={}), \
                 contextlib.redirect_stdout(stdout):
                rc = csc.main(
                    [
                        "--repo-root",
                        str(root),
                        "--config",
                        str(cfg),
                        "--compat-json",
                        str(compat_json),
                        "--mode",
                        "apply",
                    ]
                )

        self.assertEqual(rc, 0)
        self.assertIn("added stub sections", stdout.getvalue())
        self.assertIn("compat.json modified in diff", stdout.getvalue())

    def test_main_skips_empty_report_text_and_hint_returns_zero(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            cfg = root / "compat_path_map.json"
            compat_json = root / "compat.json"
            cfg.write_text('{"paths": {}}', encoding="utf-8")
            compat_json.write_text("{}", encoding="utf-8")

            stdout = io.StringIO()
            with mock.patch.object(csc, "git_diff_names", return_value=[]), \
                 mock.patch.object(csc, "git_range_trailers", return_value={}), \
                 mock.patch.object(csc, "render_report", return_value=("", 7)), \
                 contextlib.redirect_stdout(stdout):
                rc = csc.main(
                    [
                        "--repo-root",
                        str(root),
                        "--config",
                        str(cfg),
                        "--compat-json",
                        str(compat_json),
                        "--mode",
                        "hint",
                    ]
                )

        self.assertEqual(rc, 0)
        self.assertEqual(stdout.getvalue(), "")

    def test_main_uses_default_relpath_for_outside_compat_json(self) -> None:
        with tempfile.TemporaryDirectory() as td, tempfile.TemporaryDirectory() as outside:
            root = pathlib.Path(td)
            cfg = root / "compat_path_map.json"
            compat_json = pathlib.Path(outside) / "compat.json"
            cfg.write_text('{"paths": {}}', encoding="utf-8")
            compat_json.write_text('{"css": {}}', encoding="utf-8")

            stdout = io.StringIO()
            with mock.patch.object(csc, "git_diff_names", return_value=[]), \
                 mock.patch.object(csc, "git_range_trailers", return_value={}), \
                 contextlib.redirect_stdout(stdout):
                rc = csc.main(
                    [
                        "--repo-root",
                        str(root),
                        "--config",
                        str(cfg),
                        "--compat-json",
                        str(compat_json),
                    ]
                )

        self.assertEqual(rc, 0)
        self.assertIn("nothing to verify", stdout.getvalue())

    def test_script_entrypoint_exits_with_main_result(self) -> None:
        script = pathlib.Path(csc.__file__)
        stderr = io.StringIO()
        with mock.patch.object(sys, "argv", [str(script), "--mode", "hint"]), \
             mock.patch.object(csc.subprocess, "run") as run, \
             contextlib.redirect_stderr(stderr):
            run.side_effect = [
                subprocess.CompletedProcess([], 0, stdout="/repo\n"),
                subprocess.CompletedProcess([], 0, stdout=""),
                subprocess.CompletedProcess([], 0, stdout=""),
            ]
            with self.assertRaises(SystemExit) as cm:
                runpy.run_path(str(script), run_name="__main__")

        self.assertEqual(cm.exception.code, 2)


if __name__ == "__main__":
    unittest.main()

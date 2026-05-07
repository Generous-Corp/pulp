#!/usr/bin/env python3
"""Additional coverage-lane tests for version_bump_check.py."""

from __future__ import annotations

import contextlib
import io
import json
import os
import pathlib
import runpy
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import version_bump_check as vbc  # noqa: E402


class VersionFileIoTests(unittest.TestCase):
    def test_read_and_write_supported_version_file_kinds(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            repo = pathlib.Path(td)
            files = {
                "CMakeLists.txt": 'project(Pulp VERSION 1.2.3)\n',
                "package.json": '{"version": "1.2.3"}\n',
                "pyproject.toml": 'name = "pulp"\nversion = "1.2.3"\n',
                "module.py": '__version__ = "1.2.3"\n',
                "plugin.json": '"pluginVersion": "1.2.3"\n',
            }
            for rel, text in files.items():
                (repo / rel).write_text(text, encoding="utf-8")

            vfs = [
                vbc.VersionFile("CMakeLists.txt", "cmake_project_version"),
                vbc.VersionFile("package.json", "json_field", "version"),
                vbc.VersionFile("pyproject.toml", "pyproject_version"),
                vbc.VersionFile("module.py", "python_dunder_version"),
                vbc.VersionFile("plugin.json", "regex", pattern=r'"pluginVersion": "([^"]+)"'),
            ]

            for vf in vfs:
                with self.subTest(vf=vf):
                    self.assertEqual(vbc.read_version(repo, vf), "1.2.3")
                    self.assertTrue(vbc.write_version(repo, vf, "2.0.0"))
                    self.assertEqual(vbc.read_version(repo, vf), "2.0.0")
                    self.assertFalse(vbc.write_version(repo, vf, "2.0.0"))

            self.assertFalse(vbc.write_version(repo, vbc.VersionFile("missing", "json_field", "version"), "1.0.0"))
            self.assertIsNone(vbc.read_version(repo, vbc.VersionFile("missing", "json_field", "version")))

    def test_json_decode_and_unknown_version_kinds_are_tolerated(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            repo = pathlib.Path(td)
            (repo / "bad.json").write_text("{bad", encoding="utf-8")
            (repo / "plain.txt").write_text("version=1.0.0", encoding="utf-8")

            self.assertIsNone(vbc.read_version(repo, vbc.VersionFile("bad.json", "json_field", "version")))
            self.assertFalse(vbc.write_version(repo, vbc.VersionFile("bad.json", "json_field", "version"), "1.0.0"))
            self.assertIsNone(vbc.read_version(repo, vbc.VersionFile("plain.txt", "unknown")))
            self.assertFalse(vbc.write_version(repo, vbc.VersionFile("plain.txt", "unknown"), "1.0.0"))

    def test_script_entrypoint_help_exits_zero(self) -> None:
        script = pathlib.Path(vbc.__file__).resolve()
        with mock.patch.object(sys, "argv", [str(script), "--help"]), \
             contextlib.redirect_stdout(io.StringIO()):
            with self.assertRaises(SystemExit) as ctx:
                runpy.run_path(str(script), run_name="__main__")

        self.assertEqual(ctx.exception.code, 0)


class GitAndHeuristicTests(unittest.TestCase):
    def test_repo_root_glob_conventional_and_bump_edges(self) -> None:
        with mock.patch.object(
            vbc.subprocess,
            "run",
            return_value=subprocess.CompletedProcess([], 0, stdout="/tmp/pulp\n"),
        ):
            self.assertEqual(vbc.repo_root(), pathlib.Path("/tmp/pulp"))

        self.assertTrue(vbc._glob_match("src/name.txt", "src/*.txt"))
        self.assertEqual(vbc.classify_conventional("BREAKING: api change"), "major")
        self.assertEqual(vbc.classify_conventional("not conventional"), "none")
        self.assertEqual(vbc.classify_conventional("docs: update"), "none")
        self.assertEqual(vbc.bump_version("1.2.3", "patch"), "1.2.4")
        self.assertEqual(vbc.bump_version("1.2.3", "none"), "1.2.3")

    def test_git_helpers_parse_outputs_and_tolerate_bad_trailer_ranges(self) -> None:
        def fake_run(cmd: list[str], **kwargs) -> subprocess.CompletedProcess[str]:
            if cmd[:2] == ["git", "diff"] and "--name-only" in cmd:
                return subprocess.CompletedProcess(cmd, 0, stdout="a.cpp\n\nb.cpp\n")
            if cmd[:2] == ["git", "log"] and "--format=%H%x00%s%x00%B%x01" in cmd:
                return subprocess.CompletedProcess(cmd, 0, stdout="abc\0feat: hi\0body\x01")
            if cmd[:2] == ["git", "show"] and "--name-only" in cmd:
                return subprocess.CompletedProcess(cmd, 0, stdout="a.cpp\nb.cpp\n")
            if cmd[:2] == ["git", "log"] and "--format=%B%x00" in cmd:
                raise subprocess.CalledProcessError(128, cmd)
            raise AssertionError(cmd)

        with mock.patch.object(vbc.subprocess, "run", side_effect=fake_run):
            self.assertEqual(vbc.git_diff_names("base", "head"), ["a.cpp", "b.cpp"])
            self.assertEqual(vbc.git_log_subjects_and_bodies("base", "head"), [("abc", "feat: hi", "body")])
            self.assertEqual(vbc.git_commit_files("abc"), ["a.cpp", "b.cpp"])
            self.assertEqual(vbc.git_range_trailers("bad", "head"), {})

    def test_trailer_helpers_parse_valid_lines_and_skip_malformed_chunks(self) -> None:
        def fake_run(cmd: list[str], **kwargs) -> subprocess.CompletedProcess[str]:
            if cmd[:2] == ["git", "log"] and "--format=%H%x00%s%x00%B%x01" in cmd:
                return subprocess.CompletedProcess(
                    cmd,
                    0,
                    stdout="malformed\x01abc\0feat: scoped\0body\x01",
                )
            if cmd[:2] == ["git", "log"] and "--format=%B%x00" in cmd:
                return subprocess.CompletedProcess(cmd, 0, stdout="subject\n\nVersion-Bump: sdk=minor reason=\"x\"\x00")
            if cmd[:3] == ["git", "log", "-1"]:
                if cmd[-1] == "bad":
                    raise subprocess.CalledProcessError(128, cmd)
                return subprocess.CompletedProcess(cmd, 0, stdout="body\n\nVersion-Bump: sdk=patch reason=\"x\"\n")
            if cmd[:2] == ["git", "interpret-trailers"]:
                return subprocess.CompletedProcess(
                    cmd,
                    0,
                    stdout="Version-Bump: sdk=patch reason=\"x\"\nnot a trailer\nSkill-Update: skip skill=ci reason=\"x\"\n",
                )
            raise AssertionError(cmd)

        with mock.patch.object(vbc.subprocess, "run", side_effect=fake_run):
            self.assertEqual(vbc.git_log_subjects_and_bodies("base", "head"), [("abc", "feat: scoped", "body")])
            self.assertEqual(
                vbc.git_range_trailers("base", "head"),
                {
                    "version-bump": ['sdk=patch reason="x"'],
                    "skill-update": ['skip skill=ci reason="x"'],
                },
            )
            self.assertEqual(
                vbc.git_commit_trailers("HEAD"),
                {
                    "version-bump": ['sdk=patch reason="x"'],
                    "skill-update": ['skip skill=ci reason="x"'],
                },
            )
            self.assertEqual(vbc.git_commit_trailers("bad"), {})

    def test_git_diff_ignore_whitespace_filters_comments_and_blanks(self) -> None:
        cases = [
            ("", False),
            ("diff\n+   \n-   \n", False),
            ("diff\n+// comment\n-# comment\n+* comment\n", False),
            ("diff\n+++ b/file\n--- a/file\n+int x = 1;\n", True),
        ]
        for stdout, expected in cases:
            with self.subTest(stdout=stdout):
                with mock.patch.object(
                    vbc.subprocess,
                    "run",
                    return_value=subprocess.CompletedProcess([], 0, stdout=stdout),
                ):
                    self.assertEqual(
                        vbc.git_diff_ignore_whitespace_nonempty("base", "head", "file"),
                        expected,
                    )

    def test_heuristic_and_conventional_helpers(self) -> None:
        surface = vbc.Surface(
            name="sdk",
            label="SDK",
            version_files=[],
            trigger_paths=["include/**", "src/**"],
            public_api_paths=["include/**"],
            internal_only_paths=["src/**"],
        )
        with mock.patch.object(vbc, "git_diff_ignore_whitespace_nonempty", return_value=True):
            self.assertEqual(vbc.heuristic_for_surface(surface, ["include/api.hpp"], "base", "head"), "minor")
            self.assertEqual(vbc.heuristic_for_surface(surface, ["src/impl.cpp"], "base", "head"), "patch")
        with mock.patch.object(vbc, "git_diff_ignore_whitespace_nonempty", return_value=False):
            self.assertEqual(vbc.heuristic_for_surface(surface, ["include/api.hpp"], "base", "head"), "none")
        self.assertEqual(vbc.heuristic_for_surface(surface, ["docs/readme.md"], "base", "head"), "none")
        self.assertTrue(vbc.is_revert_commit("Revert \"feat\"", {}))
        self.assertTrue(vbc.is_revert_commit("fix: x", {"revert-of": ["abc"]}))
        self.assertEqual(vbc.classify_conventional("feat(api): add x"), "minor")
        self.assertEqual(vbc.classify_conventional("fix!: break x"), "major")
        self.assertEqual(vbc.max_level("patch", "minor", "none"), "minor")

    def test_glob_trailer_and_version_parser_edges(self) -> None:
        self.assertTrue(vbc._glob_match("anything/here", "**"))
        self.assertTrue(vbc._glob_match("src/a.cpp", "src/?.cpp"))
        self.assertFalse(vbc._glob_match("src/ab.cpp", "src/?.cpp"))
        self.assertTrue(vbc._glob_match("docs", "docs/**"))
        self.assertTrue(vbc._glob_match("docs/reference/index.md", "docs/**"))
        self.assertFalse(vbc._glob_match("file.txt", "**/**/file.txt"))
        self.assertTrue(vbc._glob_match("docs", "docs//**"))
        self.assertTrue(vbc._glob_match("docs/reference/index.md", "docs//**/index.md"))

        trailers = {
            "version-bump": [
                'sdk=none reason="not accepted"',
                'sdk=bogus reason="not accepted"',
                'plugin=minor reason="unrelated"',
                'sdk=MAJOR reason="accepted"',
            ],
        }
        self.assertEqual(vbc.surface_trailer_override(trailers, "Version-Bump", "sdk"), "major")
        self.assertIsNone(vbc.surface_trailer_override(trailers, "Version-Bump", "docs"))
        self.assertEqual(vbc.bump_version("not-semver", "minor"), "not-semver")
        self.assertEqual(vbc.bump_version("1.2.3", "major"), "2.0.0")

        self.assertEqual(
            vbc._extract_version_from_text('project(Pulp VERSION 1.2.3)\n', vbc.VersionFile("CMakeLists.txt", "cmake_project_version")),
            "1.2.3",
        )
        self.assertIsNone(
            vbc._extract_version_from_text("{bad", vbc.VersionFile("bad.json", "json_field", "version"))
        )
        self.assertEqual(
            vbc._extract_version_from_text('version = "1.2.3"\n', vbc.VersionFile("pyproject.toml", "pyproject_version")),
            "1.2.3",
        )
        self.assertEqual(
            vbc._extract_version_from_text('__version__ = "1.2.3"\n', vbc.VersionFile("module.py", "python_dunder_version")),
            "1.2.3",
        )
        self.assertIsNone(vbc._extract_version_from_text("version=1", vbc.VersionFile("plain.txt", "regex")))

    def test_assess_surfaces_honors_explicit_override_and_scopes_conventional_commits(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            repo = pathlib.Path(td)
            (repo / "second.json").write_text('{"version": "2.0.0"}\n', encoding="utf-8")
            surface = vbc.Surface(
                "sdk",
                "SDK",
                [
                    vbc.VersionFile("missing.json", "json_field", "version"),
                    vbc.VersionFile("second.json", "json_field", "version"),
                ],
                ["src/**"],
                public_api_paths=["src/**"],
            )
            cfg = vbc.Config([surface], [], "Version-Bump")

            with mock.patch.object(vbc, "git_diff_ignore_whitespace_nonempty", return_value=True), \
                 mock.patch.object(vbc, "git_range_trailers", return_value={"version-bump": ['sdk=patch reason="bug fix"']}):
                verdict = vbc.assess_surfaces(cfg, ["src/api.hpp"], "base", "head", repo)[0]

            self.assertEqual(verdict.heuristic, "minor")
            self.assertEqual(verdict.trailer_override, "patch")
            self.assertEqual(verdict.final_level, "patch")
            self.assertEqual(verdict.current_version, "2.0.0")

        internal_surface = vbc.Surface(
            "sdk",
            "SDK",
            [vbc.VersionFile("sdk.json", "json_field", "version")],
            ["src/**"],
            internal_only_paths=["src/**"],
        )
        with tempfile.TemporaryDirectory() as td:
            repo = pathlib.Path(td)
            (repo / "sdk.json").write_text('{"version": "1.0.0"}\n', encoding="utf-8")
            with mock.patch.object(vbc, "git_diff_ignore_whitespace_nonempty", return_value=True), \
                 mock.patch.object(vbc, "git_range_trailers", return_value={}), \
                 mock.patch.object(
                     vbc,
                     "git_log_subjects_and_bodies",
                     return_value=[
                         ("revert", 'Revert "feat: add api"', ""),
                         ("unrelated", "feat: plugin thing", ""),
                         ("related", "perf: speed path", ""),
                     ],
                 ), \
                 mock.patch.object(
                     vbc,
                     "git_commit_files",
                     side_effect=lambda sha: {
                         "unrelated": ["docs/readme.md"],
                         "related": ["src/impl.cpp"],
                     }[sha],
                 ):
                verdict = vbc.assess_surfaces(
                    vbc.Config([internal_surface], [], "Version-Bump"),
                    ["src/impl.cpp"],
                    "base",
                    "head",
                    repo,
                )[0]

            self.assertEqual(verdict.heuristic, "patch")
            self.assertEqual(verdict.final_level, "patch")

        unreadable_surface = vbc.Surface(
            "sdk",
            "SDK",
            [vbc.VersionFile("missing.json", "json_field", "version")],
            ["src/**"],
        )
        with tempfile.TemporaryDirectory() as td:
            with mock.patch.object(vbc, "git_range_trailers", return_value={}):
                verdict = vbc.assess_surfaces(
                    vbc.Config([unreadable_surface], [], "Version-Bump"),
                    [],
                    "base",
                    "head",
                    pathlib.Path(td),
                )[0]
        self.assertEqual(verdict.final_level, "none")
        self.assertIsNone(verdict.current_version)

    def test_assess_surfaces_override_and_conventional_edges(self) -> None:
        def cfg_for(repo: pathlib.Path, surface: vbc.Surface) -> vbc.Config:
            (repo / "sdk.json").write_text('{"version": "1.0.0"}\n', encoding="utf-8")
            return vbc.Config([surface], [], "Version-Bump")

        with tempfile.TemporaryDirectory() as td:
            repo = pathlib.Path(td)
            surface = vbc.Surface(
                "sdk",
                "SDK",
                [vbc.VersionFile("sdk.json", "json_field", "version")],
                ["src/**"],
                public_api_paths=["src/**"],
            )
            with mock.patch.object(vbc, "git_diff_ignore_whitespace_nonempty", return_value=True), \
                 mock.patch.object(vbc, "git_range_trailers", return_value={"version-bump": ['sdk=skip reason="x"']}):
                verdict = vbc.assess_surfaces(cfg_for(repo, surface), ["src/api.hpp"], "base", "head", repo)[0]
            self.assertEqual(verdict.trailer_override, "skip")
            self.assertEqual(verdict.final_level, "none")

        with tempfile.TemporaryDirectory() as td:
            repo = pathlib.Path(td)
            surface = vbc.Surface(
                "sdk",
                "SDK",
                [vbc.VersionFile("sdk.json", "json_field", "version")],
                ["src/**"],
                public_api_paths=["src/**"],
            )
            with mock.patch.object(vbc, "git_range_trailers", return_value={"version-bump": ['sdk=minor reason="x"']}):
                verdict = vbc.assess_surfaces(cfg_for(repo, surface), ["docs/readme.md"], "base", "head", repo)[0]
            self.assertEqual(verdict.trailer_override, "minor")
            self.assertEqual(verdict.final_level, "none")

        with tempfile.TemporaryDirectory() as td:
            repo = pathlib.Path(td)
            surface = vbc.Surface(
                "sdk",
                "SDK",
                [vbc.VersionFile("sdk.json", "json_field", "version")],
                ["src/**"],
                public_api_paths=["src/**"],
            )
            with mock.patch.object(vbc, "git_diff_ignore_whitespace_nonempty", return_value=False), \
                 mock.patch.object(vbc, "git_range_trailers", return_value={"version-bump": ['sdk=minor reason="x"']}):
                verdict = vbc.assess_surfaces(cfg_for(repo, surface), ["src/api.hpp"], "base", "head", repo)[0]
            self.assertEqual(verdict.heuristic, "none")
            self.assertEqual(verdict.final_level, "minor")

        with tempfile.TemporaryDirectory() as td:
            repo = pathlib.Path(td)
            surface = vbc.Surface(
                "sdk",
                "SDK",
                [vbc.VersionFile("sdk.json", "json_field", "version")],
                ["src/**"],
                internal_only_paths=["src/**"],
            )
            with mock.patch.object(vbc, "git_diff_ignore_whitespace_nonempty", return_value=True), \
                 mock.patch.object(vbc, "git_range_trailers", return_value={}), \
                 mock.patch.object(vbc, "git_log_subjects_and_bodies", return_value=[("sha", "feat: api", "")]), \
                 mock.patch.object(vbc, "git_commit_files", return_value=["src/impl.cpp"]):
                verdict = vbc.assess_surfaces(cfg_for(repo, surface), ["src/impl.cpp"], "base", "head", repo)[0]
            self.assertEqual(verdict.heuristic, "patch")
            self.assertEqual(verdict.final_level, "minor")

    def test_already_bumped_false_edges(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            repo = pathlib.Path(td)
            vf = vbc.VersionFile("sdk.json", "json_field", "version")
            self.assertFalse(vbc.already_bumped("base", vf, repo))

            (repo / "sdk.json").write_text('{"version": "1.2.3"}\n', encoding="utf-8")
            with mock.patch.object(vbc, "version_at_base", return_value=None):
                self.assertFalse(vbc.already_bumped("base", vf, repo))

            (repo / "sdk.json").write_text('{"name": "pulp"}\n', encoding="utf-8")
            with mock.patch.object(vbc, "version_at_base", return_value="1.2.3"):
                self.assertFalse(vbc.already_bumped("base", vf, repo))


class AssessmentReportApplyTests(unittest.TestCase):
    def test_version_at_base_and_already_bumped(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            repo = pathlib.Path(td)
            (repo / "package.json").write_text('{"version": "1.2.4"}\n', encoding="utf-8")
            vf = vbc.VersionFile("package.json", "json_field", "version")
            with mock.patch.object(
                vbc.subprocess,
                "run",
                return_value=subprocess.CompletedProcess([], 0, stdout='{"version": "1.2.3"}\n'),
            ):
                self.assertEqual(vbc.version_at_base("base", vf), "1.2.3")
                self.assertTrue(vbc.already_bumped("base", vf, repo))

        with mock.patch.object(
            vbc.subprocess,
            "run",
            side_effect=subprocess.CalledProcessError(128, ["git"]),
        ):
            self.assertIsNone(vbc.version_at_base("base", vf))

    def test_render_report_flags_partial_required_and_patch_advisory(self) -> None:
        surface = vbc.Surface(
            "plugin",
            "Plugin",
            [
                vbc.VersionFile("one.json", "json_field", "version"),
                vbc.VersionFile("two.json", "json_field", "version"),
            ],
            ["src/**"],
        )
        with tempfile.TemporaryDirectory() as td:
            repo = pathlib.Path(td)
            (repo / "one.json").write_text('{"version": "1.2.4"}', encoding="utf-8")
            (repo / "two.json").write_text('{"version": "1.2.3"}', encoding="utf-8")
            verdict = vbc.Verdict(surface, "minor", None, "1.2.4", "minor")

            with mock.patch.object(
                vbc,
                "version_at_base",
                side_effect=lambda _base, vf: "1.2.3",
            ):
                text, code = vbc.render_report([verdict], "report", "base", repo)

        self.assertEqual(code, 1)
        self.assertIn("partial bump", text)
        self.assertIn("two.json", text)

        patch_surface = vbc.Surface("sdk", "SDK", [vbc.VersionFile("sdk.json", "json_field", "version")], ["src/**"])
        with tempfile.TemporaryDirectory() as td:
            repo = pathlib.Path(td)
            (repo / "sdk.json").write_text('{"version": "1.2.3"}', encoding="utf-8")
            verdict = vbc.Verdict(patch_surface, "patch", "patch", "1.2.3", "patch")
            with mock.patch.object(vbc, "version_at_base", return_value="1.2.3"):
                text, code = vbc.render_report([verdict], "report", "base", repo)
        self.assertEqual(code, 0)
        self.assertIn("bump suggested", text)

    def test_render_report_no_bump_all_bumped_required_and_hint_paths(self) -> None:
        surface = vbc.Surface(
            "sdk",
            "SDK",
            [vbc.VersionFile("sdk.json", "json_field", "version")],
            ["src/**"],
        )
        verdict = vbc.Verdict(surface, "none", None, "1.2.3", "none")
        text, code = vbc.render_report([verdict], "report", "base", pathlib.Path("/tmp"))
        self.assertEqual(code, 0)
        self.assertIn("no bump needed", text)

        verdict = vbc.Verdict(surface, "minor", None, "1.2.3", "minor")
        with mock.patch.object(vbc, "already_bumped", return_value=True):
            text, code = vbc.render_report([verdict], "report", "base", pathlib.Path("/tmp"))
        self.assertEqual(code, 0)
        self.assertIn("bumped", text)

        with mock.patch.object(vbc, "already_bumped", return_value=False):
            text, code = vbc.render_report([verdict], "report", "base", pathlib.Path("/tmp"))
        self.assertEqual(code, 1)
        self.assertIn("bump required", text)
        self.assertIn("Version-bump check FAILED", text)

        with mock.patch.object(vbc, "already_bumped", return_value=False):
            text, code = vbc.render_report([verdict], "hint", "base", pathlib.Path("/tmp"))
        self.assertEqual(code, 0)
        self.assertIn("bump required", text)

    def test_apply_bumps_uses_base_version_and_stages_changes(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            repo = pathlib.Path(td)
            (repo / "package.json").write_text('{"version": "1.2.4"}\n', encoding="utf-8")
            (repo / "CHANGELOG.md").write_text("# Changelog\n\n## [1.2.3]\n", encoding="utf-8")
            surface = vbc.Surface(
                "plugin",
                "Plugin",
                [vbc.VersionFile("package.json", "json_field", "version")],
                ["src/**"],
                changelog="CHANGELOG.md",
            )
            verdict = vbc.Verdict(surface, "minor", None, "1.2.4", "minor")
            with mock.patch.object(vbc, "already_bumped", return_value=False), \
                 mock.patch.object(vbc, "version_at_base", return_value="1.2.3"), \
                 mock.patch.object(vbc.subprocess, "run") as run:
                edited = vbc.apply_bumps([verdict], "base", repo)

            self.assertEqual(edited, ["package.json", "CHANGELOG.md"])
            self.assertEqual(json.loads((repo / "package.json").read_text())["version"], "1.3.0")
            self.assertIn("## [1.3.0]", (repo / "CHANGELOG.md").read_text())
            run.assert_called_once()

    def test_apply_bumps_skip_and_fallback_edges(self) -> None:
        surface = vbc.Surface(
            "sdk",
            "SDK",
            [vbc.VersionFile("sdk.json", "json_field", "version")],
            ["src/**"],
        )
        with tempfile.TemporaryDirectory() as td:
            repo = pathlib.Path(td)
            (repo / "sdk.json").write_text('{"version": "1.2.4"}\n', encoding="utf-8")
            verdict = vbc.Verdict(surface, "minor", None, "1.2.4", "minor")
            with mock.patch.object(vbc, "already_bumped", return_value=True):
                self.assertEqual(vbc.apply_bumps([verdict], "base", repo), [])

            verdict = vbc.Verdict(surface, "minor", None, None, "minor")
            with mock.patch.object(vbc, "already_bumped", return_value=False):
                self.assertEqual(vbc.apply_bumps([verdict], "base", repo), [])

        with tempfile.TemporaryDirectory() as td:
            repo = pathlib.Path(td)
            (repo / "sdk.json").write_text('{"version": "2.0.0"}\n', encoding="utf-8")
            (repo / "CHANGELOG.md").write_text("# Changelog\n\nNo releases yet.\n", encoding="utf-8")
            changelog_surface = vbc.Surface(
                "sdk",
                "SDK",
                [vbc.VersionFile("sdk.json", "json_field", "version")],
                ["src/**"],
                changelog="CHANGELOG.md",
            )
            verdict = vbc.Verdict(changelog_surface, "minor", None, "2.0.0", "minor")
            with mock.patch.object(vbc, "already_bumped", return_value=False), \
                 mock.patch.object(vbc, "version_at_base", return_value=None), \
                 mock.patch.object(vbc.subprocess, "run") as run:
                edited = vbc.apply_bumps([verdict], "base", repo)

            self.assertEqual(edited, ["sdk.json", "CHANGELOG.md"])
            self.assertEqual(json.loads((repo / "sdk.json").read_text())["version"], "2.1.0")
            self.assertTrue((repo / "CHANGELOG.md").read_text().startswith("## [2.1.0]"))
            run.assert_called_once()

        with tempfile.TemporaryDirectory() as td:
            repo = pathlib.Path(td)
            (repo / "sdk.json").write_text('{"version": "1.0.0"}\n', encoding="utf-8")
            partial_surface = vbc.Surface(
                "sdk",
                "SDK",
                [
                    vbc.VersionFile("sdk.json", "json_field", "version"),
                    vbc.VersionFile("missing.json", "json_field", "version"),
                ],
                ["src/**"],
                changelog="MISSING_CHANGELOG.md",
            )
            verdict = vbc.Verdict(partial_surface, "minor", None, "1.0.0", "minor")
            with mock.patch.object(vbc, "already_bumped", return_value=False), \
                 mock.patch.object(vbc, "version_at_base", return_value="1.0.0"), \
                 mock.patch.object(vbc.subprocess, "run") as run:
                edited = vbc.apply_bumps([verdict], "base", repo)

            self.assertEqual(edited, ["sdk.json"])
            self.assertEqual(json.loads((repo / "sdk.json").read_text())["version"], "1.1.0")
            run.assert_called_once()

        with tempfile.TemporaryDirectory() as td:
            repo = pathlib.Path(td)
            (repo / "sdk.json").write_text('{"version": "1.0.0"}\n', encoding="utf-8")
            no_changelog_surface = vbc.Surface(
                "sdk",
                "SDK",
                [vbc.VersionFile("sdk.json", "json_field", "version")],
                ["src/**"],
            )
            verdict = vbc.Verdict(no_changelog_surface, "minor", None, "1.0.0", "minor")
            with mock.patch.object(vbc, "already_bumped", return_value=False), \
                 mock.patch.object(vbc, "version_at_base", return_value="1.0.0"), \
                 mock.patch.object(vbc.subprocess, "run") as run:
                edited = vbc.apply_bumps([verdict], "base", repo)

            self.assertEqual(edited, ["sdk.json"])
            self.assertEqual(json.loads((repo / "sdk.json").read_text())["version"], "1.1.0")
            run.assert_called_once()

    def test_apply_bumps_ignores_none_and_patch_verdicts(self) -> None:
        surface = vbc.Surface(
            "sdk",
            "SDK",
            [vbc.VersionFile("sdk.json", "json_field", "version")],
            ["src/**"],
        )
        verdicts = [
            vbc.Verdict(surface, "none", None, "1.0.0", "none"),
            vbc.Verdict(surface, "patch", None, "1.0.0", "patch"),
        ]
        with mock.patch.object(vbc, "already_bumped") as already_bumped:
            self.assertEqual(vbc.apply_bumps(verdicts, "base", pathlib.Path("/tmp")), [])
        already_bumped.assert_not_called()

    def test_fix_feat_skip_trailer_range_helper_requires_top_level_reason(self) -> None:
        with mock.patch.object(
            vbc,
            "git_range_trailers",
            return_value={"version-bump": ['sdk=skip reason="surface only"', 'skip reason="   "']},
        ):
            self.assertFalse(vbc._range_has_version_bump_skip_trailer("base", "head"))

        with mock.patch.object(
            vbc,
            "git_range_trailers",
            return_value={"version-bump": ['sdk=skip reason="surface only"', 'skip reason="test-only"']},
        ):
            self.assertTrue(vbc._range_has_version_bump_skip_trailer("base", "head"))

    def test_fix_feat_title_range_and_requirement_branches(self) -> None:
        self.assertTrue(vbc._is_fix_or_feat_title(" fix(scope)!: breaking"))
        self.assertFalse(vbc._is_fix_or_feat_title("docs: update"))

        with mock.patch.object(vbc, "git_log_subjects_and_bodies", return_value=[("sha", "docs: update", "")]):
            self.assertFalse(vbc._range_has_bump_commit("base", "head"))
        with mock.patch.object(
            vbc,
            "git_log_subjects_and_bodies",
            return_value=[("sha", "chore(versions): bump SDK", "")],
        ):
            self.assertTrue(vbc._range_has_bump_commit("base", "head"))

        passed, msg = vbc.check_fix_feat_requires_bump("", "base", "head")
        self.assertTrue(passed)
        self.assertIn("PR title not provided", msg)

        passed, msg = vbc.check_fix_feat_requires_bump("docs: update", "base", "head")
        self.assertTrue(passed)
        self.assertIn("no bump required", msg)

        with mock.patch.object(vbc, "_range_has_bump_commit", return_value=True):
            passed, msg = vbc.check_fix_feat_requires_bump("fix: bug", "base", "head")
        self.assertTrue(passed)
        self.assertIn("found `chore: bump versions`", msg)

        with mock.patch.object(vbc, "_range_has_bump_commit", return_value=False), \
             mock.patch.object(vbc, "_range_has_version_bump_skip_trailer", return_value=True):
            passed, msg = vbc.check_fix_feat_requires_bump("feat: feature", "base", "head")
        self.assertTrue(passed)
        self.assertIn("bypass honored", msg)

        with mock.patch.object(vbc, "_range_has_bump_commit", return_value=False), \
             mock.patch.object(vbc, "_range_has_version_bump_skip_trailer", return_value=False):
            passed, msg = vbc.check_fix_feat_requires_bump("fix: bug", "base", "head")
        self.assertFalse(passed)
        self.assertIn("contains NO commit", msg)


class MainTests(unittest.TestCase):
    def test_main_reports_missing_config(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                rc = vbc.main(["--repo-root", td, "--config", str(pathlib.Path(td) / "missing.json")])
        self.assertEqual(rc, 2)
        self.assertIn("config not found", stderr.getvalue())

    def test_script_entrypoint_reports_missing_config(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            missing = pathlib.Path(td) / "missing.json"
            env = os.environ.copy()
            env["PYTHONPATH"] = str(pathlib.Path(vbc.__file__).resolve().parent)
            proc = subprocess.run(
                [
                    sys.executable,
                    str(pathlib.Path(vbc.__file__).resolve()),
                    "--repo-root",
                    td,
                    "--config",
                    str(missing),
                ],
                capture_output=True,
                env=env,
                text=True,
            )
        self.assertEqual(proc.returncode, 2)
        self.assertIn("config not found", proc.stderr)

    def test_main_hint_and_apply_paths_with_mocks(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            repo = pathlib.Path(td)
            config = repo / "versioning.json"
            config.write_text(
                json.dumps(
                    {
                        "surfaces": {
                            "sdk": {
                                "label": "SDK",
                                "version_files": [
                                    {"path": "sdk.json", "kind": "json_field", "field": "version"}
                                ],
                                "trigger_paths": ["src/**"],
                                "public_api_paths": ["src/**"],
                            }
                        },
                        "generated_globs": ["build/**"],
                    }
                ),
                encoding="utf-8",
            )
            (repo / "sdk.json").write_text('{"version": "1.2.3"}', encoding="utf-8")
            stdout = io.StringIO()
            verdict = vbc.Verdict(
                vbc.Surface(
                    "sdk",
                    "SDK",
                    [vbc.VersionFile("sdk.json", "json_field", "version")],
                    ["src/**"],
                ),
                "minor",
                None,
                "1.2.3",
                "minor",
            )
            with mock.patch.object(vbc, "git_diff_names", return_value=["src/api.hpp", "build/generated.cpp"]), \
                 mock.patch.object(vbc, "assess_surfaces", return_value=[verdict]), \
                 mock.patch.object(vbc, "render_report", return_value=("report text", 0)) as render, \
                 contextlib.redirect_stdout(stdout):
                self.assertEqual(vbc.main(["--repo-root", str(repo), "--config", str(config), "--mode", "hint"]), 0)

            self.assertIn("report text", stdout.getvalue())
            render.assert_called_once()

            stdout = io.StringIO()
            with mock.patch.object(vbc, "git_diff_names", return_value=["src/api.hpp"]), \
                 mock.patch.object(vbc, "assess_surfaces", return_value=[verdict]), \
                 mock.patch.object(vbc, "apply_bumps", return_value=["sdk.json"]), \
                 mock.patch.object(vbc, "render_report", return_value=("after", 0)), \
                 contextlib.redirect_stdout(stdout):
                self.assertEqual(vbc.main(["--repo-root", str(repo), "--config", str(config), "--mode", "apply"]), 0)

            self.assertIn("Edited files:", stdout.getvalue())
            self.assertIn("sdk.json", stdout.getvalue())

    def test_main_apply_mode_runs_fix_feat_gate_failure(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            repo = pathlib.Path(td)
            config = repo / "versioning.json"
            config.write_text('{"surfaces": {}}\n', encoding="utf-8")
            stdout = io.StringIO()
            with mock.patch.object(vbc, "git_diff_names", return_value=[]), \
                 mock.patch.object(vbc, "assess_surfaces", return_value=[]), \
                 mock.patch.object(vbc, "apply_bumps", return_value=[]), \
                 mock.patch.object(vbc, "render_report", return_value=("", 0)), \
                 mock.patch.object(vbc, "check_fix_feat_requires_bump", return_value=(False, "missing bump")), \
                 contextlib.redirect_stdout(stdout):
                rc = vbc.main([
                    "--repo-root", str(repo),
                    "--config", str(config),
                    "--mode", "apply",
                    "--require-bump-for-fix-feat",
                    "--pr-title", "fix: user-visible bug",
                ])

        self.assertEqual(rc, 1)
        self.assertIn("missing bump", stdout.getvalue())

    def test_main_apply_mode_allows_passing_fix_feat_gate(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            repo = pathlib.Path(td)
            config = repo / "versioning.json"
            config.write_text('{"surfaces": {}}\n', encoding="utf-8")
            stdout = io.StringIO()
            with mock.patch.object(vbc, "git_diff_names", return_value=[]), \
                 mock.patch.object(vbc, "assess_surfaces", return_value=[]), \
                 mock.patch.object(vbc, "apply_bumps", return_value=[]), \
                 mock.patch.object(vbc, "render_report", return_value=("", 0)), \
                 mock.patch.object(vbc, "check_fix_feat_requires_bump", return_value=(True, "bump present")), \
                 contextlib.redirect_stdout(stdout):
                rc = vbc.main([
                    "--repo-root", str(repo),
                    "--config", str(config),
                    "--mode", "apply",
                    "--require-bump-for-fix-feat",
                    "--pr-title", "fix: user-visible bug",
                ])

        self.assertEqual(rc, 0)
        self.assertIn("bump present", stdout.getvalue())

    def test_main_report_mode_allows_empty_report_text(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            repo = pathlib.Path(td)
            config = repo / "versioning.json"
            config.write_text('{"surfaces": {}}\n', encoding="utf-8")
            stdout = io.StringIO()
            with mock.patch.object(vbc, "git_diff_names", return_value=[]), \
                 mock.patch.object(vbc, "assess_surfaces", return_value=[]), \
                 mock.patch.object(vbc, "render_report", return_value=("", 0)), \
                 contextlib.redirect_stdout(stdout):
                rc = vbc.main(["--repo-root", str(repo), "--config", str(config)])

        self.assertEqual(rc, 0)
        self.assertEqual(stdout.getvalue(), "")

    def test_main_report_and_hint_modes_run_fix_feat_gate(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            repo = pathlib.Path(td)
            config = repo / "versioning.json"
            config.write_text('{"surfaces": {}}\n', encoding="utf-8")
            stdout = io.StringIO()
            with mock.patch.object(vbc, "git_diff_names", return_value=[]), \
                 mock.patch.object(vbc, "assess_surfaces", return_value=[]), \
                 mock.patch.object(vbc, "render_report", return_value=("report text", 0)), \
                 mock.patch.object(vbc, "check_fix_feat_requires_bump", return_value=(False, "missing bump")), \
                 contextlib.redirect_stdout(stdout):
                rc = vbc.main([
                    "--repo-root", str(repo),
                    "--config", str(config),
                    "--mode", "report",
                    "--require-bump-for-fix-feat",
                    "--pr-title", "fix: bug",
                ])
        self.assertEqual(rc, 1)
        self.assertIn("fix/feat-needs-bump check", stdout.getvalue())
        self.assertIn("missing bump", stdout.getvalue())

        with tempfile.TemporaryDirectory() as td:
            repo = pathlib.Path(td)
            config = repo / "versioning.json"
            config.write_text('{"surfaces": {}}\n', encoding="utf-8")
            stdout = io.StringIO()
            with mock.patch.object(vbc, "git_diff_names", return_value=[]), \
                 mock.patch.object(vbc, "assess_surfaces", return_value=[]), \
                 mock.patch.object(vbc, "render_report", return_value=("", 0)), \
                 mock.patch.object(vbc, "check_fix_feat_requires_bump", return_value=(False, "hint only")), \
                 contextlib.redirect_stdout(stdout):
                rc = vbc.main([
                    "--repo-root", str(repo),
                    "--config", str(config),
                    "--mode", "hint",
                    "--require-bump-for-fix-feat",
                    "--pr-title", "fix: bug",
                ])
        self.assertEqual(rc, 0)
        self.assertIn("hint only", stdout.getvalue())


class PerSurfaceTrailerSkipRequiresReason(unittest.TestCase):
    """pulp #1054 — Version-Bump: <surface>=skip MUST carry a non-empty
    reason="..." (mirrors PR #1315's enforcement on the unscoped form).
    Earlier shapes silently accepted bare `cli=skip` and `cli=skip reason=""`.
    """

    def test_canonical_per_surface_skip_with_reason(self) -> None:
        result = vbc.surface_trailer_override(
            {"version-bump": ['cli=skip reason="legitimate"']},
            "version-bump",
            "cli",
        )
        self.assertEqual(result, "skip")

    def test_per_surface_skip_without_reason_rejected(self) -> None:
        # Bare `cli=skip` is rejected — author must explain why.
        result = vbc.surface_trailer_override(
            {"version-bump": ['cli=skip']},
            "version-bump",
            "cli",
        )
        self.assertIsNone(result)

    def test_per_surface_skip_with_empty_reason_rejected(self) -> None:
        # `cli=skip reason=""` is rejected (empty quotes don't count).
        result = vbc.surface_trailer_override(
            {"version-bump": ['cli=skip reason=""']},
            "version-bump",
            "cli",
        )
        self.assertIsNone(result)

    def test_per_surface_skip_with_whitespace_only_reason_rejected(self) -> None:
        # `cli=skip reason="   "` is rejected (whitespace-only doesn't count).
        result = vbc.surface_trailer_override(
            {"version-bump": ['cli=skip reason="   "']},
            "version-bump",
            "cli",
        )
        self.assertIsNone(result)

    def test_per_surface_patch_minor_major_dont_require_reason(self) -> None:
        # Bump-level trailers don't need a reason — the level itself
        # documents intent (the bump verdict IS the explanation).
        for level in ("patch", "minor", "major"):
            with self.subTest(level=level):
                result = vbc.surface_trailer_override(
                    {"version-bump": [f"cli={level}"]},
                    "version-bump",
                    "cli",
                )
                self.assertEqual(result, level)


if __name__ == "__main__":
    unittest.main()

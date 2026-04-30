#!/usr/bin/env python3
"""Additional coverage-lane tests for skill_sync_check.py."""

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


sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import skill_sync_check as ssc  # noqa: E402


class GitAndTrailerTests(unittest.TestCase):
    def test_repo_root_and_diff_names_delegate_to_git(self) -> None:
        with mock.patch.object(
            ssc.subprocess,
            "run",
            return_value=subprocess.CompletedProcess([], 0, stdout="/repo\n"),
        ) as run:
            self.assertEqual(ssc.repo_root(), pathlib.Path("/repo"))

        run.assert_called_once_with(
            ["git", "rev-parse", "--show-toplevel"],
            check=True,
            capture_output=True,
            text=True,
        )

        with mock.patch.object(
            ssc.subprocess,
            "run",
            return_value=subprocess.CompletedProcess([], 0, stdout="a\n\nb\n"),
        ):
            self.assertEqual(ssc.git_diff_names("base", "head"), ["a", "b"])

    def test_git_range_trailers_and_single_commit_shim(self) -> None:
        def fake_run(cmd: list[str], **kwargs) -> subprocess.CompletedProcess[str]:
            if cmd[:3] == ["git", "log", "--format=%B%x00"]:
                return subprocess.CompletedProcess(cmd, 0, stdout="body 1\0body 2\0")
            if cmd[:3] == ["git", "log", "-1"]:
                return subprocess.CompletedProcess(cmd, 0, stdout="body\n")
            if cmd[:2] == ["git", "interpret-trailers"]:
                return subprocess.CompletedProcess(
                    cmd,
                    0,
                    stdout='Skill-Update: skip skill=ci reason="doc-only"\nBad\n',
                )
            raise AssertionError(cmd)

        with mock.patch.object(ssc.subprocess, "run", side_effect=fake_run):
            self.assertEqual(
                ssc.git_range_trailers("base", "head"),
                {
                    "skill-update": [
                        'skip skill=ci reason="doc-only"',
                        'skip skill=ci reason="doc-only"',
                    ]
                },
            )
            self.assertEqual(
                ssc.git_commit_trailers("HEAD"),
                {"skill-update": ['skip skill=ci reason="doc-only"']},
            )

    def test_git_trailer_helpers_tolerate_git_errors(self) -> None:
        with mock.patch.object(
            ssc.subprocess,
            "run",
            side_effect=subprocess.CalledProcessError(128, ["git"]),
        ):
            self.assertEqual(ssc.git_range_trailers("base", "head"), {})
            self.assertEqual(ssc.git_commit_trailers("HEAD"), {})

    def test_parse_skill_update_trailer_variants(self) -> None:
        trailers = {
            "skill-update": [
                "skip skill=ci reason=plain",
                "skip skill=android",
                "ignore skill=ci reason=nope",
                "skip reason=missing-skill",
            ]
        }
        self.assertEqual(
            ssc.parse_skill_update_trailer(trailers, "Skill-Update"),
            {"ci": "plain", "android": "(no reason given)"},
        )


class MatchingAndSelfCheckTests(unittest.TestCase):
    def test_strip_comments_and_absolute_config_defaults(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            skills_dir = root / "abs-skills"
            path_map = root / "abs-map.json"
            config = root / "versioning.json"
            config.write_text(
                json.dumps(
                    {
                        "skills": {
                            "skills_dir": str(skills_dir),
                            "path_map": str(path_map),
                        }
                    }
                ),
                encoding="utf-8",
            )

            loaded = ssc.load_config(config, root)

        self.assertEqual(ssc._strip_comments(["not", "a", "dict"]), ["not", "a", "dict"])
        self.assertEqual(loaded.skills_dir, skills_dir)
        self.assertEqual(loaded.path_map_file, path_map)
        self.assertEqual(loaded.generated_globs, [])
        self.assertEqual(loaded.trailer_skill_update, "Skill-Update")

    def test_load_skill_map_treats_non_dict_entries_as_empty(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            path = pathlib.Path(td) / "skill_path_map.json"
            path.write_text(
                json.dumps({"skills": {"ci": ["legacy-shape"]}}),
                encoding="utf-8",
            )

            loaded = ssc.load_skill_map(path)

        self.assertEqual(loaded.skills, {"ci": []})

    def test_filter_generated_and_globs(self) -> None:
        changed = [
            "build/generated.cpp",
            "tools/cli/cmd_pr.cpp",
            "tools/cli/nested/cmd_pr.cpp",
            "test/test_a.cpp",
        ]
        self.assertEqual(
            ssc.filter_generated(changed, ["build/**", "tools/cli/**/*.cpp"]),
            ["test/test_a.cpp"],
        )
        self.assertTrue(ssc._matches_any("test/test_a.cpp", ["test/test_?.cpp"]))
        self.assertFalse(ssc._matches_any("test/test_long.cpp", ["test/test_?.cpp"]))

    def test_glob_to_regex_covers_starstar_positions(self) -> None:
        self.assertTrue(ssc._glob_match("anything/goes.txt", "**"))
        self.assertTrue(ssc._glob_match("notes.md", "**/*.md"))
        self.assertTrue(ssc._glob_match("docs/guides/setup.md", "docs/**"))
        self.assertTrue(ssc._glob_match("src/test/file.py", "src/**/test/*.py"))
        self.assertTrue(ssc._glob_match("src/pkg/test/file.py", "src/**/test/*.py"))
        self.assertFalse(ssc._glob_match("src/pkg/test/file.pyc", "src/**/test/*.py"))

    def test_glob_to_regex_preserves_double_slash_starstar_boundaries(self) -> None:
        self.assertTrue(ssc._glob_match("src//nested/file.py", "src//**"))
        self.assertFalse(ssc._glob_match("src//file.py", "src//**/file.py"))

    def test_self_check_reports_extra_and_missing_skills(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            skills_dir = pathlib.Path(td) / "skills"
            (skills_dir / "ci").mkdir(parents=True)
            (skills_dir / "unused").mkdir()
            (skills_dir / "README.md").write_text("ignore", encoding="utf-8")

            errors = ssc.self_check(
                ssc.SkillMap({"ci": ["ci/**"], "missing": ["missing/**"]}),
                skills_dir,
            )

        self.assertTrue(any("unused" in error for error in errors), msg=errors)
        self.assertTrue(any("missing" in error for error in errors), msg=errors)
        self.assertEqual(
            ssc.self_check(ssc.SkillMap({"ci": ["ci/**"]}), pathlib.Path("/nope")),
            [],
        )


class ComputeAndRenderTests(unittest.TestCase):
    def test_compute_findings_ignores_unmatched_skills_and_strips_dot_slash(self) -> None:
        class DotSlashSkillsDir:
            def relative_to(self, _repo: pathlib.Path) -> "DotSlashSkillsDir":
                return self

            def __str__(self) -> str:
                return "./.agents/skills"

        findings = ssc.compute_findings(
            changed=[
                "src/ci/file.cpp",
                ".agents/skills/ci/SKILL.md",
            ],
            skill_map=ssc.SkillMap(
                {
                    "ci": ["src/ci/**"],
                    "android": ["android/**"],
                }
            ),
            skills_dir=DotSlashSkillsDir(),  # type: ignore[arg-type]
            repo=pathlib.Path("/repo"),
            bypasses={},
        )

        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0].skill, "ci")
        self.assertTrue(findings[0].skill_md_modified)

    def test_compute_findings_handles_nested_skill_md_and_external_skills_dir(self) -> None:
        repo = pathlib.Path("/repo")
        skills_dir = pathlib.Path("/external/skills")
        changed = [
            "src/ci/file.cpp",
            "/external/skills/ci/deep/SKILL.md",
        ]

        findings = ssc.compute_findings(
            changed=changed,
            skill_map=ssc.SkillMap({"ci": ["src/ci/**"]}),
            skills_dir=skills_dir,
            repo=repo,
            bypasses={"ci": "external skill dir"},
        )

        self.assertEqual(len(findings), 1)
        self.assertTrue(findings[0].skill_md_modified)
        self.assertEqual(findings[0].bypass_reason, "external skill dir")

    def test_render_report_covers_empty_bypass_updated_and_long_touched_list(self) -> None:
        empty, empty_code = ssc.render_report([], "Skill-Update", "report")
        self.assertEqual(empty_code, 0)
        self.assertIn("nothing to verify", empty)

        bypassed = ssc.Finding(
            skill="ci",
            touched_paths=["ci/file.cpp"],
            skill_md_modified=False,
            bypass_reason="doc-only",
        )
        text, code = ssc.render_report([bypassed], "Skill-Update", "report")
        self.assertEqual(code, 0)
        self.assertIn("bypassed", text)

        updated = ssc.Finding(
            skill="ci",
            touched_paths=[f"ci/file{i}.cpp" for i in range(10)],
            skill_md_modified=True,
            bypass_reason=None,
        )
        text, code = ssc.render_report([updated], "Skill-Update", "hint")
        self.assertEqual(code, 0)
        self.assertIn("2 more", text)

    def test_render_report_apply_failure_and_report_hides_updated_paths(self) -> None:
        failing = ssc.Finding(
            skill="ci",
            touched_paths=["ci/file.cpp"],
            skill_md_modified=False,
            bypass_reason=None,
        )
        text, code = ssc.render_report([failing], "Skill-Update", "apply")
        self.assertEqual(code, 1)
        self.assertIn("Skill-sync check FAILED.", text)

        updated = ssc.Finding(
            skill="ci",
            touched_paths=["ci/file.cpp"],
            skill_md_modified=True,
            bypass_reason=None,
        )
        text, code = ssc.render_report([updated], "Skill-Update", "report")
        self.assertEqual(code, 0)
        self.assertIn("[ci] ✓ SKILL.md updated", text)
        self.assertNotIn("ci/file.cpp", text)


class MainTests(unittest.TestCase):
    def _write_config(self, root: pathlib.Path) -> pathlib.Path:
        skills = root / ".agents" / "skills" / "ci"
        skills.mkdir(parents=True)
        path_map = root / "skill_path_map.json"
        config = root / "versioning.json"
        path_map.write_text(
            json.dumps({"skills": {"ci": {"paths": ["ci/**"]}}}),
            encoding="utf-8",
        )
        config.write_text(
            json.dumps(
                {
                    "skills": {
                        "skills_dir": ".agents/skills",
                        "path_map": "skill_path_map.json",
                    },
                    "generated_globs": ["build/**"],
                    "trailers": {"skill_update": "Skill-Update"},
                }
            ),
            encoding="utf-8",
        )
        return config

    def test_main_reports_missing_config(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                rc = ssc.main(
                    [
                        "--repo-root",
                        td,
                        "--config",
                        str(pathlib.Path(td) / "missing.json"),
                    ]
                )

        self.assertEqual(rc, 2)
        self.assertIn("config not found", stderr.getvalue())

    def test_main_hint_mode_success_with_mocks(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            config = self._write_config(root)
            stdout = io.StringIO()
            with mock.patch.object(
                ssc,
                "git_diff_names",
                return_value=["ci/file.cpp", "build/generated.cpp"],
            ), mock.patch.object(
                ssc,
                "git_range_trailers",
                return_value={"skill-update": ['skip skill=ci reason="generated"']},
            ), contextlib.redirect_stdout(stdout):
                rc = ssc.main(
                    [
                        "--repo-root",
                        str(root),
                        "--config",
                        str(config),
                        "--mode",
                        "hint",
                    ]
                )

        self.assertEqual(rc, 0)
        self.assertIn("bypassed", stdout.getvalue())

    def test_main_defaults_repo_root_and_config_and_returns_report_code(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            config = self._write_config(root)
            default_config = root / "tools" / "scripts" / "versioning.json"
            default_config.parent.mkdir(parents=True)
            default_config.write_text(config.read_text(encoding="utf-8"), encoding="utf-8")
            stdout = io.StringIO()
            with mock.patch.object(ssc, "repo_root", return_value=root), mock.patch.object(
                ssc,
                "git_diff_names",
                return_value=[],
            ), mock.patch.object(
                ssc,
                "git_range_trailers",
                return_value={},
            ), contextlib.redirect_stdout(stdout):
                rc = ssc.main([])

        self.assertEqual(rc, 0)
        self.assertIn("nothing to verify", stdout.getvalue())

    def test_main_hint_mode_continues_after_self_check_errors(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            config = self._write_config(root)
            path_map = root / "skill_path_map.json"
            path_map.write_text(
                json.dumps({"skills": {"missing": {"paths": ["missing/**"]}}}),
                encoding="utf-8",
            )
            stdout = io.StringIO()
            stderr = io.StringIO()
            with mock.patch.object(
                ssc,
                "git_diff_names",
                return_value=[],
            ), mock.patch.object(
                ssc,
                "git_range_trailers",
                return_value={},
            ), contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                rc = ssc.main(
                    [
                        "--repo-root",
                        str(root),
                        "--config",
                        str(config),
                        "--mode",
                        "hint",
                    ]
                )

        self.assertEqual(rc, 0)
        self.assertIn("missing", stderr.getvalue())
        self.assertIn("nothing to verify", stdout.getvalue())

    def test_main_prints_no_report_text_and_main_module_exit(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            config = self._write_config(root)
            stdout = io.StringIO()
            with mock.patch.object(
                ssc,
                "git_diff_names",
                return_value=[],
            ), mock.patch.object(
                ssc,
                "git_range_trailers",
                return_value={},
            ), mock.patch.object(
                ssc,
                "render_report",
                return_value=("", 0),
            ), contextlib.redirect_stdout(stdout):
                rc = ssc.main(["--repo-root", str(root), "--config", str(config)])

            old_argv = sys.argv
            sys.argv = [
                str(pathlib.Path(ssc.__file__).resolve()),
                "--repo-root",
                str(root),
                "--config",
                str(root / "missing.json"),
            ]
            stderr = io.StringIO()
            try:
                with contextlib.redirect_stderr(stderr), self.assertRaises(SystemExit) as cm:
                    runpy.run_path(str(pathlib.Path(ssc.__file__).resolve()), run_name="__main__")
            finally:
                sys.argv = old_argv

        self.assertEqual(rc, 0)
        self.assertEqual(stdout.getvalue(), "")
        self.assertEqual(cm.exception.code, 2)
        self.assertIn("config not found", stderr.getvalue())

    def test_main_self_check_blocks_report_mode(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            config = self._write_config(root)
            path_map = root / "skill_path_map.json"
            path_map.write_text(
                json.dumps({"skills": {"missing": {"paths": ["missing/**"]}}}),
                encoding="utf-8",
            )
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                rc = ssc.main(["--repo-root", str(root), "--config", str(config)])

        self.assertEqual(rc, 1)
        self.assertIn("missing", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()

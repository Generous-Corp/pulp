#!/usr/bin/env python3
"""Additional focused tests for tools/deps/audit.py."""

from __future__ import annotations

import io
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]

sys.path.insert(0, str(ROOT / "tools" / "deps"))
import audit  # noqa: E402


class ParserEdgeTests(unittest.TestCase):
    def test_dependencies_md_tolerates_empty_cell_lists(self) -> None:
        class EmptyCellsLine:
            def strip(self, chars: str | None = None) -> "EmptyCellsLine":
                return self

            def startswith(self, prefix: str) -> bool:
                return prefix == "|"

            def split(self, sep: str | None = None, maxsplit: int = -1) -> list[str]:
                return []

        class TextWithEmptyCells:
            def splitlines(self) -> list[EmptyCellsLine]:
                return [EmptyCellsLine()]

        class PathLike:
            def read_text(self) -> TextWithEmptyCells:
                return TextWithEmptyCells()

        with mock.patch.object(audit, "DEPENDENCIES_MD", PathLike()):
            self.assertEqual(audit.parse_dependencies_md(), set())

    def test_dependencies_md_skips_headers_and_separator_rows(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            deps_md = Path(td) / "DEPENDENCIES.md"
            deps_md.write_text(
                textwrap.dedent(
                    """\
                    Intro text
                    | Name | Version |
                    | ---- | ------- |
                    | ------ | ------- |
                    | SDK | Notes |
                    | Real Dep | 1.0 |
                    | ----- | ignored |
                    """
                ),
                encoding="utf-8",
            )

            with mock.patch.object(audit, "DEPENDENCIES_MD", deps_md):
                self.assertEqual(audit.parse_dependencies_md(), {"Real Dep"})

    def test_licensing_md_tolerates_empty_cell_lists(self) -> None:
        class EmptyCellsLine:
            def strip(self, chars: str | None = None) -> "EmptyCellsLine":
                return self

            def startswith(self, prefix: str) -> bool:
                return prefix == "|"

            def split(self, sep: str | None = None, maxsplit: int = -1) -> list[str]:
                return []

        class TextWithEmptyCells:
            def splitlines(self) -> list[EmptyCellsLine]:
                return [EmptyCellsLine()]

        class PathLike:
            def read_text(self) -> TextWithEmptyCells:
                return TextWithEmptyCells()

        with mock.patch.object(audit, "LICENSING_MD", PathLike()):
            self.assertEqual(audit.parse_licensing_md(), set())

    def test_manifest_alias_set_includes_explicit_default_and_normalised_names(self) -> None:
        aliases = audit.manifest_alias_set({
            "name": "Highway",
            "external_names": ["HWY custom"],
        })

        self.assertIn("highway", aliases)
        self.assertIn("hwy", aliases)
        self.assertIn("hwycustom", aliases)

    def test_missing_manifest_source_paths_return_empty(self) -> None:
        missing = Path(tempfile.gettempdir()) / "pulp-audit-missing-fixture"
        self.assertFalse(missing.exists())

        with mock.patch.object(audit, "REQUIREMENTS_DOCS", missing), \
             mock.patch.object(audit, "MKDOCS_YML", missing), \
             mock.patch.object(audit, "EXTERNAL_DIR", missing):
            self.assertEqual(audit.parse_requirements_docs(), [])
            self.assertEqual(audit.parse_mkdocs_yml(), [])
            self.assertEqual(audit.parse_external_dirs(), [])

        self.assertEqual(audit.parse_fetchcontent(missing), [])

    def test_requirements_parser_ignores_unparseable_lines(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            req = Path(td) / "requirements.txt"
            req.write_text(
                textwrap.dedent(
                    """\
                    @not-a-package
                    valid-package[extra]>=1.0 ; python_version >= "3.11"
                    """
                ),
                encoding="utf-8",
            )

            with mock.patch.object(audit, "REQUIREMENTS_DOCS", req):
                declared = audit.parse_requirements_docs()

        self.assertEqual([dep.name for dep in declared], ["valid-package"])

    def test_external_dir_parser_skips_files_hidden_and_ignored_dirs(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            external = Path(td) / "external"
            external.mkdir()
            (external / "README.md").write_text("not a directory\n", encoding="utf-8")
            (external / ".cache").mkdir()
            (external / "fonts").mkdir()
            (external / "actual-dep").mkdir()

            with mock.patch.object(audit, "EXTERNAL_DIR", external):
                declared = audit.parse_external_dirs()

        self.assertEqual(
            declared,
            [audit.DeclaredDep("actual-dep", "external/", "actual-dep/")],
        )

    def test_collect_declared_restores_globals_after_extra_inputs(self) -> None:
        original = (
            audit.REQUIREMENTS_DOCS,
            audit.MKDOCS_YML,
            list(audit.EXTRA_CMAKELISTS),
            audit.ROOT_CMAKELISTS,
        )
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            req = root / "requirements.txt"
            mkdocs = root / "mkdocs.yml"
            cmake = root / "CMakeLists.txt"
            req.write_text("temp-package==1\n", encoding="utf-8")
            mkdocs.write_text("site_name: Temp\n", encoding="utf-8")
            cmake.write_text("# none\n", encoding="utf-8")
            with mock.patch.object(audit, "EXTERNAL_DIR", root / "missing-external"):
                declared = audit.collect_declared(
                    extra_requirements=req,
                    extra_mkdocs=mkdocs,
                    extra_cmake=[cmake],
                )

        self.assertIn("temp-package", {dep.name for dep in declared})
        self.assertEqual(
            (
                audit.REQUIREMENTS_DOCS,
                audit.MKDOCS_YML,
                audit.EXTRA_CMAKELISTS,
                audit.ROOT_CMAKELISTS,
            ),
            original,
        )


class UpstreamStatusTests(unittest.TestCase):
    def test_run_git_ls_remote_returns_stdout_and_swallows_failures(self) -> None:
        completed = subprocess.CompletedProcess(
            args=["git"],
            returncode=0,
            stdout="abc123\tHEAD\n",
            stderr="",
        )
        with mock.patch.object(audit.subprocess, "run", return_value=completed) as run:
            self.assertEqual(
                audit.run_git_ls_remote("https://example.invalid/repo.git", "HEAD"),
                "abc123\tHEAD",
            )

        self.assertEqual(run.call_args.args[0][-1], "HEAD")
        with mock.patch.object(
            audit.subprocess,
            "run",
            side_effect=subprocess.TimeoutExpired(["git"], 5),
        ):
            self.assertEqual(audit.run_git_ls_remote("repo", "HEAD"), "")
        with mock.patch.object(
            audit.subprocess,
            "run",
            side_effect=subprocess.CalledProcessError(1, ["git"]),
        ):
            self.assertEqual(audit.run_git_ls_remote("repo", "HEAD"), "")

    def test_semver_and_latest_tag_select_highest_parseable_tag(self) -> None:
        output = textwrap.dedent(
            """\
            abc\trefs/tags/v1.2.0
            def\trefs/tags/not-a-version
            ghi\trefs/tags/2.0.1

            """
        )
        with mock.patch.object(audit, "run_git_ls_remote", return_value=output):
            self.assertEqual(audit.latest_semver_tag("repo"), "2.0.1")

        self.assertEqual(audit.semver_key("release-3.4.5"), (3, 4, 5))
        self.assertIsNone(audit.semver_key("release"))
        with mock.patch.object(audit, "run_git_ls_remote", return_value=""):
            self.assertIsNone(audit.latest_semver_tag("repo"))

    def test_upstream_status_handles_all_kinds(self) -> None:
        def dep(kind: str, ref: str | None = None) -> dict:
            upstream = {"kind": kind}
            if ref is not None:
                upstream["ref"] = ref
            return {"repository": "repo", "upstream": upstream}

        with mock.patch.object(
            audit,
            "run_git_ls_remote",
            side_effect=[
                "1234567890abcdef\tHEAD",
                "",
                "abcdef1234567890\trefs/heads/main",
                "",
                "tagsha\trefs/tags/v1.0.0",
                "",
                "",
                "",
            ],
        ), mock.patch.object(
            audit,
            "latest_semver_tag",
            side_effect=["v1.2.0", "v1.0.0", None],
        ):
            self.assertEqual(audit.upstream_status(dep("none")), "manual")
            self.assertEqual(audit.upstream_status(dep("git-head")), "1234567890ab")
            self.assertEqual(audit.upstream_status(dep("git-head")), "missing")
            self.assertEqual(audit.upstream_status(dep("git-branch", "main")), "main @ abcdef123456")
            self.assertEqual(audit.upstream_status(dep("git-branch", "dev")), "dev @ missing")
            self.assertEqual(
                audit.upstream_status(dep("git-tag", "v1.0.0")),
                "present; latest=v1.2.0",
            )
            self.assertEqual(audit.upstream_status(dep("git-tag", "v1.0.0")), "missing")
            self.assertEqual(audit.upstream_status(dep("git-tag", "v1.0.0")), "missing")
            self.assertEqual(audit.upstream_status(dep("other")), "unknown")


class RenderingAndMainTests(unittest.TestCase):
    def test_render_markdown_includes_all_missing_sections(self) -> None:
        output = audit.render_markdown(
            [
                {
                    "name": "dep",
                    "version": "1",
                    "license": "MIT",
                    "source_kind": "vendored",
                    "upstream": "skipped",
                    "dependencies_md": "no",
                    "notice_md": "no",
                    "licensing_md": "no",
                }
            ],
            ["missing-deps"],
            ["missing-notice"],
            ["missing-license"],
            [
                audit.DeclaredDep("declared-with-location", "source.txt", "line 1"),
                audit.DeclaredDep("declared-no-location", "other.txt"),
            ],
        )

        self.assertIn("| dep | 1 | MIT | vendored | skipped | no | no | no |", output)
        self.assertIn("## Missing from DEPENDENCIES.md", output)
        self.assertIn("- missing-notice", output)
        self.assertIn("- missing-license", output)
        self.assertIn("`declared-with-location` from source.txt (line 1)", output)
        self.assertIn("`declared-no-location` from other.txt", output)

    def test_main_text_mode_reports_missing_sections_and_strict_failure(self) -> None:
        manifest = [
            {
                "name": "Missing Dep",
                "version": "1.0",
                "license": "MIT",
                "source_kind": "vendored",
                "repository": "repo",
                "upstream": {"kind": "none"},
                "documented_in_dependencies_md": True,
                "documented_in_notice_md": True,
            },
            {
                "name": "VST3-SDK",
                "version": "3",
                "license": "other",
                "source_kind": "sdk",
                "repository": "repo",
                "upstream": {"kind": "none"},
                "documented_in_dependencies_md": False,
                "documented_in_notice_md": True,
            },
        ]
        declared = [audit.DeclaredDep("unknown-declared", "fixture", "line 1")]

        with mock.patch.object(sys, "argv", ["audit.py", "--strict"]), \
             mock.patch.object(audit, "load_manifest", return_value=manifest), \
             mock.patch.object(audit, "parse_dependencies_md", return_value=set()), \
             mock.patch.object(audit, "parse_notice_md", return_value=set()), \
             mock.patch.object(audit, "parse_licensing_md", return_value={"VST3 SDK"}), \
             mock.patch.object(audit, "collect_declared", return_value=declared), \
             mock.patch.object(audit, "find_uncovered_declarations", return_value=declared), \
             mock.patch("sys.stdout", new_callable=io.StringIO) as stdout:
            rc = audit.main()

        output = stdout.getvalue()
        self.assertEqual(rc, 1)
        self.assertIn("Missing Dep: version=1.0", output)
        self.assertIn("Missing from DEPENDENCIES.md", output)
        self.assertIn("Missing from NOTICE.md", output)
        self.assertIn("Missing from docs/reference/licensing.md", output)
        self.assertIn("unknown-declared from fixture (line 1)", output)
        self.assertIn("VST3-SDK: version=3", output)
        self.assertIn("licensing.md=yes", output)

    def test_main_markdown_mode_uses_upstream_status_and_succeeds_without_strict(self) -> None:
        manifest = [
            {
                "name": "Documented",
                "version": "1.0",
                "license": "MIT",
                "source_kind": "vendored",
                "repository": "repo",
                "upstream": {"kind": "git-head"},
                "documented_in_dependencies_md": True,
                "documented_in_notice_md": True,
            },
        ]

        with mock.patch.object(
            sys,
            "argv",
            ["audit.py", "--check-upstream", "--format", "markdown"],
        ), mock.patch.object(audit, "load_manifest", return_value=manifest), \
             mock.patch.object(audit, "parse_dependencies_md", return_value={"Documented"}), \
             mock.patch.object(audit, "parse_notice_md", return_value={"Documented"}), \
             mock.patch.object(audit, "parse_licensing_md", return_value={"Documented"}), \
             mock.patch.object(audit, "collect_declared", return_value=[]), \
             mock.patch.object(audit, "find_uncovered_declarations", return_value=[]), \
             mock.patch.object(audit, "upstream_status", return_value="abcdef123456"), \
             mock.patch("sys.stdout", new_callable=io.StringIO) as stdout:
            rc = audit.main()

        self.assertEqual(rc, 0)
        self.assertIn("# Dependency Audit", stdout.getvalue())
        self.assertIn("abcdef123456", stdout.getvalue())


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Additional unit tests for tools/packages/freshness_check.py."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import pathlib
import runpy
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "tools/packages/freshness_check.py"

spec = importlib.util.spec_from_file_location("freshness_check_extra_target", SCRIPT)
assert spec and spec.loader
fc = importlib.util.module_from_spec(spec)
sys.modules["freshness_check_extra_target"] = fc
spec.loader.exec_module(fc)


@contextlib.contextmanager
def argv(args: list[str]):
    old = sys.argv[:]
    sys.argv = args[:]
    try:
        yield
    finally:
        sys.argv = old


@contextlib.contextmanager
def module_attr(module, name: str, value):
    old = getattr(module, name)
    setattr(module, name, value)
    try:
        yield
    finally:
        setattr(module, name, old)


class FreshnessCheckExtraTests(unittest.TestCase):
    def test_check_result_default_issues_lists_are_independent(self) -> None:
        first = fc.CheckResult(package="first", pinned_version="1.0.0")
        second = fc.CheckResult(package="second", pinned_version="1.0.0")

        first.issues.append("stale")

        self.assertEqual(second.issues, [])

        explicit_issues = ["already known"]
        explicit = fc.CheckResult(
            package="explicit",
            pinned_version="1.0.0",
            issues=explicit_issues,
        )
        self.assertIs(explicit.issues, explicit_issues)

    def test_extract_owner_repo_rejects_incomplete_github_urls(self) -> None:
        self.assertIsNone(fc.extract_owner_repo("https://github.com"))
        self.assertIsNone(fc.extract_owner_repo("https://github.com/acme"))

    def test_run_gh_returns_json_or_none_for_failures(self) -> None:
        with mock.patch.object(
            fc.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["gh"], 1, stdout="{}"),
        ):
            self.assertIsNone(fc.run_gh(["repos/acme/fail"]))

        with mock.patch.object(
            fc.subprocess,
            "run",
            side_effect=subprocess.TimeoutExpired(["gh"], timeout=30),
        ):
            self.assertIsNone(fc.run_gh(["repos/acme/slow"]))

        with mock.patch.object(
            fc.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["gh"], 0, stdout="{"),
        ):
            self.assertIsNone(fc.run_gh(["repos/acme/bad-json"]))

        with mock.patch.object(
            fc.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(
                ["gh"],
                0,
                stdout='{"archived": false}',
            ),
        ):
            self.assertEqual(fc.run_gh(["repos/acme/ok"]), {"archived": False})

    def test_check_package_reports_parse_api_archive_and_license_issues(self) -> None:
        bad_url = fc.check_package(
            "bad-url",
            {"version": "1.0.0", "fetch": {"git_repository": "not-a-url"}},
        )
        self.assertEqual(bad_url.issues, ["Cannot parse GitHub URL: not-a-url"])

        with mock.patch.object(fc, "run_gh", return_value=None):
            unreachable = fc.check_package(
                "unreachable",
                {
                    "version": "1.0.0",
                    "fetch": {"git_repository": "https://github.com/acme/missing"},
                },
            )

        self.assertEqual(unreachable.issues, ["Cannot reach GitHub API"])

        responses = {
            "repos/acme/archived": {
                "archived": True,
                "pushed_at": "2026-04-02T10:11:12Z",
            },
            "repos/acme/archived/releases?per_page=1": [{"tag_name": "v2.0.0"}],
            "repos/acme/archived/license": {"license": {"spdx_id": "GPL-3.0-only"}},
        }

        with mock.patch.object(fc, "run_gh", side_effect=lambda args: responses[args[0]]):
            result = fc.check_package(
                "archived",
                {
                    "version": "v1.0.0",
                    "license": "MIT",
                    "fetch": {"git_repository": "https://github.com/acme/archived"},
                },
            )

        self.assertTrue(result.archived)
        self.assertEqual(result.last_commit_date, "2026-04-02")
        self.assertEqual(result.latest_version, "v2.0.0")
        self.assertTrue(result.license_changed)
        self.assertEqual(
            result.issues,
            [
                "Repository is archived",
                "Newer version available: v2.0.0 (pinned: v1.0.0)",
                "License mismatch: registry says MIT, GitHub says GPL-3.0-only",
            ],
        )

    def test_check_package_accepts_clean_equal_release_without_license_payload(self) -> None:
        responses = {
            "repos/acme/clean": {"archived": False, "pushed_at": ""},
            "repos/acme/clean/releases?per_page=1": [{"tag_name": "v1.0.0"}],
            "repos/acme/clean/license": None,
        }

        with mock.patch.object(fc, "run_gh", side_effect=lambda args: responses[args[0]]):
            result = fc.check_package(
                "clean",
                {
                    "version": "v1.0.0",
                    "license": "MIT",
                    "fetch": {"git_repository": "https://github.com/acme/clean.git"},
                },
            )

        self.assertFalse(result.archived)
        self.assertIsNone(result.last_commit_date)
        self.assertEqual(result.latest_version, "v1.0.0")
        self.assertFalse(result.license_changed)
        self.assertEqual(result.issues, [])

    def test_check_package_uses_tags_after_non_list_release_payload(self) -> None:
        responses = {
            "repos/acme/tagged": {
                "archived": False,
                "pushed_at": "2026-04-02T10:11:12Z",
            },
            "repos/acme/tagged/releases?per_page=1": {"message": "no releases"},
            "repos/acme/tagged/tags?per_page=1": [{"name": "v1.2.0"}],
            "repos/acme/tagged/license": {"license": {"spdx_id": "NOASSERTION"}},
        }

        with mock.patch.object(fc, "run_gh", side_effect=lambda args: responses[args[0]]):
            result = fc.check_package(
                "tagged",
                {
                    "version": "v1.0.0",
                    "license": "MIT",
                    "fetch": {"git_repository": "https://github.com/acme/tagged.git"},
                },
            )

        self.assertEqual(result.last_commit_date, "2026-04-02")
        self.assertEqual(result.latest_version, "v1.2.0")
        self.assertFalse(result.license_changed)
        self.assertEqual(
            result.issues,
            ["Newer version available: v1.2.0 (pinned: v1.0.0)"],
        )

    def test_check_package_uses_tags_after_empty_release_payload(self) -> None:
        responses = {
            "repos/acme/tagged": {"archived": False, "pushed_at": ""},
            "repos/acme/tagged/releases?per_page=1": [],
            "repos/acme/tagged/tags?per_page=1": [{"name": "v1.0.0"}],
            "repos/acme/tagged/license": {"license": {"spdx_id": "NOASSERTION"}},
        }

        with mock.patch.object(fc, "run_gh", side_effect=lambda args: responses[args[0]]):
            result = fc.check_package(
                "tagged",
                {
                    "version": "v1.0.0",
                    "license": "MIT",
                    "fetch": {"git_repository": "https://github.com/acme/tagged.git"},
                },
            )

        self.assertEqual(result.latest_version, "v1.0.0")
        self.assertIsNone(result.last_commit_date)
        self.assertFalse(result.license_changed)
        self.assertEqual(result.issues, [])

    def test_check_package_tolerates_missing_tags_and_license_payloads(self) -> None:
        responses = {
            "repos/acme/untagged": {"archived": False},
            "repos/acme/untagged/releases?per_page=1": None,
            "repos/acme/untagged/tags?per_page=1": [],
            "repos/acme/untagged/license": None,
        }

        with mock.patch.object(fc, "run_gh", side_effect=lambda args: responses[args[0]]):
            result = fc.check_package(
                "untagged",
                {
                    "version": "v1.0.0",
                    "license": "MIT",
                    "fetch": {"git_repository": "https://github.com/acme/untagged.git"},
                },
            )

        self.assertIsNone(result.latest_version)
        self.assertIsNone(result.last_commit_date)
        self.assertFalse(result.license_changed)
        self.assertEqual(result.issues, [])

    def test_main_without_package_filter_emits_markdown_and_text_summaries(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            registry_path = pathlib.Path(td) / "registry.json"
            registry_path.write_text(
                json.dumps(
                    {
                        "packages": {
                            "ok": {"version": "1.0.0"},
                            "stale": {"version": "1.0.0"},
                        }
                    }
                ),
                encoding="utf-8",
            )

            def fake_check(slug: str, pkg: dict):
                if slug == "stale":
                    return fc.CheckResult(
                        package=slug,
                        pinned_version=pkg["version"],
                        latest_version="2.0.0",
                        last_commit_date="2026-04-02",
                        issues=["Newer version available"],
                    )
                return fc.CheckResult(
                    package=slug,
                    pinned_version=pkg["version"],
                    latest_version=pkg["version"],
                    last_commit_date="2026-04-01",
                )

            out = io.StringIO()
            err = io.StringIO()
            with module_attr(fc, "REGISTRY", registry_path), mock.patch.object(fc, "check_package", fake_check):
                with argv(["freshness_check.py", "--format", "markdown"]):
                    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                        rc = fc.main()

            self.assertEqual(rc, 1)
            self.assertIn("| Package | Pinned | Latest | Last Commit | Issues |", out.getvalue())
            self.assertIn("| ok | 1.0.0 | 1.0.0 | 2026-04-01 | OK |", out.getvalue())
            self.assertIn(
                "| stale | 1.0.0 | 2.0.0 | 2026-04-02 | Newer version available |",
                out.getvalue(),
            )
            self.assertIn("Checking ok", err.getvalue())
            self.assertIn("2 packages checked, 1 issues", err.getvalue())

            out = io.StringIO()
            err = io.StringIO()
            with module_attr(fc, "REGISTRY", registry_path), mock.patch.object(fc, "check_package", fake_check):
                with argv(["freshness_check.py"]):
                    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                        rc = fc.main()

            self.assertEqual(rc, 1)
            text = out.getvalue()
            self.assertIn("ok", text)
            self.assertIn("OK", text)
            self.assertIn("stale", text)
            self.assertIn("ISSUES", text)
            self.assertIn("- Newer version available", text)
            self.assertIn("2 packages checked, 1 issues", err.getvalue())

    def test_main_filters_packages_and_emits_json(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            registry_path = pathlib.Path(td) / "registry.json"
            registry_path.write_text(
                json.dumps(
                    {
                        "packages": {
                            "ok": {
                                "version": "1.0.0",
                                "fetch": {"git_repository": "https://github.com/acme/ok"},
                            }
                        }
                    }
                ),
                encoding="utf-8",
            )

            def fake_check(slug: str, pkg: dict):
                return fc.CheckResult(
                    package=slug,
                    pinned_version=pkg["version"],
                    latest_version=pkg["version"],
                    last_commit_date="2026-04-01",
                )

            out = io.StringIO()
            err = io.StringIO()
            with module_attr(fc, "REGISTRY", registry_path), mock.patch.object(fc, "check_package", fake_check):
                with argv(["freshness_check.py", "--package", "ok", "--format", "json"]):
                    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                        rc = fc.main()

            self.assertEqual(rc, 0)
            payload = json.loads(out.getvalue())
            self.assertEqual(
                payload,
                [
                    {
                        "package": "ok",
                        "pinned": "1.0.0",
                        "latest": "1.0.0",
                        "last_commit": "2026-04-01",
                        "archived": False,
                        "license_changed": False,
                        "issues": [],
                    }
                ],
            )
            self.assertIn("Checking ok", err.getvalue())

    def test_script_entrypoint_reports_missing_package_without_network(self) -> None:
        proc = subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                "--package",
                "__freshness_check_missing_package__",
            ],
            capture_output=True,
            text=True,
            check=False,
        )

        self.assertEqual(proc.returncode, 1)
        self.assertEqual(proc.stdout, "")
        self.assertIn("not in registry", proc.stderr)

    def test_entrypoint_executes_main_in_process(self) -> None:
        err = io.StringIO()
        with argv(["freshness_check.py", "--package", "__freshness_check_missing_package__"]):
            with contextlib.redirect_stderr(err):
                with self.assertRaises(SystemExit) as raised:
                    runpy.run_path(str(SCRIPT), run_name="__main__")

        self.assertEqual(raised.exception.code, 1)
        self.assertIn("not in registry", err.getvalue())


if __name__ == "__main__":
    unittest.main(verbosity=2)

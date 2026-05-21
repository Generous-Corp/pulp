#!/usr/bin/env python3
"""Additional unit tests for package freshness checking edge paths.

Run:
    python3 tools/packages/test_freshness_check_extra.py
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import pathlib
import runpy
import sys
import tempfile
import unittest
from unittest import mock


ROOT = pathlib.Path(__file__).parent


def load_module(name: str, path: pathlib.Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


fc = load_module("freshness_check_extra_target", ROOT / "freshness_check.py")


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
    def test_result_preserves_explicit_issue_list(self) -> None:
        issues = ["already known"]

        result = fc.CheckResult(
            package="audio-lib",
            pinned_version="1.0.0",
            issues=issues,
        )

        self.assertIs(result.issues, issues)

    def test_extract_owner_repo_rejects_incomplete_github_url(self) -> None:
        self.assertIsNone(fc.extract_owner_repo("https://github.com/acme"))

    def test_run_gh_parses_json_and_swallows_failures(self) -> None:
        with mock.patch.object(
            fc.subprocess,
            "run",
            return_value=fc.subprocess.CompletedProcess(
                ["gh"], 0, stdout='{"ok": true}\n',
            ),
        ) as run:
            self.assertEqual(fc.run_gh(["repos/acme/audio-lib"]), {"ok": True})
        self.assertEqual(
            run.call_args.args[0],
            ["gh", "api", "repos/acme/audio-lib"],
        )

        with mock.patch.object(
            fc.subprocess,
            "run",
            return_value=fc.subprocess.CompletedProcess(["gh"], 1, stdout="{}"),
        ):
            self.assertIsNone(fc.run_gh(["repos/acme/audio-lib"]))

        with mock.patch.object(
            fc.subprocess,
            "run",
            return_value=fc.subprocess.CompletedProcess(["gh"], 0, stdout="{not json}"),
        ):
            self.assertIsNone(fc.run_gh(["repos/acme/audio-lib"]))

        with mock.patch.object(
            fc.subprocess,
            "run",
            side_effect=fc.subprocess.TimeoutExpired(["gh"], timeout=30),
        ):
            self.assertIsNone(fc.run_gh(["repos/acme/audio-lib"]))

    def test_check_package_reports_unparseable_repo_url_and_api_failure(self) -> None:
        bad_url = fc.check_package(
            "audio-lib",
            {"version": "1.0.0", "fetch": {"git_repository": "not github"}},
        )
        self.assertEqual(bad_url.issues, ["Cannot parse GitHub URL: not github"])

        with mock.patch.object(fc, "run_gh", return_value=None):
            unreachable = fc.check_package(
                "audio-lib",
                {
                    "version": "1.0.0",
                    "fetch": {"git_repository": "https://github.com/acme/audio-lib"},
                },
            )

        self.assertEqual(unreachable.issues, ["Cannot reach GitHub API"])

    def test_check_package_reports_archived_newer_version_and_license_mismatch(self) -> None:
        responses = {
            "repos/acme/audio-lib": {
                "archived": True,
                "pushed_at": "2026-05-01T10:11:12Z",
            },
            "repos/acme/audio-lib/releases?per_page=1": [{"tag_name": "v2.0.0"}],
            "repos/acme/audio-lib/license": {"license": {"spdx_id": "Apache-2.0"}},
        }

        with mock.patch.object(fc, "run_gh", side_effect=lambda args: responses[args[0]]):
            result = fc.check_package(
                "audio-lib",
                {
                    "version": "v1.0.0",
                    "license": "MIT",
                    "fetch": {"git_repository": "https://github.com/acme/audio-lib"},
                },
            )

        self.assertTrue(result.archived)
        self.assertEqual(result.last_commit_date, "2026-05-01")
        self.assertEqual(result.latest_version, "v2.0.0")
        self.assertTrue(result.license_changed)
        self.assertEqual(
            result.issues,
            [
                "Repository is archived",
                "Newer version available: v2.0.0 (pinned: v1.0.0)",
                "License mismatch: registry says MIT, GitHub says Apache-2.0",
            ],
        )

    def test_check_package_falls_back_to_tags_without_release(self) -> None:
        responses = {
            "repos/acme/audio-lib": {"archived": False, "pushed_at": ""},
            "repos/acme/audio-lib/releases?per_page=1": [],
            "repos/acme/audio-lib/tags?per_page=1": [{"name": "v1.2.3"}],
            "repos/acme/audio-lib/license": {"license": {"spdx_id": "NOASSERTION"}},
        }

        with mock.patch.object(fc, "run_gh", side_effect=lambda args: responses[args[0]]):
            result = fc.check_package(
                "audio-lib",
                {
                    "version": "v1.2.3",
                    "license": "MIT",
                    "fetch": {"git_repository": "https://github.com/acme/audio-lib"},
                },
            )

        self.assertEqual(result.latest_version, "v1.2.3")
        self.assertIsNone(result.last_commit_date)
        self.assertFalse(result.archived)
        self.assertFalse(result.license_changed)
        self.assertEqual(result.issues, [])

    def test_check_package_tolerates_missing_tags_and_license(self) -> None:
        responses = {
            "repos/acme/audio-lib": {"archived": False},
            "repos/acme/audio-lib/releases?per_page=1": None,
            "repos/acme/audio-lib/tags?per_page=1": [],
            "repos/acme/audio-lib/license": None,
        }

        with mock.patch.object(fc, "run_gh", side_effect=lambda args: responses[args[0]]):
            result = fc.check_package(
                "audio-lib",
                {
                    "version": "v1.2.3",
                    "license": "MIT",
                    "fetch": {"git_repository": "https://github.com/acme/audio-lib"},
                },
            )

        self.assertIsNone(result.latest_version)
        self.assertIsNone(result.last_commit_date)
        self.assertEqual(result.issues, [])

    def test_main_emits_markdown_for_all_packages(self) -> None:
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
        text = out.getvalue()
        self.assertIn("| Package | Pinned | Latest | Last Commit | Issues |", text)
        self.assertIn("| ok | 1.0.0 | 1.0.0 | 2026-04-01 | OK |", text)
        self.assertIn("| stale | 1.0.0 | 2.0.0 | ? | Newer version available |", text)
        self.assertIn("2 packages checked, 1 issues", err.getvalue())

    def test_main_emits_json_for_selected_package(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            registry_path = pathlib.Path(td) / "registry.json"
            registry_path.write_text(
                json.dumps(
                    {
                        "packages": {
                            "ok": {"version": "1.0.0"},
                            "other": {"version": "9.9.9"},
                        }
                    }
                ),
                encoding="utf-8",
            )

            def fake_check(slug: str, pkg: dict):
                return fc.CheckResult(
                    package=slug,
                    pinned_version=pkg["version"],
                    latest_version="1.0.0",
                    last_commit_date="2026-04-01",
                )

            out = io.StringIO()
            err = io.StringIO()
            with module_attr(fc, "REGISTRY", registry_path), \
                 mock.patch.object(fc, "check_package", fake_check):
                with argv(["freshness_check.py", "--package", "ok", "--format", "json"]):
                    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                        rc = fc.main()

        self.assertEqual(rc, 0)
        payload = json.loads(out.getvalue())
        self.assertEqual(len(payload), 1)
        self.assertEqual(payload[0]["package"], "ok")
        self.assertEqual(payload[0]["pinned"], "1.0.0")
        self.assertEqual(payload[0]["last_commit"], "2026-04-01")
        self.assertIn("Checking ok...", err.getvalue())
        self.assertNotIn("other", err.getvalue())

    def test_main_emits_text_for_ok_and_issue_results(self) -> None:
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
                        issues=["Cannot reach GitHub API"],
                    )
                return fc.CheckResult(package=slug, pinned_version=pkg["version"])

            out = io.StringIO()
            err = io.StringIO()
            with module_attr(fc, "REGISTRY", registry_path), mock.patch.object(fc, "check_package", fake_check):
                with argv(["freshness_check.py"]):
                    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                        rc = fc.main()

        self.assertEqual(rc, 1)
        text = out.getvalue()
        self.assertRegex(text, r"ok\s+\.+\s+OK")
        self.assertRegex(text, r"stale\s+\.+\s+ISSUES")
        self.assertIn("    - Cannot reach GitHub API", text)
        self.assertIn("2 packages checked, 1 issues", err.getvalue())

    def test_script_entrypoint_reports_unknown_package(self) -> None:
        old_argv = sys.argv[:]
        stdout = io.StringIO()
        stderr = io.StringIO()
        try:
            sys.argv = [
                str(ROOT / "freshness_check.py"),
                "--package",
                "__definitely_missing__",
            ]
            with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                with self.assertRaises(SystemExit) as raised:
                    runpy.run_path(str(ROOT / "freshness_check.py"), run_name="__main__")
        finally:
            sys.argv = old_argv

        self.assertEqual(raised.exception.code, 1)
        self.assertEqual(stdout.getvalue(), "")
        self.assertIn("not in registry", stderr.getvalue())


if __name__ == "__main__":
    unittest.main(verbosity=2)

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


if __name__ == "__main__":
    unittest.main(verbosity=2)

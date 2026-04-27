#!/usr/bin/env python3
"""Unit tests for package registry validation helpers."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest import mock


ROOT = pathlib.Path(__file__).parent


def load_module(name: str, path: pathlib.Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


vr = load_module("validate_registry", ROOT / "validate_registry.py")
fc = load_module("freshness_check", ROOT / "freshness_check.py")


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


class ValidateRegistryTests(unittest.TestCase):
    def test_load_json_exits_for_missing_and_malformed_files(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            missing = pathlib.Path(td) / "missing.json"
            err = io.StringIO()
            with contextlib.redirect_stderr(err), self.assertRaises(SystemExit) as cm:
                vr.load_json(missing)
            self.assertEqual(cm.exception.code, 1)
            self.assertIn("not found", err.getvalue())

            malformed = pathlib.Path(td) / "bad.json"
            malformed.write_text("{not json", encoding="utf-8")
            err = io.StringIO()
            with contextlib.redirect_stderr(err), self.assertRaises(SystemExit) as cm:
                vr.load_json(malformed)
            self.assertEqual(cm.exception.code, 1)
            self.assertIn("ERROR:", err.getvalue())

    def test_structural_validation_reports_errors_and_warnings(self) -> None:
        registry = {
            "registry_version": 1,
            "packages": {
                "Bad_Slug": {
                    "version": "1.0.0",
                    "verification": {
                        "verified_version": "0.9.0",
                        "build_status": {"macOS-ARM64": "pass"},
                    },
                    "platforms": {},
                },
                "desktop-only": {
                    "version": "2.0.0",
                    "verification": {
                        "verified_version": "2.0.0",
                        "build_status": {"macOS-arm64": "pass"},
                    },
                    "platforms": {"macOS": {}, "Windows": {}},
                },
            },
        }

        errors, warnings = vr.validate_structural(registry)

        self.assertIn("registry_version must be 2", errors)
        self.assertTrue(any("Bad_Slug: invalid slug" in error for error in errors))
        self.assertTrue(any("Bad_Slug: must have at least one platform" in error for error in errors))
        self.assertTrue(any("verified_version" in warning for warning in warnings))
        self.assertTrue(any("build_status key 'macOS-ARM64'" in warning for warning in warnings))
        self.assertTrue(any("desktop-only: no Android or iOS" in warning for warning in warnings))

    def test_license_validation_classifies_allowed_rejected_and_unknown(self) -> None:
        registry = {
            "packages": {
                "mit-lib": {"license": "MIT"},
                "gpl-lib": {"license": "GPL-3.0-only"},
                "custom-lib": {"license": "Custom-Proprietary"},
            }
        }

        errors, warnings = vr.validate_licenses(registry)

        self.assertEqual(len(errors), 1)
        self.assertIn("gpl-lib", errors[0])
        self.assertEqual(len(warnings), 1)
        self.assertIn("custom-lib", warnings[0])

    def test_main_valid_registry_reports_success(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            registry_path = root / "registry.json"
            schema_path = root / "schema.json"
            registry_path.write_text(
                json.dumps(
                    {
                        "registry_version": 2,
                        "packages": {
                            "mobile-ready": {
                                "version": "1.0.0",
                                "license": "MIT",
                                "verification": {
                                    "verified_version": "1.0.0",
                                    "build_status": {"macOS-arm64": "pass"},
                                },
                                "platforms": {"macOS": {}, "iOS": {}},
                                "overlaps_with_builtin": {"filters": ["biquad"]},
                            }
                        },
                    }
                ),
                encoding="utf-8",
            )
            schema_path.write_text("{}", encoding="utf-8")

            out = io.StringIO()
            with module_attr(vr, "REGISTRY", registry_path), module_attr(vr, "SCHEMA", schema_path):
                with argv(["validate_registry.py", "--strict", "--check-licenses"]):
                    with contextlib.redirect_stdout(out):
                        rc = vr.main()

            self.assertEqual(rc, 0)
            text = out.getvalue()
            self.assertIn("mobile-ready", text)
            self.assertIn("1 packages validated, 0 errors, 0 warnings", text)


class FreshnessCheckTests(unittest.TestCase):
    def test_extract_owner_repo_accepts_github_urls(self) -> None:
        self.assertEqual(
            fc.extract_owner_repo("https://github.com/acme/audio-lib.git"),
            ("acme", "audio-lib"),
        )
        self.assertEqual(
            fc.extract_owner_repo("https://github.com/acme/audio-lib/"),
            ("acme", "audio-lib"),
        )
        self.assertIsNone(fc.extract_owner_repo("https://example.com/acme/audio-lib"))

    def test_run_gh_returns_json_or_none_for_failures(self) -> None:
        completed = SimpleNamespace(returncode=0, stdout='{"ok": true}')
        with mock.patch.object(fc.subprocess, "run", return_value=completed):
            self.assertEqual(fc.run_gh(["repos/acme/audio-lib"]), {"ok": True})

        failed = SimpleNamespace(returncode=1, stdout="")
        with mock.patch.object(fc.subprocess, "run", return_value=failed):
            self.assertIsNone(fc.run_gh(["repos/acme/audio-lib"]))

        bad_json = SimpleNamespace(returncode=0, stdout="not-json")
        with mock.patch.object(fc.subprocess, "run", return_value=bad_json):
            self.assertIsNone(fc.run_gh(["repos/acme/audio-lib"]))

        with mock.patch.object(fc.subprocess, "run", side_effect=subprocess.TimeoutExpired("gh", 30)):
            self.assertIsNone(fc.run_gh(["repos/acme/audio-lib"]))

    def test_check_package_records_metadata_version_and_license_issues(self) -> None:
        responses = {
            "repos/acme/audio-lib": {"archived": True, "pushed_at": "2026-04-01T00:00:00Z"},
            "repos/acme/audio-lib/releases?per_page=1": [{"tag_name": "v2.0.0"}],
            "repos/acme/audio-lib/license": {"license": {"spdx_id": "Apache-2.0"}},
        }

        with mock.patch.object(fc, "run_gh", side_effect=lambda args: responses[args[0]]):
            result = fc.check_package(
                "audio-lib",
                {
                    "version": "v1.0.0",
                    "license": "MIT",
                    "fetch": {"git_repository": "https://github.com/acme/audio-lib.git"},
                },
            )

        self.assertTrue(result.archived)
        self.assertEqual(result.last_commit_date, "2026-04-01")
        self.assertEqual(result.latest_version, "v2.0.0")
        self.assertTrue(result.license_changed)
        self.assertIn("Repository is archived", result.issues)
        self.assertTrue(any("Newer version available" in issue for issue in result.issues))
        self.assertTrue(any("License mismatch" in issue for issue in result.issues))

    def test_check_package_handles_unparseable_and_unreachable_repositories(self) -> None:
        bad_url = fc.check_package(
            "bad-url",
            {"version": "1.0.0", "fetch": {"git_repository": "not-a-url"}},
        )
        self.assertIn("Cannot parse GitHub URL", bad_url.issues[0])

        with mock.patch.object(fc, "run_gh", return_value=None):
            unreachable = fc.check_package(
                "unreachable",
                {
                    "version": "1.0.0",
                    "fetch": {"git_repository": "https://github.com/acme/missing"},
                },
            )
        self.assertEqual(unreachable.issues, ["Cannot reach GitHub API"])

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
            self.assertEqual(payload[0]["package"], "ok")
            self.assertIn("Checking ok", err.getvalue())

            err = io.StringIO()
            with module_attr(fc, "REGISTRY", registry_path):
                with argv(["freshness_check.py", "--package", "missing"]):
                    with contextlib.redirect_stderr(err):
                        rc = fc.main()

            self.assertEqual(rc, 1)
            self.assertIn("not in registry", err.getvalue())


if __name__ == "__main__":
    unittest.main(verbosity=2)

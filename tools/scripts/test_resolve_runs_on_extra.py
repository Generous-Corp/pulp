#!/usr/bin/env python3
"""Extra edge tests for tools/scripts/resolve_runs_on.py."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import os
import sys
import unittest
from pathlib import Path
from unittest import mock


SCRIPT = Path(__file__).parent / "resolve_runs_on.py"

spec = importlib.util.spec_from_file_location("resolve_runs_on", SCRIPT)
assert spec and spec.loader
resolver = importlib.util.module_from_spec(spec)
sys.modules["resolve_runs_on"] = resolver
spec.loader.exec_module(resolver)


class ResolverEdgeTests(unittest.TestCase):
    def test_load_selector_accepts_string_and_array_json(self) -> None:
        self.assertEqual(
            resolver._load_selector('"ubuntu-latest"', "Linux"),
            '"ubuntu-latest"',
        )
        self.assertEqual(
            resolver._load_selector('["self-hosted","macos"]', "macOS"),
            '["self-hosted", "macos"]',
        )

    def test_load_selector_rejects_invalid_json_with_target_name(self) -> None:
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            with self.assertRaises(SystemExit) as ctx:
                resolver._load_selector("{bad", "Windows")

        self.assertEqual(ctx.exception.code, 1)
        self.assertIn("Windows runner selector JSON is not valid", stderr.getvalue())

    def test_env_nonempty_handles_missing_none_and_whitespace(self) -> None:
        with mock.patch.dict(os.environ, {"EMPTY": "   ", "VALUE": " label "}, clear=True):
            self.assertEqual(resolver._env_nonempty(None), "")
            self.assertEqual(resolver._env_nonempty("MISSING"), "")
            self.assertEqual(resolver._env_nonempty("EMPTY"), "")
            self.assertEqual(resolver._env_nonempty("VALUE"), "label")

    def test_optional_namespace_returns_empty_when_namespace_has_no_selector(self) -> None:
        with mock.patch.dict(
            os.environ,
            {
                "EXPLICIT_MACOS_RUNNER_SELECTOR_JSON": "   ",
                "NAMESPACE_MACOS_RUNS_ON_JSON": "",
            },
            clear=True,
        ):
            out = resolver.resolve_optional_namespace_mode(
                target_name="macOS (ARM64)",
                requested="namespace",
                explicit_env="EXPLICIT_MACOS_RUNNER_SELECTOR_JSON",
                namespace_env="NAMESPACE_MACOS_RUNS_ON_JSON",
            )

        self.assertEqual(out, "")

    def test_optional_namespace_honors_explicit_for_non_namespace_provider(self) -> None:
        with mock.patch.dict(
            os.environ,
            {
                "EXPLICIT_MACOS_RUNNER_SELECTOR_JSON":
                    '["self-hosted","sanitizer"]',
                "NAMESPACE_MACOS_RUNS_ON_JSON": "",
            },
            clear=True,
        ):
            out = resolver.resolve_optional_namespace_mode(
                target_name="macOS (ARM64)",
                requested="github-hosted",
                explicit_env="EXPLICIT_MACOS_RUNNER_SELECTOR_JSON",
                namespace_env="NAMESPACE_MACOS_RUNS_ON_JSON",
            )

        self.assertEqual(out, '["self-hosted", "sanitizer"]')

    def test_provider_helper_rejects_unknown_provider_when_called_directly(self) -> None:
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            with self.assertRaises(SystemExit) as ctx:
                resolver.resolve_provider_mode(
                    target_name="Linux (x64)",
                    requested="self-hosted",
                    github_hosted_label="ubuntu-latest",
                    explicit_env=None,
                    namespace_env=None,
                    namespace_setting_name=None,
                    local_env=None,
                    local_setting_name=None,
                )

        self.assertEqual(ctx.exception.code, 1)
        self.assertIn("Unsupported runner_provider: self-hosted", stderr.getvalue())

    def test_provider_mode_can_read_custom_requested_provider_env(self) -> None:
        stdout = io.StringIO()
        with mock.patch.dict(
            os.environ,
            {
                "PULP_RUNNER_PROVIDER": "local",
                "LOCAL_LINUX_RUNS_ON_JSON": '["self-hosted","linux"]',
            },
            clear=True,
        ):
            with contextlib.redirect_stdout(stdout):
                rc = resolver.main(
                    [
                        "--target-name",
                        "Linux (x64)",
                        "--mode",
                        "provider",
                        "--requested-provider-env",
                        "PULP_RUNNER_PROVIDER",
                        "--github-hosted-label",
                        "ubuntu-latest",
                        "--local-env",
                        "LOCAL_LINUX_RUNS_ON_JSON",
                    ]
                )

        self.assertEqual(rc, 0)
        self.assertEqual(stdout.getvalue(), '["self-hosted", "linux"]')

    def test_optional_namespace_main_prints_empty_for_github_hosted(self) -> None:
        stdout = io.StringIO()
        with mock.patch.dict(os.environ, {"REQUESTED_PROVIDER": "github-hosted"}, clear=True):
            with contextlib.redirect_stdout(stdout):
                rc = resolver.main(
                    [
                        "--target-name",
                        "macOS (ARM64)",
                        "--mode",
                        "optional-namespace",
                        "--explicit-env",
                        "EXPLICIT_MACOS_RUNNER_SELECTOR_JSON",
                        "--namespace-env",
                        "NAMESPACE_MACOS_RUNS_ON_JSON",
                    ]
                )

        self.assertEqual(rc, 0)
        self.assertEqual(stdout.getvalue(), "")


if __name__ == "__main__":
    unittest.main()

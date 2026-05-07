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


if __name__ == "__main__":
    unittest.main()

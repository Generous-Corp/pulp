#!/usr/bin/env python3
"""Focused orchestration tests for validate_render_update.py."""

from __future__ import annotations

import tempfile
from pathlib import Path
import unittest
from unittest import mock

from tools.deps import validate_render_update as validator


class RenderUpdateValidationTests(unittest.TestCase):
    def test_cache_only_runs_two_fetches_and_capability_probe(self) -> None:
        asset = {"sha256": "a" * 64}
        calls: list[list[str]] = []

        def run(command: list[str]) -> str:
            calls.append(command)
            if command[1:2] == ["tools/scripts/fetch_skia_for_release.py"]:
                return "OK: immutable Skia cache generation ready at /cache\n"
            return "OK\n"

        with tempfile.TemporaryDirectory() as temp, \
             mock.patch.object(validator, "native_platform", return_value="darwin-arm64"), \
             mock.patch.object(validator.skia_fetch, "manifest_asset", return_value=("mac-arm64", asset)), \
             mock.patch.object(validator, "run_checked", side_effect=run):
            result = validator.validate(Path(temp), Path(temp) / "build", cache_only=True)

        self.assertEqual(result["asset_sha256"], "a" * 64)
        self.assertTrue(result["second_fetch_no_download"])
        self.assertEqual(result["mixed_provider"], {"status": "not-run-cache-only"})
        self.assertEqual(
            sum(
                any(item.endswith("fetch_skia_for_release.py") for item in call)
                for call in calls
            ),
            2,
        )
        self.assertTrue(any(
            any(item.endswith("verify_skia_m153_capabilities.py") for item in call)
            for call in calls
        ))

    def test_missing_second_fetch_marker_fails_closed(self) -> None:
        asset = {"sha256": "b" * 64}
        with tempfile.TemporaryDirectory() as temp, \
             mock.patch.object(validator, "native_platform", return_value="linux-x64"), \
             mock.patch.object(validator.skia_fetch, "manifest_asset", return_value=("linux-x64", asset)), \
             mock.patch.object(validator, "run_checked", return_value="downloaded again\n"):
            with self.assertRaisesRegex(RuntimeError, "no-download warm hit"):
                validator.validate(Path(temp), Path(temp) / "build", cache_only=True)

    def test_full_arm64_path_builds_and_records_real_capture(self) -> None:
        asset = {"sha256": "c" * 64}
        with tempfile.TemporaryDirectory() as temp:
            build = Path(temp) / "build"
            capture = build / "pulp-threejs-provider-identity.png"

            def run(command: list[str]) -> str:
                if command[1:2] == ["tools/scripts/fetch_skia_for_release.py"]:
                    return "OK: immutable Skia cache generation ready at /cache\n"
                if any(item.endswith("provider_identity_test.cmake") for item in command):
                    capture.parent.mkdir(parents=True, exist_ok=True)
                    capture.write_bytes(b"png-proof")
                return "OK\n"

            with mock.patch.object(validator, "native_platform", return_value="darwin-arm64"), \
                 mock.patch.object(validator.skia_fetch, "manifest_asset", return_value=("mac-arm64", asset)), \
                 mock.patch.object(validator, "run_checked", side_effect=run), \
                 mock.patch.object(validator, "expected_v8_version", return_value="13.9.202.28"):
                result = validator.validate(Path(temp) / "cache", build, cache_only=False)

        self.assertEqual(result["mixed_provider"]["status"], "pass")
        self.assertEqual(result["mixed_provider"]["capture_bytes"], 9)


if __name__ == "__main__":
    unittest.main()

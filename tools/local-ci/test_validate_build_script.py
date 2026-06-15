#!/usr/bin/env python3
"""Tests for validate-build script contracts used by local CI."""

from __future__ import annotations

import os
import subprocess
import unittest
from pathlib import Path


VALIDATE_BUILD_PATH = Path(__file__).parent.parent.parent / "validate-build.sh"


class ValidateBuildScriptTests(unittest.TestCase):
    def test_preserves_original_args_for_lock_reexec(self) -> None:
        text = VALIDATE_BUILD_PATH.read_text()

        self.assertIn('ORIGINAL_ARGS=("$@")', text)
        self.assertIn('if ((${#ORIGINAL_ARGS[@]})); then', text)
        self.assertIn('acquire_validation_lock "${ORIGINAL_ARGS[@]}"', text)
        self.assertIn('else\n    acquire_validation_lock\nfi', text)

    def test_no_args_survives_strict_empty_array(self) -> None:
        env = os.environ.copy()
        env["PULP_VALIDATE_NO_LOCK"] = "1"
        env["PULP_EXPECT_SMOKE"] = "1"

        result = subprocess.run(
            ["bash", str(VALIDATE_BUILD_PATH)],
            cwd=VALIDATE_BUILD_PATH.parent,
            env=env,
            text=True,
            capture_output=True,
            check=False,
        )

        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("Smoke validation contract violated", result.stderr)
        self.assertNotIn("unbound variable", result.stderr)

    def test_uses_release_sdk_for_install_smoke(self) -> None:
        text = VALIDATE_BUILD_PATH.read_text()
        self.assertIn("-DCMAKE_BUILD_TYPE=Release", text)
        self.assertNotIn("-DCMAKE_BUILD_TYPE=Debug", text)

        ps1 = VALIDATE_BUILD_PATH.with_suffix(".ps1").read_text()
        self.assertIn('"-DCMAKE_BUILD_TYPE=Release"', ps1)
        self.assertIn("cmake --build $BuildDir --config Release", ps1)
        self.assertIn("cmake --install $BuildDir --prefix $InstallDir --config Release", ps1)
        self.assertIn("ctest --test-dir $BuildDir --output-on-failure -C Release", ps1)


if __name__ == "__main__":
    unittest.main()

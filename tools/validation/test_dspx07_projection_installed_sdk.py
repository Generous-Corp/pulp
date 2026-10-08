#!/usr/bin/env python3
"""Unit tests for the DSPX-07 installed-consumer validator."""

from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


HERE = Path(__file__).resolve().parent
SCRIPT = HERE / "dspx07_projection_installed_sdk.py"


class InstalledProjectionValidatorTests(unittest.TestCase):
    def test_missing_public_header_fails_closed(self) -> None:
        with tempfile.TemporaryDirectory(prefix="dspx07-validator-") as temp:
            sdk = Path(temp) / "sdk"
            (sdk / "lib/cmake/Pulp").mkdir(parents=True)
            result = subprocess.run(
                [sys.executable, str(SCRIPT), "--sdk", str(sdk), "--output", str(Path(temp) / "r.json")],
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("header is missing", result.stderr)

    def test_real_checkout_header_is_public(self) -> None:
        # This is a source-level guard for the package path. The full consumer
        # replay runs against a release SDK in the release/Forge lane.
        header = HERE.parent.parent / "core/format/include/pulp/format/projection_capability.hpp"
        self.assertTrue(header.is_file())
        self.assertIn("projection_capability", header.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()

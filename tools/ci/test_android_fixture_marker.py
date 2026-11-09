#!/usr/bin/env python3
"""Tests for tools/ci/android-fixture-marker.sh (pass-marker detection)."""

from __future__ import annotations

import subprocess
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
HELPER = HERE / "android-fixture-marker.sh"
LANE = HERE / "android-fixture-lane.sh"


def has_marker(output: str, marker: str) -> bool:
    # pipefail on, as in the lane script, so a broken pipe would show up.
    script = (
        "set -euo pipefail\n"
        f"source '{HELPER}'\n"
        'output_has_marker "$1" "$2"\n'
    )
    result = subprocess.run(
        ["bash", "-c", script, "bash", output, marker],
        capture_output=True,
        text=True,
        encoding="utf-8",
        check=False,
    )
    return result.returncode == 0


class MarkerDetectionTests(unittest.TestCase):
    def test_finds_marker(self) -> None:
        self.assertTrue(has_marker("a\npulp-fixture-runner: ok\nb\n", "pulp-fixture-runner:"))

    def test_missing_marker(self) -> None:
        self.assertFalse(has_marker("nothing here\n", "pulp-fixture-runner:"))

    def test_marker_at_start_of_large_output(self) -> None:
        # Early marker plus ~100 KB after it: the old pipe form failed here.
        big = "pulp-fixture-runner: 16 document(s) verified\n" + ("x" * 99 + "\n") * 1000
        self.assertTrue(has_marker(big, "pulp-fixture-runner:"))

    def test_lane_uses_helper_not_pipe(self) -> None:
        text = LANE.read_text(encoding="utf-8")
        self.assertIn("output_has_marker", text)
        self.assertNotIn("grep -Fq \"$required_marker\"", text)


if __name__ == "__main__":
    unittest.main()

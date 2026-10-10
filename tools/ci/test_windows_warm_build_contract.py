#!/usr/bin/env python3
"""Static contract checks for the reusable Windows warm-build helper."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools" / "ci" / "windows-warm-build.ps1"


class WindowsWarmBuildContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.text = SCRIPT.read_text(encoding="utf-8")

    def test_warm_path_is_fingerprinted_before_regeneration_suppression(self) -> None:
        self.assertIn("Get-SourceFingerprint", self.text)
        self.assertIn("CMAKE_SUPPRESS_REGENERATION=ON", self.text)
        self.assertIn("$state.fingerprint -ne $fingerprint", self.text)

    def test_incremental_build_has_bounded_parallelism(self) -> None:
        self.assertIn("[ValidateRange(1, 32)]", self.text)
        self.assertIn("'--parallel', $Jobs", self.text)
        self.assertNotIn("--parallel $([Environment]::ProcessorCount)", self.text)

    def test_release_and_short_platform_matrix_are_explicit(self) -> None:
        self.assertIn("'ARM64EC', 'ARM64', 'x64'", self.text)
        self.assertIn("'-DCMAKE_BUILD_TYPE=Release'", self.text)
        self.assertIn("'-DPULP_BUILD_EXAMPLES=OFF'", self.text)


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
import tempfile
import unittest
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).parent))
import check_vellum_sdk_staging as checker


class CheckVellumSdkStagingTests(unittest.TestCase):
    def make_prefix(self) -> Path:
        root = Path(self.tmp.name)
        (root / "include/pulp/vellum").mkdir(parents=True)
        (root / "lib").mkdir()
        (root / "lib/libpulp-vellum.a").write_bytes(b"archive")
        targets = root / "lib/cmake/Pulp/PulpTargets.cmake"
        targets.parent.mkdir(parents=True)
        targets.write_text("add_library(Pulp::vellum STATIC IMPORTED)\n", encoding="utf-8")
        return root

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_required_surface_passes(self) -> None:
        self.assertEqual(checker.check(self.make_prefix()), [])

    def test_missing_header_fails(self) -> None:
        prefix = self.make_prefix()
        for path in (prefix / "include/pulp/vellum").iterdir():
            path.unlink()
        (prefix / "include/pulp/vellum").rmdir()
        errors = checker.check(prefix)
        self.assertIn("include/pulp/vellum is missing", errors)


if __name__ == "__main__":
    unittest.main()

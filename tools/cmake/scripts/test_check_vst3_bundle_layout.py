#!/usr/bin/env python3
"""Positive and negative fixtures for Windows VST3 bundle verification."""

import json
import tempfile
import unittest
from pathlib import Path

from check_vst3_bundle_layout import check


class WindowsVst3BundleLayoutTest(unittest.TestCase):
    def make_bundle(self) -> tuple[Path, Path]:
        root = Path(tempfile.mkdtemp())
        bundle = root / "Probe.vst3"
        arch = bundle / "Contents" / "x86_64-win"
        arch.mkdir(parents=True)
        (arch / "Probe.vst3").write_bytes(b"module")
        (arch / "icudtl.dat").write_bytes(b"icu")
        (arch / "pulp-build-info.json").write_text(
            json.dumps({
                "schema": "pulp.build-info.v1",
                "bundled_runtime_libraries": [
                    {"file": "icudtl.dat", "component": "skia-icu-data"}
                ],
            }),
            encoding="utf-8",
        )
        return root, bundle

    def test_complete_x64_bundle_passes(self) -> None:
        _root, bundle = self.make_bundle()
        self.assertEqual(check(bundle), [])

    def test_declared_sidecar_missing_fails_closed(self) -> None:
        _root, bundle = self.make_bundle()
        (bundle / "Contents" / "x86_64-win" / "icudtl.dat").unlink()
        problems = check(bundle)
        self.assertTrue(any("icudtl.dat" in problem for problem in problems))

    def test_missing_manifest_fails_closed(self) -> None:
        _root, bundle = self.make_bundle()
        (bundle / "Contents" / "x86_64-win" / "pulp-build-info.json").unlink()
        self.assertTrue(any("pulp-build-info.json" in problem for problem in check(bundle)))


if __name__ == "__main__":
    unittest.main()

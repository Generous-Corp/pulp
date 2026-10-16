#!/usr/bin/env python3
"""Positive and negative fixtures for Windows VST3 bundle verification."""

import json
import tempfile
import unittest
from pathlib import Path

from check_vst3_bundle_layout import check


def find_spectr_build_info(build_dir: Path, architecture: str) -> Path | None:
    """Mirror Spectr's Find-SpectrBuildInfoSource candidate order."""
    candidates = (
        build_dir / "VST3" / "Spectr.vst3" / "Contents" / architecture / "Spectr.pulp-build-info.json",
        build_dir / "VST3" / "Release" / "Spectr.pulp-build-info.json",
        build_dir / "VST3" / "Spectr.pulp-build-info.json",
    )
    return next((candidate for candidate in candidates if candidate.is_file()), None)


class WindowsVst3BundleLayoutTest(unittest.TestCase):
    def make_bundle(self) -> tuple[Path, Path]:
        root = Path(tempfile.mkdtemp())
        bundle = root / "Probe.vst3"
        arch = bundle / "Contents" / "x86_64-win"
        arch.mkdir(parents=True)
        (arch / "Probe.vst3").write_bytes(b"module")
        (arch / "icudtl.dat").write_bytes(b"icu")
        (arch / "Probe.pulp-build-info.json").write_text(
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
        (bundle / "Contents" / "x86_64-win" / "Probe.pulp-build-info.json").unlink()
        self.assertTrue(any("Probe.pulp-build-info.json" in problem for problem in check(bundle)))

    def test_top_level_array_fails_diagnostically(self) -> None:
        _root, bundle = self.make_bundle()
        (bundle / "Contents" / "x86_64-win" / "Probe.pulp-build-info.json").write_text("[]\n")
        problems = check(bundle)
        self.assertTrue(any("must contain a JSON object" in problem for problem in problems))

    def test_null_runtime_library_list_fails_diagnostically(self) -> None:
        _root, bundle = self.make_bundle()
        (bundle / "Contents" / "x86_64-win" / "Probe.pulp-build-info.json").write_text(
            json.dumps({"schema": "pulp.build-info.v1", "bundled_runtime_libraries": None})
        )
        problems = check(bundle)
        self.assertTrue(any("must be a JSON list" in problem for problem in problems))

    def test_object_runtime_library_list_fails_diagnostically(self) -> None:
        _root, bundle = self.make_bundle()
        (bundle / "Contents" / "x86_64-win" / "Probe.pulp-build-info.json").write_text(
            json.dumps({"schema": "pulp.build-info.v1", "bundled_runtime_libraries": {}})
        )
        problems = check(bundle)
        self.assertTrue(any("must be a JSON list" in problem for problem in problems))

    def test_generic_filename_is_rejected_by_consumer_contract(self) -> None:
        _root, bundle = self.make_bundle()
        arch = bundle / "Contents" / "x86_64-win"
        (arch / "Probe.pulp-build-info.json").rename(arch / "pulp-build-info.json")
        problems = check(bundle)
        self.assertTrue(any("Probe.pulp-build-info.json" in problem for problem in problems))

    def test_spectr_consumer_discovers_module_base_filename(self) -> None:
        root = Path(tempfile.mkdtemp())
        sidecar = (root / "VST3" / "Spectr.vst3" / "Contents" /
                   "arm64ec-win" / "Spectr.pulp-build-info.json")
        sidecar.parent.mkdir(parents=True)
        sidecar.write_text("{}", encoding="utf-8")
        self.assertEqual(find_spectr_build_info(root, "arm64ec-win"), sidecar)

    def test_spectr_consumer_does_not_discover_generic_name(self) -> None:
        root = Path(tempfile.mkdtemp())
        sidecar = (root / "VST3" / "Spectr.vst3" / "Contents" /
                   "arm64ec-win" / "pulp-build-info.json")
        sidecar.parent.mkdir(parents=True)
        sidecar.write_text("{}", encoding="utf-8")
        self.assertIsNone(find_spectr_build_info(root, "arm64ec-win"))


if __name__ == "__main__":
    unittest.main()

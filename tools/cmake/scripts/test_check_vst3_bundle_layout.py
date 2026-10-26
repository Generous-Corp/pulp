#!/usr/bin/env python3
"""Positive and negative fixtures for Windows VST3 bundle verification."""

import json
import tempfile
import unittest
from pathlib import Path

from check_vst3_bundle_layout import check


class WindowsVst3BundleLayoutTest(unittest.TestCase):
    def make_bundle(self) -> tuple[Path, Path]:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
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

    def test_missing_runtime_library_list_fails_closed(self) -> None:
        _root, bundle = self.make_bundle()
        (bundle / "Contents" / "x86_64-win" / "Probe.pulp-build-info.json").write_text(
            json.dumps({"schema": "pulp.build-info.v1"}), encoding="utf-8"
        )
        self.assertTrue(any("missing bundled_runtime_libraries" in problem
                            for problem in check(bundle)))

    def test_top_level_array_fails_diagnostically(self) -> None:
        _root, bundle = self.make_bundle()
        (bundle / "Contents" / "x86_64-win" / "Probe.pulp-build-info.json").write_text(
            "[]\n", encoding="utf-8"
        )
        problems = check(bundle)
        self.assertTrue(any("must contain a JSON object" in problem for problem in problems))

    def test_null_runtime_library_list_fails_diagnostically(self) -> None:
        _root, bundle = self.make_bundle()
        (bundle / "Contents" / "x86_64-win" / "Probe.pulp-build-info.json").write_text(
            json.dumps({"schema": "pulp.build-info.v1", "bundled_runtime_libraries": None}),
            encoding="utf-8",
        )
        problems = check(bundle)
        self.assertTrue(any("must be a JSON list" in problem for problem in problems))

    def test_object_runtime_library_list_fails_diagnostically(self) -> None:
        _root, bundle = self.make_bundle()
        (bundle / "Contents" / "x86_64-win" / "Probe.pulp-build-info.json").write_text(
            json.dumps({"schema": "pulp.build-info.v1", "bundled_runtime_libraries": {}}),
            encoding="utf-8",
        )
        problems = check(bundle)
        self.assertTrue(any("must be a JSON list" in problem for problem in problems))

    def test_non_object_runtime_entry_fails_diagnostically(self) -> None:
        _root, bundle = self.make_bundle()
        (bundle / "Contents" / "x86_64-win" / "Probe.pulp-build-info.json").write_text(
            json.dumps({"schema": "pulp.build-info.v1", "bundled_runtime_libraries": [None]}),
            encoding="utf-8",
        )
        problems = check(bundle)
        self.assertTrue(any("entries must be JSON objects" in problem for problem in problems))

    def test_non_string_file_fails_diagnostically(self) -> None:
        _root, bundle = self.make_bundle()
        (bundle / "Contents" / "x86_64-win" / "Probe.pulp-build-info.json").write_text(
            json.dumps({"schema": "pulp.build-info.v1", "bundled_runtime_libraries": [{"file": 7}]}),
            encoding="utf-8",
        )
        problems = check(bundle)
        self.assertTrue(any("file must be a JSON string" in problem for problem in problems))

    def test_empty_file_fails_diagnostically(self) -> None:
        _root, bundle = self.make_bundle()
        (bundle / "Contents" / "x86_64-win" / "Probe.pulp-build-info.json").write_text(
            json.dumps({"schema": "pulp.build-info.v1", "bundled_runtime_libraries": [{"file": ""}]}),
            encoding="utf-8",
        )
        problems = check(bundle)
        self.assertTrue(any("non-empty basename" in problem for problem in problems))

    def test_path_file_fails_diagnostically(self) -> None:
        _root, bundle = self.make_bundle()
        manifest = bundle / "Contents" / "x86_64-win" / "Probe.pulp-build-info.json"
        for value in ("sub/icu.dat", "..", "../icu.dat", "/tmp/icu.dat", r"C:\\icu.dat"):
            manifest.write_text(json.dumps({
                "schema": "pulp.build-info.v1",
                "bundled_runtime_libraries": [{"file": value}],
            }), encoding="utf-8")
            problems = check(bundle)
            self.assertTrue(any("plain basename" in problem or "relative basename" in problem
                                for problem in problems), value)

    def test_generic_filename_is_rejected_by_consumer_contract(self) -> None:
        _root, bundle = self.make_bundle()
        arch = bundle / "Contents" / "x86_64-win"
        (arch / "Probe.pulp-build-info.json").rename(arch / "pulp-build-info.json")
        problems = check(bundle)
        self.assertTrue(any("Probe.pulp-build-info.json" in problem for problem in problems))

if __name__ == "__main__":
    unittest.main()

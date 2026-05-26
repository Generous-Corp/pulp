#!/usr/bin/env python3
"""Unit coverage for tools/scripts/verify_macos_cross_artifacts.py.

These tests build synthetic bundle trees and assert the verifier's
argument-parsing, expected-artifact mapping, and structural-error
behavior. They do not require a real Mach-O file because the actual
`file`/`otool` calls are unit-tested only behind an `--require-...`
flag that we leave OFF here. The aim is to lock in the Pulp-side
contract of the script so the private cross-infra repo can rely on it
without re-implementing the structural checks.

See planning/2026-05-24-linux-hosted-macos-arm64-cross-lane.md.
"""
from __future__ import annotations

import importlib.util
import pathlib
import plistlib
import sys
import tempfile
import unittest


SCRIPT = (
    pathlib.Path(__file__).resolve().parent / "verify_macos_cross_artifacts.py"
)
spec = importlib.util.spec_from_file_location(
    "verify_macos_cross_artifacts", SCRIPT,
)
assert spec and spec.loader
vmca = importlib.util.module_from_spec(spec)
sys.modules["verify_macos_cross_artifacts"] = vmca
spec.loader.exec_module(vmca)


def _make_bundle(
    root: pathlib.Path,
    bundle_name: str,
    executable_relpath: str,
    *,
    bundle_id: str = "ai.pulp.PulpGain",
    executable: str = "PulpGain",
) -> pathlib.Path:
    """Create a minimal Apple bundle layout under `root/bundle_name`."""
    bundle = root / bundle_name
    contents = bundle / "Contents"
    contents.mkdir(parents=True, exist_ok=True)
    info = contents / "Info.plist"
    with info.open("wb") as f:
        plistlib.dump(
            {
                "CFBundleIdentifier": bundle_id,
                "CFBundleExecutable": executable,
                "CFBundlePackageType": "BNDL",
            },
            f,
        )
    exe = bundle / executable_relpath
    exe.parent.mkdir(parents=True, exist_ok=True)
    exe.write_bytes(b"")
    return bundle


class ExpectedArtifactsTests(unittest.TestCase):
    def test_default_expect_covers_app_vst3_component_clap(self) -> None:
        args = vmca.parse_args([
            "/dev/null",
        ])
        expected = vmca.expected_artifacts(args)
        names = sorted(expected.keys())
        self.assertEqual(
            names,
            sorted(["PulpGain.app", "PulpGain.vst3", "PulpGain.component", "PulpGain.clap"]),
        )

    def test_unknown_kind_fails(self) -> None:
        # parse_args() is intentionally lazy and does not validate
        # `--expect` values; the failure lives in expected_artifacts()
        # so the script can return structured help for `--help`.
        args = vmca.parse_args(["/dev/null", "--expect", "app:not-a-thing"])
        with self.assertRaises(SystemExit):
            vmca.expected_artifacts(args)

    def test_auv3_expected_artifact_includes_appex_and_framework(self) -> None:
        args = vmca.parse_args([
            "/dev/null",
            "--expect", "auv3",
            "--product-name", "MyPlug",
        ])
        expected = vmca.expected_artifacts(args)
        self.assertIn("MyPlug-AUv3.app", expected)
        spec_ = expected["MyPlug-AUv3.app"]
        self.assertIn("appex", spec_)
        self.assertIn("framework", spec_)
        self.assertEqual(
            spec_["appex_executable"],
            "Contents/PlugIns/MyPlug.appex/Contents/MacOS/MyPlug",
        )


class MainStructuralTests(unittest.TestCase):
    def test_missing_artifact_directory_fails(self) -> None:
        with self.assertRaises(SystemExit):
            vmca.main(["/nonexistent/path/that/should/not/exist"])

    def test_missing_bundle_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            with self.assertRaises(SystemExit):
                vmca.main([tmpdir, "--expect", "clap"])

    def test_missing_info_plist_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            root = pathlib.Path(tmpdir)
            bundle = root / "PulpGain.clap"
            (bundle / "Contents" / "MacOS").mkdir(parents=True)
            (bundle / "Contents" / "MacOS" / "PulpGain").write_bytes(b"")
            with self.assertRaises(SystemExit):
                vmca.main([str(root), "--expect", "clap"])

    def test_missing_executable_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            root = pathlib.Path(tmpdir)
            bundle = root / "PulpGain.clap"
            (bundle / "Contents").mkdir(parents=True)
            with (bundle / "Contents" / "Info.plist").open("wb") as f:
                plistlib.dump({"CFBundleIdentifier": "ai.pulp.PulpGain"}, f)
            with self.assertRaises(SystemExit):
                vmca.main([str(root), "--expect", "clap"])

    def test_non_macho_executable_fails(self) -> None:
        # The verifier shells out to `file(1)` to confirm Mach-O arm64.
        # An empty file is "data" or "empty" and fails the check.
        with tempfile.TemporaryDirectory() as tmpdir:
            root = pathlib.Path(tmpdir)
            _make_bundle(root, "PulpGain.clap", "Contents/MacOS/PulpGain")
            with self.assertRaises(SystemExit):
                vmca.main([str(root), "--expect", "clap"])


if __name__ == "__main__":
    unittest.main()

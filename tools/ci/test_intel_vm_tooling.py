#!/usr/bin/env python3
"""Static contract tests for the Intel (x86_64) cross-build VM lane.

Covers the reproducible manifest (.shipyard/vm-image.intel.toml), the
tart-provision.sh manifest Intel-layer support, and the intel-vm-cross-build.sh
usage tool — all without spinning a VM. Run: python3 -m unittest
tools.ci.test_intel_vm_tooling
"""
import subprocess
import tomllib
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / ".shipyard" / "vm-image.intel.toml"
PROVISION = ROOT / "tools" / "ci" / "tart-provision.sh"
XBUILD = ROOT / "tools" / "ci" / "intel-vm-cross-build.sh"


class IntelManifest(unittest.TestCase):
    def setUp(self):
        with open(MANIFEST, "rb") as f:
            self.m = tomllib.load(f)

    def test_derives_from_runner_golden(self):
        # Must inherit Xcode/Skia/tools from the arm64 runner golden, not a bare base.
        self.assertEqual(self.m["base"], "pulp-build-runner:latest")
        self.assertEqual(self.m["name"], "pulp-intel-build")

    def test_declares_rust_x86_64_target(self):
        tc = self.m.get("toolchain", {})
        self.assertTrue(tc.get("rust"))
        self.assertIn("x86_64-apple-darwin", tc.get("rust_targets", []))

    def test_declares_rosetta(self):
        self.assertTrue(self.m.get("toolchain", {}).get("rosetta"))

    def test_does_not_redeclare_xcode(self):
        # Re-declaring Xcode would trigger a multi-hour re-provision; it's inherited.
        self.assertNotIn("xcode", self.m.get("toolchain", {}))


class ProvisionManifestSupport(unittest.TestCase):
    def setUp(self):
        self.text = PROVISION.read_text(encoding="utf-8")

    def test_parser_emits_intel_fields(self):
        for key in ("MF_RUST", "MF_RUST_TARGETS", "MF_ROSETTA"):
            self.assertIn(key, self.text)

    def test_installs_rust_targets_and_rosetta(self):
        self.assertIn("rustup target add", self.text)
        self.assertIn("install-rosetta", self.text)

    def test_manifest_read_reports_intel_layer(self):
        # Actually run the parser embedded in tart-provision.sh against the manifest.
        snippet = (
            "import sys, tomllib\n"
            "m = tomllib.load(open(sys.argv[1], 'rb'))\n"
            "tc = m.get('toolchain', {})\n"
            "print('rust', '1' if tc.get('rust') else '0')\n"
            "print('targets', ' '.join(tc.get('rust_targets', [])))\n"
            "print('rosetta', '1' if tc.get('rosetta') else '0')\n"
        )
        out = subprocess.run(
            ["python3", "-c", snippet, str(MANIFEST)],
            capture_output=True, text=True, check=True,
        ).stdout
        self.assertIn("rust 1", out)
        self.assertIn("targets x86_64-apple-darwin", out)
        self.assertIn("rosetta 1", out)


class CrossBuildScript(unittest.TestCase):
    def setUp(self):
        self.text = XBUILD.read_text(encoding="utf-8")

    def test_has_skia_clobber_fix(self):
        # The one cross-build gotcha MUST be in the recipe.
        self.assertIn("rm -rf external/skia-build/build", self.text)

    def test_cross_configures_x86_64(self):
        self.assertIn("-DCMAKE_OSX_ARCHITECTURES=x86_64", self.text)
        self.assertIn("PULP_RUST_CLI_TARGET=x86_64-apple-darwin", self.text)

    def test_verifies_thin_arch_and_rosetta_run(self):
        self.assertIn("lipo -archs", self.text)
        self.assertIn("arch -x86_64", self.text)
        self.assertIn("INTEL-BUILD-VERIFIED-OK", self.text)

    def test_discards_vm_by_default(self):
        # cleanup trap must delete the VM unless --keep.
        self.assertIn("trap cleanup EXIT", self.text)
        self.assertIn("tart delete", self.text)

    def test_bash_syntax_ok(self):
        subprocess.run(["bash", "-n", str(XBUILD)], check=True)


if __name__ == "__main__":
    unittest.main()

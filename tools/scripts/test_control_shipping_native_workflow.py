#!/usr/bin/env python3

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / ".github/workflows/control-shipping-native.yml"
SCANNER = ROOT / "tools/cmake/check_control_shipping_artifact.cmake"


class ControlShippingNativeWorkflowTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.workflow = WORKFLOW.read_text(encoding="utf-8")
        cls.scanner = SCANNER.read_text(encoding="utf-8")

    def test_one_matrix_covers_required_native_legs(self) -> None:
        for value in (
            "macos-universal",
            "linux-x64",
            "linux-arm64",
            "windows-x64",
            "ubuntu-24.04-arm",
            "windows-2022",
            "arm64;x86_64",
        ):
            self.assertIn(value, self.workflow)

    def test_installed_sdk_builds_real_format_targets(self) -> None:
        self.assertIn("tools/validation/sdk-smoke", self.workflow)
        self.assertIn("PULP_SDK_SMOKE_REQUIRE_FORMATS", self.workflow)
        self.assertIn("Standalone;VST3;CLAP;LV2;AU;AUv3", self.workflow)
        self.assertIn("AUv3Framework,AUv3Extension,AUv3Host", self.workflow)
        self.assertIn("verify_control_shipping_native_evidence.py", self.workflow)
        self.assertNotIn("cmake --build build-sdk --config Release --parallel", self.workflow)
        self.assertNotIn("cmake --build smoke/build --config Release --parallel", self.workflow)

    def test_linked_control_implementation_paths_trigger_the_matrix(self) -> None:
        for path in (
            "core/events/**",
            "core/format/**",
            "core/platform/**",
            "core/runtime/**",
            "core/state/**",
            "inspect/**",
            "tools/cli/**",
            "tools/mcp/**",
        ):
            self.assertEqual(self.workflow.count(f"- '{path}'"), 2)

    def test_aax_skip_cannot_be_mistaken_for_proof(self) -> None:
        self.assertIn("PULP_AAX_SDK_ZIP_URL", self.workflow)
        self.assertIn("PULP_AAX_SDK_ZIP_SHA256", self.workflow)
        self.assertIn("github.event_name == 'push'", self.workflow)
        self.assertIn("--aax-mode unavailable", self.workflow)
        self.assertIn("aax-sdk-secret-withheld-untrusted-event", self.workflow)
        self.assertIn('--aax-unavailable-reason "$reason"', self.workflow)

    def test_canonical_scanner_owns_native_tools(self) -> None:
        self.assertIn('find_program(_pulp_nm NAMES nm llvm-nm)', self.scanner)
        self.assertIn('find_program(_pulp_readelf NAMES readelf llvm-readelf)', self.scanner)
        self.assertIn('find_program(_pulp_dumpbin NAMES dumpbin.exe dumpbin', self.scanner)
        self.assertIn('/SYMBOLS /UNDNAME', self.scanner)
        self.assertIn('find_program(_pulp_lipo NAMES lipo)', self.scanner)


if __name__ == "__main__":
    unittest.main()

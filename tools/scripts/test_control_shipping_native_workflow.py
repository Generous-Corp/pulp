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
            "windows-arm64",
            "ubuntu-24.04-arm",
            "windows-2022",
            "windows-11-arm",
            "arm64;x86_64",
            '"ARM64"',
        ):
            self.assertIn(value, self.workflow)

        windows_leg = self.workflow.split('"leg": "windows-x64"', 1)[1].split(
            '"leg":', 1
        )[0]
        self.assertIn('"report_architectures": "x86_64-win"', windows_leg)

        windows_arm_leg = self.workflow.split('"leg": "windows-arm64"', 1)[1].split(
            '"leg":', 1
        )[0]
        self.assertIn('"os": "windows-11-arm"', windows_arm_leg)
        self.assertIn('"cmake_architectures": "ARM64"', windows_arm_leg)
        self.assertIn('"report_architectures": "arm64-win"', windows_arm_leg)

        self.assertGreaterEqual(
            self.workflow.count('if [ -n "${{ matrix.cmake_architectures }}" ]'),
            2,
        )

    def test_installed_sdk_builds_real_format_targets(self) -> None:
        self.assertIn("tools/validation/sdk-smoke", self.workflow)
        self.assertIn("PULP_SDK_SMOKE_REQUIRE_FORMATS", self.workflow)
        self.assertIn("Standalone;VST3;CLAP;LV2;AU;AUv3", self.workflow)
        self.assertIn("AUv3Framework,AUv3Extension,AUv3Host", self.workflow)
        self.assertIn("verify_control_shipping_native_evidence.py", self.workflow)
        self.assertNotIn("cmake --build build-sdk --config Release --parallel", self.workflow)
        self.assertNotIn("cmake --build smoke/build --config Release --parallel", self.workflow)

    def test_installed_sdk_carries_the_scanner_and_its_marker_list(self) -> None:
        # check_control_shipping_artifact.cmake reads the shared forbidden-marker
        # list through its module from its own directory; an SDK staged without
        # either fails closed at scan time, so the workflow asserts both at install.
        for installed in (
            "check_control_shipping_artifact.cmake",
            "PulpReleaseArtifactScan.cmake",
            "release_artifact_forbidden.json",
        ):
            self.assertIn(f'test -f "$prefix/lib/cmake/Pulp/{installed}"', self.workflow)
        self.assertIn("PulpReleaseArtifactScan.cmake", self.scanner)
        self.assertIn("release_artifact_forbidden.json", self.scanner)

    def test_gpu_audio_lifecycle_probe_executes_and_retains_receipt(self) -> None:
        self.assertIn("PulpSDKSmokeGpuAudioLifecycleProbe", self.workflow)
        self.assertIn("Execute installed-SDK GPU-audio lifecycle probe", self.workflow)
        self.assertIn("gpu-audio-lifecycle-receipt.json", self.workflow)
        self.assertIn('"dev.pulp.control/gpu-audio-lifecycle@1"', self.workflow)
        self.assertIn('"exit_code": status', self.workflow)
        self.assertIn('"source_head_sha": os.environ.get("PULP_SOURCE_HEAD_SHA", "")', self.workflow)
        self.assertIn('PULP_SOURCE_HEAD_SHA="${{ github.event.pull_request.head.sha || github.sha }}"', self.workflow)
        self.assertIn("Upload installed-SDK GPU-audio lifecycle receipt", self.workflow)
        self.assertIn("if: always()", self.workflow)

    def test_implementation_paths_do_not_restore_push_trigger(self) -> None:
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
            self.assertEqual(self.workflow.count(f"- '{path}'"), 0)

    def test_aax_skip_cannot_be_mistaken_for_proof(self) -> None:
        self.assertIn("PULP_AAX_SDK_ZIP_URL", self.workflow)
        self.assertIn("PULP_AAX_SDK_ZIP_SHA256", self.workflow)
        self.assertIn("github.event_name == 'push'", self.workflow)
        self.assertIn("--aax-mode unavailable", self.workflow)
        self.assertIn("aax-sdk-secret-withheld-untrusted-event", self.workflow)
        self.assertIn('--aax-unavailable-reason "$reason"', self.workflow)

    def test_aax_unavailable_configure_uses_a_nonempty_command_vector(self) -> None:
        configure_step = self.workflow.split(
            "- name: Configure real installed-SDK plug-in artifacts", 1
        )[1].split("- name: Build and scan real plug-in artifacts", 1)[0]
        self.assertIn("configure_args=(", configure_step)
        configure_initializer = configure_step.split("configure_args=(", 1)[1].split(
            "\n          )", 1
        )[0]
        self.assertIn("-S tools/validation/sdk-smoke", configure_initializer)
        self.assertIn("-B smoke/build", configure_initializer)
        self.assertIn("configure_args+=(-DPULP_ENABLE_AAX=ON)", configure_step)
        self.assertIn('cmake "${configure_args[@]}"', configure_step)
        self.assertNotIn("aax_args=()", configure_step)
        self.assertNotIn('"${aax_args[@]}"', configure_step)

    def test_canonical_scanner_owns_native_tools(self) -> None:
        self.assertIn('find_program(_pulp_nm NAMES nm llvm-nm)', self.scanner)
        self.assertIn('find_program(_pulp_readelf NAMES readelf llvm-readelf)', self.scanner)
        self.assertIn('find_program(_pulp_dumpbin NAMES dumpbin.exe dumpbin', self.scanner)
        self.assertIn('/SYMBOLS /UNDNAME', self.scanner)
        self.assertIn('find_program(_pulp_lipo NAMES lipo)', self.scanner)


if __name__ == "__main__":
    unittest.main()

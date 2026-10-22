#!/usr/bin/env python3
"""Self-test for gpu_fault_hooks_release_gate.py: a clean synthetic build
passes, and each way a fault hook can reach a build is planted and caught."""
from __future__ import annotations

import importlib.util
import os
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("gpu_fault_hooks_release_gate",
                                              HERE / "gpu_fault_hooks_release_gate.py")
GATE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GATE)


class ReleaseGate(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.build = Path(self._tmp.name)
        (self.build / "CMakeCache.txt").write_text(
            "CMAKE_BUILD_TYPE:STRING=Release\nPULP_BUILD_EXAMPLES:BOOL=ON\n", encoding="utf-8")
        (self.build / "build.ninja").write_text("build PulpGain_CLAP: phony\n", encoding="utf-8")
        lib = self.build / "core/gpu_audio/libpulp-gpu-audio.a"
        lib.parent.mkdir(parents=True)
        lib.write_bytes(b"!<arch>\n_ZN4pulp9gpu_audio12GpuConvolver7prepareEv\0")
        plugin = self.build / "CLAP/PulpGain.clap/Contents/MacOS/PulpGain"
        plugin.parent.mkdir(parents=True)
        plugin.write_bytes(b"\xcf\xfa\xed\xfe pulp gain")

    def tearDown(self):
        self._tmp.cleanup()

    def run_gate(self) -> int:
        return GATE.main(["--build-dir", str(self.build)])

    def test_a_clean_build_passes(self):
        self.assertEqual(GATE.check(self.build), [])
        self.assertEqual(self.run_gate(), 0)

    def test_the_hooks_option_is_caught(self):
        with (self.build / "CMakeCache.txt").open("a", encoding="utf-8") as cache:
            cache.write("PULP_GPU_AUDIO_FAULT_HOOKS:BOOL=ON\n")
        self.assertEqual(self.run_gate(), 1)

    def test_the_hooks_option_off_is_clean(self):
        with (self.build / "CMakeCache.txt").open("a", encoding="utf-8") as cache:
            cache.write("PULP_GPU_AUDIO_FAULT_HOOKS:BOOL=OFF\n")
        self.assertEqual(self.run_gate(), 0)

    def test_a_declared_test_target_is_caught(self):
        with (self.build / "build.ninja").open("a", encoding="utf-8") as ninja:
            ninja.write("build PulpGpuFaultContinuity_AU: phony\n")
        self.assertEqual(self.run_gate(), 1)

    def test_every_forbidden_identifier_is_caught_in_a_library(self):
        lib = self.build / "core/gpu_audio/libpulp-gpu-audio.a"
        clean = lib.read_bytes()
        for needle in GATE.FORBIDDEN:
            with self.subTest(needle=needle):
                lib.write_bytes(clean + b"_ZN4pulp" + needle + b"Ev\0")
                findings = GATE.check(self.build)
                self.assertEqual(len(findings), 1)
                self.assertIn(needle.decode(), findings[0])
        lib.write_bytes(clean)
        self.assertEqual(GATE.check(self.build), [])

    def test_a_hook_inside_a_plugin_bundle_is_caught(self):
        binary = self.build / "AU/Some.component/Contents/MacOS/Some"
        binary.parent.mkdir(parents=True)
        binary.write_bytes(b"\xcf\xfa\xed\xfe resolve_run_config")
        self.assertEqual(self.run_gate(), 1)

    def test_a_hook_in_a_plain_executable_is_caught(self):
        exe = self.build / "tools/some-host"
        exe.parent.mkdir(parents=True)
        exe.write_bytes(b"\xcf\xfa\xed\xfe arm_provider_fault")
        os.chmod(exe, 0o755)
        self.assertEqual(self.run_gate(), 1)

    def test_the_test_au_subtype_in_a_bundle_plist_is_caught(self):
        plist = self.build / "AU/Other.component/Contents/Info.plist"
        plist.parent.mkdir(parents=True)
        import plistlib
        info = {"CFBundleIdentifier": "com.example.other",
                "AudioComponents": [{"type": "aufx", "subtype": "PGan", "manufacturer": "Pulp"}]}
        plist.write_bytes(plistlib.dumps(info))
        self.assertEqual(self.run_gate(), 0)
        info["AudioComponents"][0]["subtype"] = "GpFc"
        plist.write_bytes(plistlib.dumps(info))
        findings = GATE.check(self.build)
        self.assertEqual(len(findings), 1)
        self.assertIn("AU subtype GpFc", findings[0])

    def test_the_four_character_code_alone_in_a_binary_is_not_a_finding(self):
        lib = self.build / "core/gpu_audio/libpulp-gpu-audio.a"
        lib.write_bytes(lib.read_bytes() + b"\x00GpFc\x00")
        self.assertEqual(GATE.check(self.build), [])

    def test_the_bundle_name_in_a_vst3_moduleinfo_is_caught(self):
        info = self.build / "VST3/X.vst3/Contents/Resources/moduleinfo.json"
        info.parent.mkdir(parents=True)
        info.write_text('{"Name": "x", "Bundle": "com.pulp.gpu-fault-continuity"}',
                        encoding="utf-8")
        self.assertEqual(self.run_gate(), 1)

    def test_not_a_build_directory(self):
        (self.build / "CMakeCache.txt").unlink()
        self.assertEqual(self.run_gate(), 2)


if __name__ == "__main__":
    unittest.main()

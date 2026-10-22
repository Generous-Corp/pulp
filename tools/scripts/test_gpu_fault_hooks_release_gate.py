#!/usr/bin/env python3
"""Self-test for gpu_fault_hooks_release_gate.py: a clean synthetic build
passes, and each way a fault hook can reach a build is planted and caught."""
from __future__ import annotations

import importlib.util
import os
import shutil
import subprocess
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

    def test_every_cmake_true_spelling_of_the_option_is_caught(self):
        base = (self.build / "CMakeCache.txt").read_text(encoding="utf-8")
        for value in ("ON", "on", "Y", "yes", "TRUE", "1", "2"):
            with self.subTest(value=value):
                (self.build / "CMakeCache.txt").write_text(
                    base + f"PULP_GPU_AUDIO_FAULT_HOOKS:BOOL={value}\n", encoding="utf-8")
                self.assertEqual(self.run_gate(), 1)
        for value in ("OFF", "0", "NO", "FALSE", "N", ""):
            with self.subTest(value=value):
                (self.build / "CMakeCache.txt").write_text(
                    base + f"PULP_GPU_AUDIO_FAULT_HOOKS:BOOL={value}\n", encoding="utf-8")
                self.assertEqual(self.run_gate(), 0)

    def test_other_generators_manifests_are_read(self):
        for name in ("build-Release.ninja", "Pulp.sln"):
            with self.subTest(name=name):
                path = self.build / name
                path.write_text("PulpGpuFaultContinuity_CLAP\n", encoding="utf-8")
                self.assertEqual(self.run_gate(), 1)
                path.unlink()
        self.assertEqual(self.run_gate(), 0)

    def test_an_extra_directory_is_scanned(self):
        with tempfile.TemporaryDirectory() as extra:
            plugin = Path(extra) / "Components/X.component/Contents/MacOS/X"
            plugin.parent.mkdir(parents=True)
            plugin.write_bytes(b"\xcf\xfa\xed\xfe arm_provider_fault")
            self.assertEqual(self.run_gate(), 0)
            self.assertEqual(GATE.main(["--build-dir", str(self.build), "--extra-dir", extra]),
                             1)

    def _staged(self, planted: bool) -> Path:
        stage = self.build.parent / (self.build.name + ("-dirty" if planted else "-clean"))
        binary = stage / "Payload/X.clap/Contents/MacOS/X"
        binary.parent.mkdir(parents=True, exist_ok=True)
        binary.write_bytes(b"\xcf\xfa\xed\xfe " + (b"resolve_run_config" if planted else b"ok"))
        self.addCleanup(shutil.rmtree, stage, True)
        return stage

    def test_zip_and_tar_archives_are_opened_and_scanned(self):
        for planted in (False, True):
            stage = self._staged(planted)
            for kind in ("zip", "gztar"):
                with self.subTest(planted=planted, kind=kind):
                    out = shutil.make_archive(str(self.build / f"dist-{kind}"), kind, stage)
                    self.assertEqual(self.run_gate(), 1 if planted else 0)
                    Path(out).unlink()

    def test_a_zip_inside_a_zip_is_scanned(self):
        stage = self._staged(True)
        inner = shutil.make_archive(str(self.build.parent / (self.build.name + "-inner")), "zip",
                                    stage)
        self.addCleanup(Path(inner).unlink)
        outer_stage = self.build.parent / (self.build.name + "-outer-stage")
        outer_stage.mkdir()
        self.addCleanup(shutil.rmtree, outer_stage, True)
        shutil.copy(inner, outer_stage / "inner.zip")
        shutil.make_archive(str(self.build / "outer"), "zip", outer_stage)
        self.assertEqual(self.run_gate(), 1)

    def test_an_unreadable_archive_fails_closed(self):
        (self.build / "broken.zip").write_bytes(b"not a zip")
        findings = GATE.check(self.build)
        self.assertEqual(len(findings), 1)
        self.assertIn("could not be opened", findings[0])

    def test_a_zip_member_escaping_the_archive_is_refused(self):
        import zipfile
        with zipfile.ZipFile(self.build / "slip.zip", "w") as z:
            z.writestr("../../outside.bin", b"x")
        findings = GATE.check(self.build)
        self.assertEqual(len(findings), 1)
        self.assertIn("escapes the archive", findings[0])

    def _tar_with_link(self) -> Path:
        import tarfile
        path = self.build / "links.tar"
        with tarfile.open(path, "w") as tar:
            info = tarfile.TarInfo("evil")
            info.type = tarfile.SYMTYPE
            info.linkname = "/etc/passwd"
            tar.addfile(info)
        return path

    def test_a_tar_link_is_refused_with_the_data_filter(self):
        self._tar_with_link()
        findings = GATE.check(self.build)
        self.assertEqual(len(findings), 1)
        self.assertIn("could not be opened", findings[0])

    def test_a_tar_link_is_refused_without_the_data_filter(self):
        import tarfile
        self._tar_with_link()
        saved = getattr(tarfile, "data_filter", None)
        if saved is not None:
            del tarfile.data_filter
            self.addCleanup(setattr, tarfile, "data_filter", saved)
        findings = GATE.check(self.build)
        self.assertEqual(len(findings), 1)
        self.assertIn("is a link or escapes", findings[0])

    def test_a_symlinked_bundle_is_followed(self):
        with tempfile.TemporaryDirectory() as elsewhere:
            target = Path(elsewhere) / "X.component" / "Contents" / "MacOS"
            target.mkdir(parents=True)
            (target / "X").write_bytes(b"\xcf\xfa\xed\xfe arm_provider_fault")
            components = self.build / "Components"
            components.mkdir()
            (components / "X.component").symlink_to(Path(elsewhere) / "X.component")
            self.assertEqual(self.run_gate(), 1)

    def test_a_symlink_loop_terminates(self):
        loop = self.build / "loop"
        loop.mkdir()
        (loop / "again").symlink_to(loop)
        self.assertEqual(self.run_gate(), 0)

    def test_an_unreadable_binary_fails_closed(self):
        lib = self.build / "core/gpu_audio/libpulp-gpu-audio.a"
        os.chmod(lib, 0)
        try:
            if os.access(lib, os.R_OK):
                self.skipTest("running with privileges that ignore file modes")
            findings = GATE.check(self.build)
        finally:
            os.chmod(lib, 0o644)
        self.assertEqual(len(findings), 1)
        self.assertIn("unreadable", findings[0])

    def test_extra_paths_never_report_clean_by_mistake(self):
        missing = self.build.parent / (self.build.name + "-missing")
        self.assertEqual(GATE.main(["--build-dir", str(self.build), "--extra-dir",
                                    str(missing)]), 1)
        plain = self.build / "notes.txt"
        plain.write_text("x", encoding="utf-8")
        self.assertEqual(GATE.main(["--build-dir", str(self.build), "--extra-dir",
                                    str(plain)]), 1)
        stage = self.build.parent / (self.build.name + "-stage")
        (stage / "X.clap/Contents/MacOS").mkdir(parents=True)
        (stage / "X.clap/Contents/MacOS/X").write_bytes(b"resolve_run_config")
        self.addCleanup(shutil.rmtree, stage, True)
        archive = shutil.make_archive(str(self.build.parent / (self.build.name + "-dist")),
                                      "zip", stage)
        self.addCleanup(os.unlink, archive)
        self.assertEqual(GATE.main(["--build-dir", str(self.build), "--extra-dir",
                                    archive]), 1)

    def test_not_a_build_directory(self):
        (self.build / "CMakeCache.txt").unlink()
        self.assertEqual(self.run_gate(), 2)


if __name__ == "__main__":
    unittest.main()

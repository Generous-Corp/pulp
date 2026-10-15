#!/usr/bin/env python3
"""Configure-time contract for the Windows ARM64 Skia archive layout.

The published Windows ARM64 slice stores ``skia.lib`` and
``dawn_combined.lib`` below ``build/win-gpu/lib/Release/arm64`` while x64 is
flat.  This test uses CMake's Windows platform emulation with a compiler-free
``project(... NONE)`` fixture, so it runs on the normal macOS/Linux gates too.
"""
from __future__ import annotations

import pathlib
import shutil
import subprocess
import tempfile
import unittest


REPO = pathlib.Path(__file__).resolve().parents[2]
FINDSKIA = REPO / "tools/cmake/FindSkia.cmake"
WINDOWS_PROVIDER = REPO / "tools/cmake/PulpWindowsSkiaProvider.cmake"


class FindSkiaWindowsArm64Slice(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def _make_fixture(self, *, arm64: bool = False, arm64ec: bool = False,
                      arm64x: bool = False, flat: bool = False,
                      dawn: bool = True) -> pathlib.Path:
        name = "skia-" + "-".join(n for n, on in (("arm64", arm64),
                                                   ("arm64ec", arm64ec),
                                                   ("arm64x", arm64x),
                                                   ("flat", flat)) if on)
        skia = self.tmp / name
        release = skia / "build" / "win-gpu" / "lib" / "Release"
        (skia / "build" / "include").mkdir(parents=True, exist_ok=True)
        # FindSkia only needs archive-shaped paths for this configure test; the
        # real linkable contents are supplied by skia-builder.
        for libdir, present in ((release / "arm64", arm64),
                                (release / "arm64ec", arm64ec),
                                (release / "arm64x", arm64x),
                                (release, flat)):
            if present:
                libdir.mkdir(parents=True, exist_ok=True)
                (libdir / "skia.lib").write_bytes(b"fixture\n")
                if dawn:
                    (libdir / "dawn_combined.lib").write_bytes(b"fixture\n")
        return skia

    def _configure(self, skia: pathlib.Path, *, target_arch: str, expect_found: bool,
                   expect_slice: str | None = None, host: str = "ARM64",
                   expect_provider: bool | None = None) -> str:
        """Configure a compiler-free project for ``target_arch`` on ``host``.

        A real MSVC configure records the target in
        CMAKE_CXX_COMPILER_ARCHITECTURE_ID; the NONE project sets it directly,
        while CMAKE_SYSTEM_PROCESSOR stays the host, as in a Visual Studio
        configure.
        """
        project = self.tmp / f"project-{len(list(self.tmp.iterdir()))}"
        project.mkdir()
        expected = "TRUE" if expect_found else "FALSE"
        checks = ""
        if expect_provider is not None:
            provider = "TRUE" if expect_provider else "FALSE"
            checks += (f'if(NOT PULP_SKIA_PROVIDES_DAWN STREQUAL "{provider}")\n'
                       f'  message(FATAL_ERROR "unexpected PULP_SKIA_PROVIDES_DAWN=${{PULP_SKIA_PROVIDES_DAWN}} DAWN_LIBRARY=${{DAWN_LIBRARY}}")\n'
                       "endif()\n")
        if expect_slice in {"arm64", "arm64ec", "arm64x"}:
            checks += (f'if(NOT SKIA_LIBRARY MATCHES "/{expect_slice}/skia\\.lib$")\n'
                      f'  message(FATAL_ERROR "{expect_slice} archive slice was not selected: ${{SKIA_LIBRARY}}")\n'
                      "endif()\n")
        elif expect_slice == "flat":
            checks = ('if(SKIA_LIBRARY MATCHES "/arm64/")\n'
                      '  message(FATAL_ERROR "ARM64 archive slice selected for a non-ARM64 target: ${SKIA_LIBRARY}")\n'
                      "endif()\n")
        (project / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.20)\n"
            "project(findskia_windows_arm64 NONE)\n"
            f'set(PULP_ROOT_DIR "{REPO.as_posix()}")\n'
            f'set(SKIA_DIR "{skia.as_posix()}")\n'
            f'include("{FINDSKIA.as_posix()}")\n'
            f'if(NOT SKIA_FOUND STREQUAL "{expected}")\n'
            "  message(FATAL_ERROR \"unexpected SKIA_FOUND=${SKIA_FOUND}\")\n"
            "endif()\n" + checks,
            encoding="utf-8",
        )
        result = subprocess.run(
            [
                "cmake",
                "-S",
                str(project),
                "-B",
                str(project / "build"),
                "-DCMAKE_SYSTEM_NAME=Windows",
                f"-DCMAKE_SYSTEM_PROCESSOR={host}",
                f"-DCMAKE_CXX_COMPILER_ARCHITECTURE_ID={target_arch}",
            ],
            check=False,
            capture_output=True,
            text=True,
            encoding="utf-8",
        )
        output = result.stdout + result.stderr
        self.assertEqual(result.returncode, 0, output)
        return output

    def test_arm64_slice_is_selected(self) -> None:
        output = self._configure(self._make_fixture(arm64=True), target_arch="ARM64",
                                 expect_found=True, expect_slice="arm64",
                                 expect_provider=True)
        self.assertIn("selecting Windows ARM64 archive slice", output)

    def test_flat_only_fixture_does_not_claim_arm64_support(self) -> None:
        self._configure(self._make_fixture(arm64=False, flat=True), target_arch="ARM64",
                        expect_found=False)

    def test_x64_target_on_an_arm64_host_keeps_the_flat_layout(self) -> None:
        # CMAKE_SYSTEM_PROCESSOR is the ARM64 host; the target is x64.
        output = self._configure(self._make_fixture(arm64=True, flat=True), target_arch="x64",
                                 expect_found=True, expect_slice="flat")
        self.assertNotIn("selecting Windows ARM64 archive slice", output)

    def test_arm64ec_and_arm64x_targets_select_matching_slices(self) -> None:
        for arch, slice_name, kwargs in (("ARM64EC", "arm64ec", {"arm64ec": True}),
                                         ("ARM64X", "arm64x", {"arm64x": True})):
            with self.subTest(target=arch):
                output = self._configure(self._make_fixture(**kwargs),
                                         target_arch=arch, expect_found=True,
                                         expect_slice=slice_name)
                self.assertIn(f"selecting Windows {slice_name} archive slice", output)

    def test_arm64ec_without_matching_slice_fails_closed(self) -> None:
        output = self._configure(self._make_fixture(arm64=True, flat=True),
                                 target_arch="ARM64EC", expect_found=False)
        self.assertIn("no Windows archive slice for arm64ec-win", output)

    def test_matching_skia_without_dawn_records_no_provider(self) -> None:
        output = self._configure(self._make_fixture(arm64ec=True, dawn=False),
                                 target_arch="ARM64EC", expect_found=True,
                                 expect_slice="arm64ec", expect_provider=False)
        self.assertIn("selecting Windows arm64ec archive slice", output)

    def _configure_provider_policy(self, target_arch: str, provider: bool) -> subprocess.CompletedProcess[str]:
        project = self.tmp / f"provider-{target_arch}-{provider}"
        project.mkdir()
        expected_skip = "TRUE" if target_arch.upper() in {"ARM64EC", "ARM64X"} else "FALSE"
        provider_value = "TRUE" if provider else "FALSE"
        (project / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.20)\n"
            "project(windows_skia_provider NONE)\n"
            f'include("{WINDOWS_PROVIDER.as_posix()}")\n'
            "pulp_windows_skia_provider_target(_target)\n"
            "pulp_windows_skia_provider_requires_skia_dawn(${_target} _skip)\n"
            "set(PULP_ENABLE_GPU TRUE)\n"
            "pulp_windows_skia_provider_should_declare_wgpu(_declare)\n"
            f'if(NOT _skip STREQUAL "{expected_skip}")\n'
            "  message(FATAL_ERROR \"unexpected skip=${_skip} target=${_target}\")\n"
            "endif()\n"
            f'if(NOT _declare STREQUAL "{"FALSE" if expected_skip == "TRUE" else "TRUE"}")\n'
            "  message(FATAL_ERROR \"unexpected declare=${_declare}\")\n"
            "endif()\n"
            f'set(PULP_SKIA_PROVIDES_DAWN {provider_value})\n'
            "pulp_windows_skia_provider_require_dawn(${_target})\n"
            + ("if(NOT PULP_HAS_WEBGPU)\n  message(FATAL_ERROR \"Skia Dawn did not enable PULP_HAS_WEBGPU\")\nendif()\n"
               if provider and expected_skip == "TRUE" else "")
            , encoding="utf-8")
        return subprocess.run(
            ["cmake", "-S", str(project), "-B", str(project / "build"),
             "-DCMAKE_SYSTEM_NAME=Windows",
             f"-DCMAKE_CXX_COMPILER_ARCHITECTURE_ID={target_arch}"],
            check=False, capture_output=True, text=True, encoding="utf-8")

    def test_provider_policy_skips_wgpu_and_enables_webgpu_for_matching_dawn(self) -> None:
        for arch in ("ARM64EC", "ARM64X"):
            with self.subTest(target=arch):
                result = self._configure_provider_policy(arch, provider=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_provider_policy_fails_closed_without_dawn(self) -> None:
        result = self._configure_provider_policy("ARM64EC", provider=False)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("requires a matching Skia/Dawn archive", result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()

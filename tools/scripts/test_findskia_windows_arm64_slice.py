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


class FindSkiaWindowsArm64Slice(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def _make_fixture(self, *, arm64: bool) -> pathlib.Path:
        skia = self.tmp / ("skia-arm64" if arm64 else "skia-flat")
        libdir = skia / "build" / "win-gpu" / "lib" / "Release"
        (skia / "build" / "include").mkdir(parents=True)
        if arm64:
            libdir = libdir / "arm64"
        libdir.mkdir(parents=True)
        # FindSkia only needs archive-shaped paths for this configure test; the
        # real linkable contents are supplied by skia-builder.
        for name in ("skia.lib", "dawn_combined.lib"):
            (libdir / name).write_bytes(b"arm64 fixture\n")
        return skia

    def _configure(self, skia: pathlib.Path, expect_found: bool) -> str:
        project = self.tmp / ("project-arm64" if expect_found else "project-flat")
        project.mkdir()
        expected = "TRUE" if expect_found else "FALSE"
        (project / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.20)\n"
            "project(findskia_windows_arm64 NONE)\n"
            f'set(PULP_ROOT_DIR "{REPO.as_posix()}")\n'
            f'set(SKIA_DIR "{skia.as_posix()}")\n'
            f'include("{FINDSKIA.as_posix()}")\n'
            f'if(NOT SKIA_FOUND STREQUAL "{expected}")\n'
            "  message(FATAL_ERROR \"unexpected SKIA_FOUND=${SKIA_FOUND}\")\n"
            "endif()\n"
            + (
                'if(NOT SKIA_LIBRARY MATCHES "/arm64/skia\\.lib$")\n'
                '  message(FATAL_ERROR "ARM64 archive slice was not selected: ${SKIA_LIBRARY}")\n'
                "endif()\n"
                if expect_found
                else ""
            )
        )
        build = project / "build"
        result = subprocess.run(
            [
                "cmake",
                "-S",
                str(project),
                "-B",
                str(build),
                "-DCMAKE_SYSTEM_NAME=Windows",
                "-DCMAKE_SYSTEM_PROCESSOR=ARM64",
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        output = result.stdout + result.stderr
        self.assertEqual(result.returncode, 0, output)
        return output

    def test_arm64_slice_is_selected(self) -> None:
        output = self._configure(self._make_fixture(arm64=True), expect_found=True)
        self.assertIn("selecting Windows ARM64 archive slice", output)

    def test_flat_only_fixture_does_not_claim_arm64_support(self) -> None:
        self._configure(self._make_fixture(arm64=False), expect_found=False)


if __name__ == "__main__":
    unittest.main()

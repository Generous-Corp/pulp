#!/usr/bin/env python3
"""Configure-time test for the macOS arch assertion in tools/cmake/FindSkia.cmake.

FindSkia now fails LOUD at configure time when the resolved mac Skia archive
does not contain every requested target architecture (G3). This drives a real
`cmake` configure of a tiny project that `include()`s FindSkia.cmake against a
fixture whose libskia.a is a THIN x86_64 stub archive (built here with
clang/ar/lipo), and asserts:

  * -DCMAKE_OSX_ARCHITECTURES=arm64  → configure FAILS with "architecture
    mismatch" (the lib is x86_64-only), and
  * -DCMAKE_OSX_ARCHITECTURES=x86_64 → configure SUCCEEDS (arch satisfied).

Skips cleanly when not on macOS or the toolchain (clang/ar/lipo/cmake) is
absent, so non-Apple CI does not fail on it.

Run:
    python3 tools/scripts/test_findskia_arch_assert.py
"""
from __future__ import annotations

import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
FINDSKIA = REPO / "tools/cmake/FindSkia.cmake"


def _toolchain_ready():
    return (os.uname().sysname == "Darwin"
            and all(shutil.which(t) for t in ("clang", "ar", "lipo", "cmake")))


@unittest.skipUnless(_toolchain_ready(), "macOS + clang/ar/lipo/cmake required")
class FindSkiaArchAssert(unittest.TestCase):
    def setUp(self):
        self.tmp = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

        # Fixture Skia tree: a THIN x86_64 stub for libskia.a + libdawn_combined.a
        # at the layout FindSkia probes (build/mac-gpu/lib/Release/).
        self.skia = self.tmp / "skiafix"
        libdir = self.skia / "build" / "mac-gpu" / "lib" / "Release"
        libdir.mkdir(parents=True)
        (self.skia / "build" / "include").mkdir(parents=True)  # so SKIA_FOUND can pass
        src = self.tmp / "s.c"
        src.write_text("int pulp_skia_stub(void){return 0;}\n")
        obj = self.tmp / "s.o"
        subprocess.run(["clang", "-arch", "x86_64", "-c", str(src), "-o", str(obj)],
                       check=True, capture_output=True)
        for lib in ("libskia.a", "libdawn_combined.a"):
            subprocess.run(["ar", "crs", str(libdir / lib), str(obj)],
                           check=True, capture_output=True)
        # Sanity: the stub really is x86_64-only.
        archs = subprocess.run(["lipo", "-archs", str(libdir / "libskia.a")],
                               capture_output=True, text=True).stdout.split()
        self.assertEqual(archs, ["x86_64"])

        # Tiny project that includes FindSkia.cmake.
        self.proj = self.tmp / "proj"
        self.proj.mkdir()
        (self.proj / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.20)\n"
            "project(findskia_arch_assert NONE)\n"
            f'set(SKIA_DIR "{self.skia.as_posix()}")\n'
            f'include("{FINDSKIA.as_posix()}")\n')

    def _configure(self, arch):
        build = self.proj / f"build-{arch}"
        r = subprocess.run(
            ["cmake", "-S", str(self.proj), "-B", str(build),
             f"-DCMAKE_OSX_ARCHITECTURES={arch}"],
            capture_output=True, text=True)
        return r.returncode, (r.stdout + r.stderr)

    def test_wrong_arch_fails_configure(self):
        rc, out = self._configure("arm64")
        self.assertNotEqual(rc, 0, msg=f"configure unexpectedly succeeded:\n{out}")
        # The FindSkia FATAL names the missing arch and the target arch.
        self.assertIn("missing: arm64", out,
                      msg=f"expected the arch assertion to fire:\n{out}")
        self.assertIn("targets 'arm64'", out)

    def test_matching_arch_configures(self):
        rc, out = self._configure("x86_64")
        self.assertEqual(rc, 0, msg=f"configure should succeed for matching arch:\n{out}")
        self.assertNotIn("missing:", out)


if __name__ == "__main__":
    unittest.main()

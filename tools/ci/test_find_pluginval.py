#!/usr/bin/env python3
"""_pulp_find_pluginval (tools/cmake/PulpFindPluginval.cmake) resolves the real binary.

pluginval ships on macOS as an app bundle. CMake's app-bundle search answers
the second lookup in a configure with the bundle's Contents/MacOS directory
prefixed twice, a path that does not exist, so plug-in validation tests after
the first registered no command. This configures a project against a fake
pluginval.app found through CMAKE_APPBUNDLE_PATH and reads what CMake resolved:

- two plain app-bundle lookups (the control) show whether this CMake doubles
  the path, which the helper must survive either way;
- every helper call, including those after an app-bundle lookup, resolves to
  exactly <bundle>/Contents/MacOS/pluginval, never re-prefixed, and the file
  exists;
- a cache already holding a path that names no file (a configure poisoned by
  the doubled answer) heals to the real binary on the next configure.

Run:
    python3 tools/ci/test_find_pluginval.py
"""
from __future__ import annotations

import os
import re
import shutil
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
MODULE = ROOT / "tools/cmake/PulpFindPluginval.cmake"

PROJECT = """cmake_minimum_required(VERSION 3.24)
project(find_pluginval NONE)
include("{module}")
find_program(RAW1 pluginval)
find_program(RAW2 pluginval)
_pulp_find_pluginval(HELPED1)
_pulp_find_pluginval(HELPED2)
foreach(v RAW1 RAW2 HELPED1 HELPED2)
  message(STATUS "RESOLVED ${{v}}=${{${{v}}}}")
endforeach()
"""


@unittest.skipUnless(shutil.which("cmake"), "needs cmake")
class FindPluginvalTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.bundles = root / "Apps"
        macos = self.bundles / "pluginval.app" / "Contents" / "MacOS"
        macos.mkdir(parents=True)
        self.binary = macos / "pluginval"
        self.binary.write_text("#!/bin/sh\nexit 0\n")
        self.binary.chmod(self.binary.stat().st_mode | stat.S_IXUSR)
        self.src, self.build = root / "src", root / "build"
        self.src.mkdir()
        (self.src / "CMakeLists.txt").write_text(PROJECT.format(module=MODULE.as_posix()))

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def configure(self, *extra: str) -> dict[str, str]:
        env = {k: v for k, v in os.environ.items() if not k.startswith("CMAKE_")}
        # Keep the host's own pluginval (PATH, ~/Applications) out of the search.
        env["PATH"] = "/usr/bin:/bin"
        proc = subprocess.run([shutil.which("cmake"), "-S", str(self.src), "-B", str(self.build),
                               f"-DCMAKE_APPBUNDLE_PATH={self.bundles}",
                               "-DCMAKE_SYSTEM_APPBUNDLE_PATH=", *extra],
                              capture_output=True, text=True, timeout=120, env=env)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        return dict(re.findall(r"RESOLVED (\w+)=(.*)", proc.stdout))

    def test_every_helper_lookup_resolves_the_binary_itself(self) -> None:
        got = self.configure()
        expected = str(self.binary)
        for name in ("HELPED1", "HELPED2"):
            self.assertEqual(os.path.realpath(got[name]), os.path.realpath(expected), name)
            self.assertNotIn("Contents/MacOS/" + str(self.bundles).lstrip("/"), got[name])
            self.assertTrue(Path(got[name]).is_file(), name)
        # Control: the fake bundle is visible to CMake's own app-bundle search,
        # so the helper runs after real app-bundle lookups.
        self.assertTrue(got["RAW1"].endswith("pluginval"), got)

    def test_a_cached_path_that_names_no_file_heals(self) -> None:
        doubled = f"{self.binary.parent}{self.binary.parent}/pluginval"
        got = self.configure(f"-DHELPED1={doubled}", f"-DHELPED2={doubled}")
        for name in ("HELPED1", "HELPED2"):
            self.assertEqual(os.path.realpath(got[name]), os.path.realpath(self.binary), name)


if __name__ == "__main__":
    unittest.main()

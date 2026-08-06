#!/usr/bin/env python3
"""Behavioral contract for dependency-bound Rack package generation."""

from pathlib import Path
import hashlib
import subprocess
import sys
import tempfile
import time
import unittest


HERE = Path(__file__).resolve().parent
PULP_RACK = HERE / "PulpRack.cmake"


class PulpRackPackageTest(unittest.TestCase):
    @unittest.skipUnless(sys.platform == "darwin", "uses macOS system tar --zstd")
    def test_resource_removal_rebuilds_archive_without_stale_stage_files(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "source"
            build = root / "build"
            sdk = root / "rack-sdk"
            (source / "res").mkdir(parents=True)
            (sdk / "include").mkdir(parents=True)
            (sdk / "dep" / "include").mkdir(parents=True)
            (sdk / "include" / "rack.hpp").write_text("// fixture\n")
            (source / "plugin.cpp").write_text('extern "C" void init() {}\n')
            (source / "plugin.json").write_text(
                '{"slug":"Fixture","version":"1.0.0","modules":[]}\n'
            )
            (source / "LICENSE.md").write_text("fixture\n")
            old_resource = source / "res" / "removed.svg"
            old_resource.write_text("old\n")
            (source / "CMakeLists.txt").write_text(
                f"""cmake_minimum_required(VERSION 3.24)
project(rack_contract LANGUAGES CXX)
set(PULP_HAS_RACK TRUE)
set(PULP_RACK_SDK_DIR_REAL "{sdk}")
include("{PULP_RACK}")
pulp_add_rack_plugin(fixture
  SLUG Fixture VERSION 1.0.0
  MANIFEST "${{CMAKE_CURRENT_SOURCE_DIR}}/plugin.json"
  RES_DIR "${{CMAKE_CURRENT_SOURCE_DIR}}/res"
  LICENSES "${{CMAKE_CURRENT_SOURCE_DIR}}/LICENSE.md"
  SOURCES "${{CMAKE_CURRENT_SOURCE_DIR}}/plugin.cpp")
"""
            )

            subprocess.run(
                ["cmake", "-S", str(source), "-B", str(build),
                 "-DCMAKE_BUILD_TYPE=Release"],
                check=True, capture_output=True, text=True,
            )
            first_build = self._build(build)
            self.assertIn("Packaging Fixture", first_build)
            archive = next((build / "rack").glob("Fixture-1.0.0-*.vcvplugin"))
            self.assertIn("Fixture/res/removed.svg", self._members(archive))
            first_hash = hashlib.sha256(archive.read_bytes()).hexdigest()
            first_mtime = archive.stat().st_mtime_ns

            old_resource.unlink()
            time.sleep(1.1)
            second_build = self._build(build)
            self.assertIn("Packaging Fixture", second_build)
            members = self._members(archive)
            self.assertNotIn("Fixture/res/removed.svg", members)
            self.assertIn("Fixture/plugin.json", members)
            self.assertTrue(any(name.endswith("/plugin.dylib") for name in members))
            self.assertGreater(archive.stat().st_mtime_ns, first_mtime)
            self.assertNotEqual(
                hashlib.sha256(archive.read_bytes()).hexdigest(), first_hash
            )

    def test_gnu_unique_is_a_compile_option_not_a_link_option(self) -> None:
        text = PULP_RACK.read_text()
        self.assertRegex(
            text,
            r"target_compile_options\(\$\{target\} PRIVATE -fno-gnu-unique\)",
        )
        link_blocks = [
            block.split(")", 1)[0]
            for block in text.split("target_link_options(")[1:]
        ]
        self.assertFalse(any("-fno-gnu-unique" in block for block in link_blocks))

    @staticmethod
    def _build(build: Path) -> str:
        proc = subprocess.run(
            ["cmake", "--build", str(build), "--target", "fixture-package",
             "--parallel", "2"],
            check=True, capture_output=True, text=True,
        )
        return proc.stdout + proc.stderr

    @staticmethod
    def _members(archive: Path) -> set[str]:
        proc = subprocess.run(
            ["/usr/bin/tar", "--zstd", "-tf", str(archive)],
            check=True, capture_output=True, text=True,
        )
        return set(proc.stdout.splitlines())


if __name__ == "__main__":
    unittest.main()

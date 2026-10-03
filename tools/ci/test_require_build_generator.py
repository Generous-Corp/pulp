#!/usr/bin/env python3
"""Tests for tools/ci/require_build_generator.sh.

A build directory configured with another generator (or whose cache names
none) is removed so the next configure starts clean; one already on the
wanted generator, or no directory at all, is left alone.

Run:
    python3 tools/ci/test_require_build_generator.py
"""
from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent / "require_build_generator.sh"


def run(build: Path, cwd: Path | None = None) -> subprocess.CompletedProcess:
    """Run the guard from `cwd` (default: the build directory's parent, the
    checkout a lane configures from)."""
    return subprocess.run(["bash", str(SCRIPT), str(build), "Ninja"], capture_output=True, text=True,
                          cwd=cwd or build.parent)


class RequireBuildGeneratorTests(unittest.TestCase):
    def build_with(self, root: Path, cache_line: str | None) -> Path:
        build = root / "build"
        (build / "obj").mkdir(parents=True)
        if cache_line is not None:
            (build / "CMakeCache.txt").write_text(f"FOO:BOOL=ON\n{cache_line}\n")
        return build

    def test_another_generator_is_removed_and_said_so(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            build = self.build_with(Path(tmp), "CMAKE_GENERATOR:INTERNAL=Unix Makefiles")
            proc = run(build)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertFalse(build.exists())
            self.assertIn("'Unix Makefiles', not 'Ninja'", proc.stdout)

    def test_a_cache_naming_no_generator_is_left_for_cmake(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            build = self.build_with(Path(tmp), "BAR:STRING=x")
            self.assertEqual(run(build).returncode, 0)
            self.assertTrue((build / "obj").is_dir())

    def test_it_removes_nothing_outside_the_checkout_or_through_a_symlink(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp).resolve()
            outside = self.build_with(root / "elsewhere", "CMAKE_GENERATOR:INTERNAL=Unix Makefiles")
            checkout = root / "checkout"
            checkout.mkdir()
            link = checkout / "build"
            link.symlink_to(outside)
            proc = run(link, cwd=checkout)
            self.assertEqual(proc.returncode, 1)
            self.assertIn("symlink", proc.stderr)
            proc = run(outside, cwd=checkout)  # a real directory, but not inside the checkout
            self.assertEqual(proc.returncode, 1)
            self.assertIn("not inside", proc.stderr)
            self.assertTrue((outside / "obj").is_dir())
            # The checkout itself is never a build directory to remove.
            (checkout / "CMakeCache.txt").write_text("CMAKE_GENERATOR:INTERNAL=Unix Makefiles\n")
            self.assertEqual(run(checkout, cwd=checkout).returncode, 1)
            self.assertTrue(checkout.is_dir())

    def test_the_wanted_generator_and_a_fresh_directory_are_left_alone(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            build = self.build_with(Path(tmp), "CMAKE_GENERATOR:INTERNAL=Ninja")
            proc = run(build)
            self.assertEqual((proc.returncode, proc.stdout), (0, ""))
            self.assertTrue((build / "obj").is_dir())
            unconfigured = self.build_with(Path(tmp) / "fresh", None)
            self.assertEqual(run(unconfigured).returncode, 0)
            self.assertTrue((unconfigured / "obj").is_dir())
            self.assertEqual(run(Path(tmp) / "absent").returncode, 0)


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Additional edge coverage for package_cli.py."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import pathlib
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock


SCRIPT = pathlib.Path(__file__).parent / "package_cli.py"
spec = importlib.util.spec_from_file_location("package_cli", SCRIPT)
assert spec and spec.loader
pc = importlib.util.module_from_spec(spec)
sys.modules["package_cli"] = pc
spec.loader.exec_module(pc)


@contextlib.contextmanager
def argv(args: list[str]):
    old = sys.argv[:]
    sys.argv = args[:]
    try:
        yield
    finally:
        sys.argv = old


class FindWgpuLibExtraTests(unittest.TestCase):
    def test_find_wgpu_lib_dedupes_roots_and_ignores_non_file_matches(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            home = root / "home"
            cache = home / ".cache"
            build = root / "build"
            build.mkdir()
            (build / "libwgpu_native.so").mkdir()
            (build / "wgpu_native.so").mkdir()

            with mock.patch.object(pc.Path, "home", return_value=home):
                with mock.patch.dict(pc.os.environ, {"XDG_CACHE_HOME": str(cache)}, clear=True):
                    self.assertIsNone(pc.find_wgpu_lib(build, "linux-x64"))

    def test_find_wgpu_lib_searches_xdg_cache_roots(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            home = root / "home"
            xdg = root / "xdg"
            lib_dir = xdg / "Pulp" / "fetchcontent-src"
            lib_dir.mkdir(parents=True)
            dylib = lib_dir / "libwgpu_native.dylib"
            dylib.write_text("dylib", encoding="utf-8")

            with mock.patch.object(pc.Path, "home", return_value=home):
                with mock.patch.dict(pc.os.environ, {"XDG_CACHE_HOME": str(xdg)}, clear=True):
                    found = pc.find_wgpu_lib(root / "missing-build", "darwin-arm64")
                    self.assertIsNotNone(found)
                    self.assertTrue(found.samefile(dylib))

    def test_find_wgpu_lib_searches_localappdata_root(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            local_appdata = root / "LocalAppData"
            lib_dir = local_appdata / "Pulp" / "fetchcontent-src"
            lib_dir.mkdir(parents=True)
            dll = lib_dir / "wgpu_native.dll"
            dll.write_text("dll", encoding="utf-8")

            with mock.patch.object(pc.Path, "home", return_value=root / "home"):
                with mock.patch.dict(pc.os.environ, {"LOCALAPPDATA": str(local_appdata)}, clear=True):
                    self.assertEqual(
                        pc.find_wgpu_lib(root / "missing-build", "windows-arm64"),
                        dll,
                    )


class RpathExtraTests(unittest.TestCase):
    def test_fix_rpath_macos_ignores_relative_rpath(self) -> None:
        otool_output = """
Load command 1
          cmd LC_RPATH
      cmdsize 40
         path relative/lib (offset 12)
        """
        with mock.patch.object(pc.subprocess, "check_output", return_value=otool_output):
            with mock.patch.object(pc.subprocess, "check_call") as check_call:
                with mock.patch.object(pc.subprocess, "run") as run:
                    pc.fix_rpath_macos(pathlib.Path("/tmp/pulp"))

        check_call.assert_not_called()
        run.assert_called_once()

    def test_fix_rpath_macos_ignores_malformed_rpath_line(self) -> None:
        otool_output = """
Load command 1
          cmd LC_RPATH
      cmdsize 48
         path /tmp/no-offset-marker
        """
        with mock.patch.object(pc.subprocess, "check_output", return_value=otool_output):
            with mock.patch.object(pc.subprocess, "check_call") as check_call:
                with mock.patch.object(pc.subprocess, "run") as run:
                    pc.fix_rpath_macos(pathlib.Path("/tmp/pulp"))

        check_call.assert_not_called()
        run.assert_called_once()


class MainExtraTests(unittest.TestCase):
    def test_main_packages_macos_tarball_and_rewrites_rpath(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            binary = root / "pulp-built"
            binary.write_text("binary", encoding="utf-8")
            wgpu = root / "libwgpu_native.dylib"
            wgpu.write_text("wgpu", encoding="utf-8")
            out = root / "pulp-darwin-arm64.tar.gz"

            with mock.patch.object(pc, "find_wgpu_lib", return_value=wgpu):
                with mock.patch.object(pc, "fix_rpath_macos") as fix_rpath:
                    with argv(
                        [
                            "package_cli.py",
                            "--binary",
                            str(binary),
                            "--build-dir",
                            str(root / "build"),
                            "--platform",
                            "darwin-arm64",
                            "--out",
                            str(out),
                        ]
                    ):
                        rc = pc.main()

            self.assertEqual(rc, 0)
            fix_rpath.assert_called_once()
            with tarfile.open(out, "r:gz") as tar:
                self.assertEqual(sorted(tar.getnames()), ["libwgpu_native.dylib", "pulp"])

    def test_script_entrypoint_reports_missing_binary(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            completed = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "--binary",
                    str(root / "missing-pulp"),
                    "--build-dir",
                    str(root / "build"),
                    "--platform",
                    "linux-x64",
                    "--out",
                    str(root / "pulp.tar.gz"),
                ],
                text=True,
                capture_output=True,
            )

        self.assertEqual(completed.returncode, 2)
        self.assertIn("FAIL: binary not at", completed.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)

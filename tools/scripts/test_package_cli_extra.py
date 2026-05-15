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
import zipfile
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
    def test_find_wgpu_lib_prefers_build_dir_before_cache_roots(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            build_lib = root / "build" / "_deps" / "wgpu" / "libwgpu_native.so"
            cache_lib = root / "home" / ".cache" / "pulp" / "libwgpu_native.so"
            build_lib.parent.mkdir(parents=True)
            cache_lib.parent.mkdir(parents=True)
            build_lib.write_text("build", encoding="utf-8")
            cache_lib.write_text("cache", encoding="utf-8")

            with mock.patch.object(pc.Path, "home", return_value=root / "home"):
                with mock.patch.dict(pc.os.environ, {}, clear=True):
                    self.assertEqual(pc.find_wgpu_lib(root / "build", "linux-x64"), build_lib)

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
    def test_fix_rpath_macos_deletes_multiple_absolute_rpaths(self) -> None:
        otool_output = """
Load command 1
          cmd LC_RPATH
      cmdsize 48
         path /Users/runner/Library/Caches/Pulp/wgpu (offset 12)
Load command 2
          cmd LC_RPATH
      cmdsize 48
         path /opt/pulp/cache/wgpu (offset 12)
Load command 3
          cmd LC_RPATH
      cmdsize 40
         path @loader_path (offset 12)
        """
        binary = pathlib.Path("/tmp/pulp")

        with mock.patch.object(pc.subprocess, "check_output", return_value=otool_output):
            with mock.patch.object(pc.subprocess, "check_call") as check_call:
                with mock.patch.object(pc.subprocess, "run") as run:
                    pc.fix_rpath_macos(binary)

        self.assertEqual(check_call.call_count, 2)
        check_call.assert_any_call(
            [
                "install_name_tool",
                "-delete_rpath",
                "/Users/runner/Library/Caches/Pulp/wgpu",
                str(binary),
            ]
        )
        check_call.assert_any_call(
            [
                "install_name_tool",
                "-delete_rpath",
                "/opt/pulp/cache/wgpu",
                str(binary),
            ]
        )
        run.assert_called_once_with(
            ["install_name_tool", "-add_rpath", "@loader_path", str(binary)],
            check=False,
        )

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


class StageBinaryExtraTests(unittest.TestCase):
    def test_stage_binary_sets_unix_executable_bit(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            src = root / "built-pulp"
            src.write_text("binary", encoding="utf-8")
            stage = root / "stage"
            stage.mkdir()

            staged = pc.stage_binary(src, stage, "pulp", is_windows=False)

            self.assertEqual(staged, stage / "pulp")
            self.assertEqual(staged.read_text(encoding="utf-8"), "binary")
            self.assertTrue(pc.os.access(staged, pc.os.X_OK))

    def test_stage_binary_keeps_windows_copy_non_executable(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            src = root / "built-pulp.exe"
            src.write_text("binary", encoding="utf-8")
            stage = root / "stage"
            stage.mkdir()

            staged = pc.stage_binary(src, stage, "pulp.exe", is_windows=True)

            self.assertEqual(staged, stage / "pulp.exe")
            self.assertEqual(staged.read_text(encoding="utf-8"), "binary")


class MainExtraTests(unittest.TestCase):
    def test_main_rejects_missing_cpp_binary(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            binary = root / "pulp-built"
            binary.write_text("binary", encoding="utf-8")

            with argv(
                [
                    "package_cli.py",
                    "--binary",
                    str(binary),
                    "--cpp-binary",
                    str(root / "missing-pulp-cpp"),
                    "--build-dir",
                    str(root / "build"),
                    "--platform",
                    "linux-x64",
                    "--out",
                    str(root / "pulp-linux-x64.tar.gz"),
                ]
            ):
                with contextlib.redirect_stderr(io.StringIO()) as stderr:
                    rc = pc.main()

            self.assertEqual(rc, 2)
            self.assertIn("FAIL: --cpp-binary not at", stderr.getvalue())

    def test_main_rejects_missing_wgpu_library(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            binary = root / "pulp-built"
            binary.write_text("binary", encoding="utf-8")
            out = root / "pulp-linux-x64.tar.gz"

            with mock.patch.object(pc, "find_wgpu_lib", return_value=None):
                with argv(
                    [
                        "package_cli.py",
                        "--binary",
                        str(binary),
                        "--build-dir",
                        str(root / "build"),
                        "--platform",
                        "linux-x64",
                        "--out",
                        str(out),
                    ]
                ):
                    with contextlib.redirect_stderr(io.StringIO()) as stderr:
                        rc = pc.main()

            self.assertEqual(rc, 1)
            self.assertFalse(out.exists())
            self.assertIn("wgpu native library not found", stderr.getvalue())

    def test_main_packages_windows_zip_with_delegate_and_dll(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            binary = root / "pulp-built.exe"
            cpp_binary = root / "pulp-cpp-built.exe"
            wgpu = root / "wgpu_native.dll"
            binary.write_text("binary", encoding="utf-8")
            cpp_binary.write_text("cpp", encoding="utf-8")
            wgpu.write_text("wgpu", encoding="utf-8")
            out = root / "pulp-windows-x64.zip"

            with mock.patch.object(pc, "find_wgpu_lib", return_value=wgpu):
                with mock.patch.object(pc, "fix_rpath_macos") as mac_rpath:
                    with mock.patch.object(pc, "fix_rpath_linux") as linux_rpath:
                        with argv(
                            [
                                "package_cli.py",
                                "--binary",
                                str(binary),
                                "--cpp-binary",
                                str(cpp_binary),
                                "--build-dir",
                                str(root / "build"),
                                "--platform",
                                "windows-x64",
                                "--out",
                                str(out),
                            ]
                        ):
                            rc = pc.main()

            self.assertEqual(rc, 0)
            mac_rpath.assert_not_called()
            linux_rpath.assert_not_called()
            with zipfile.ZipFile(out) as z:
                self.assertEqual(
                    sorted(z.namelist()),
                    ["pulp-cpp.exe", "pulp.exe", "wgpu_native.dll"],
                )

    def test_main_rewrites_macos_rpath_for_primary_and_delegate(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            binary = root / "pulp-built"
            cpp_binary = root / "pulp-cpp-built"
            wgpu = root / "libwgpu_native.dylib"
            binary.write_text("binary", encoding="utf-8")
            cpp_binary.write_text("cpp", encoding="utf-8")
            wgpu.write_text("wgpu", encoding="utf-8")
            out = root / "pulp-darwin-x64.tar.gz"

            with mock.patch.object(pc, "find_wgpu_lib", return_value=wgpu):
                with mock.patch.object(pc, "fix_rpath_macos") as fix_rpath:
                    with argv(
                        [
                            "package_cli.py",
                            "--binary",
                            str(binary),
                            "--cpp-binary",
                            str(cpp_binary),
                            "--build-dir",
                            str(root / "build"),
                            "--platform",
                            "darwin-x64",
                            "--out",
                            str(out),
                        ]
                    ):
                        rc = pc.main()

            self.assertEqual(rc, 0)
            self.assertEqual(fix_rpath.call_count, 2)
            self.assertEqual(
                [call.args[0].name for call in fix_rpath.call_args_list],
                ["pulp", "pulp-cpp"],
            )
            with tarfile.open(out, "r:gz") as tar:
                self.assertEqual(
                    sorted(tar.getnames()),
                    ["libwgpu_native.dylib", "pulp", "pulp-cpp"],
                )

    def test_main_rewrites_linux_rpath_for_primary_and_delegate(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            binary = root / "pulp-built"
            cpp_binary = root / "pulp-cpp-built"
            wgpu = root / "libwgpu_native.so"
            binary.write_text("binary", encoding="utf-8")
            cpp_binary.write_text("cpp", encoding="utf-8")
            wgpu.write_text("wgpu", encoding="utf-8")
            out = root / "pulp-linux-x64.tar.gz"

            with mock.patch.object(pc, "find_wgpu_lib", return_value=wgpu):
                with mock.patch.object(pc, "fix_rpath_linux") as fix_rpath:
                    with argv(
                        [
                            "package_cli.py",
                            "--binary",
                            str(binary),
                            "--cpp-binary",
                            str(cpp_binary),
                            "--build-dir",
                            str(root / "build"),
                            "--platform",
                            "linux-x64",
                            "--out",
                            str(out),
                        ]
                    ):
                        rc = pc.main()

            self.assertEqual(rc, 0)
            self.assertEqual(fix_rpath.call_count, 2)
            self.assertEqual(
                [call.args[0].name for call in fix_rpath.call_args_list],
                ["pulp", "pulp-cpp"],
            )
            with tarfile.open(out, "r:gz") as tar:
                self.assertEqual(
                    sorted(tar.getnames()),
                    ["libwgpu_native.so", "pulp", "pulp-cpp"],
                )

    def test_main_rewrites_linux_rpath_for_mcp_binary_too(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            binary = root / "pulp-built"
            cpp_binary = root / "pulp-cpp-built"
            mcp_binary = root / "pulp-mcp-built"
            wgpu = root / "libwgpu_native.so"
            binary.write_text("binary", encoding="utf-8")
            cpp_binary.write_text("cpp", encoding="utf-8")
            mcp_binary.write_text("mcp", encoding="utf-8")
            wgpu.write_text("wgpu", encoding="utf-8")
            out = root / "pulp-linux-x64.tar.gz"

            with mock.patch.object(pc, "find_wgpu_lib", return_value=wgpu):
                with mock.patch.object(pc, "fix_rpath_linux") as fix_rpath:
                    with argv(
                        [
                            "package_cli.py",
                            "--binary",
                            str(binary),
                            "--cpp-binary",
                            str(cpp_binary),
                            "--mcp-binary",
                            str(mcp_binary),
                            "--build-dir",
                            str(root / "build"),
                            "--platform",
                            "linux-x64",
                            "--out",
                            str(out),
                        ]
                    ):
                        rc = pc.main()

            self.assertEqual(rc, 0)
            self.assertEqual(fix_rpath.call_count, 3)
            self.assertEqual(
                [call.args[0].name for call in fix_rpath.call_args_list],
                ["pulp", "pulp-cpp", "pulp-mcp"],
            )
            with tarfile.open(out, "r:gz") as tar:
                self.assertEqual(
                    sorted(tar.getnames()),
                    ["libwgpu_native.so", "pulp", "pulp-cpp", "pulp-mcp"],
                )

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

    def test_main_packages_unknown_platform_as_tarball_without_rpath(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            binary = root / "pulp-built"
            binary.write_text("binary", encoding="utf-8")
            wgpu = root / "libwgpu_native.custom"
            wgpu.write_text("wgpu", encoding="utf-8")
            out = root / "pulp-custom.tar.gz"

            with mock.patch.object(pc, "find_wgpu_lib", return_value=wgpu):
                with mock.patch.object(pc, "fix_rpath_macos") as mac_rpath:
                    with mock.patch.object(pc, "fix_rpath_linux") as linux_rpath:
                        with argv(
                            [
                                "package_cli.py",
                                "--binary",
                                str(binary),
                                "--build-dir",
                                str(root / "build"),
                                "--platform",
                                "haiku-x64",
                                "--out",
                                str(out),
                            ]
                        ):
                            rc = pc.main()

            self.assertEqual(rc, 0)
            mac_rpath.assert_not_called()
            linux_rpath.assert_not_called()
            with tarfile.open(out, "r:gz") as tar:
                self.assertEqual(sorted(tar.getnames()), ["libwgpu_native.custom", "pulp"])

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

#!/usr/bin/env python3
"""Unit tests for tools/scripts/package_cli.py."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import pathlib
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


class FindWgpuLibTests(unittest.TestCase):
    def test_find_wgpu_lib_selects_platform_suffix_from_build_dir(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            build = pathlib.Path(td) / "build"
            nested = build / "_deps" / "wgpu"
            nested.mkdir(parents=True)
            lib = nested / "libwgpu_native.so"
            lib.write_text("so", encoding="utf-8")
            (nested / "libwgpu_native.dylib").write_text("dylib", encoding="utf-8")

            self.assertEqual(pc.find_wgpu_lib(build, "linux-x64"), lib)

    def test_find_wgpu_lib_finds_windows_dll_name(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            build = pathlib.Path(td) / "build"
            nested = build / "bin"
            nested.mkdir(parents=True)
            dll = nested / "wgpu_native.dll"
            dll.write_text("dll", encoding="utf-8")

            self.assertEqual(pc.find_wgpu_lib(build, "windows-x64"), dll)


class RpathTests(unittest.TestCase):
    def test_fix_rpath_macos_deletes_absolute_rpaths_and_adds_loader_path(self) -> None:
        otool_output = """
Load command 1
          cmd LC_RPATH
      cmdsize 48
         path /Users/runner/Library/Caches/Pulp/wgpu (offset 12)
Load command 2
          cmd LC_RPATH
      cmdsize 32
         path @loader_path (offset 12)
        """
        binary = pathlib.Path("/tmp/pulp")

        with mock.patch.object(pc.subprocess, "check_output", return_value=otool_output):
            with mock.patch.object(pc.subprocess, "check_call") as check_call:
                with mock.patch.object(pc.subprocess, "run") as run:
                    pc.fix_rpath_macos(binary)

        check_call.assert_called_once_with(
            [
                "install_name_tool",
                "-delete_rpath",
                "/Users/runner/Library/Caches/Pulp/wgpu",
                str(binary),
            ]
        )
        run.assert_called_once_with(
            ["install_name_tool", "-add_rpath", "@loader_path", str(binary)],
            check=False,
        )

    def test_fix_rpath_linux_skips_when_patchelf_is_missing(self) -> None:
        out = io.StringIO()
        with mock.patch.object(pc.shutil, "which", return_value=None):
            with mock.patch.object(pc.subprocess, "check_call") as check_call:
                with contextlib.redirect_stdout(out):
                    pc.fix_rpath_linux(pathlib.Path("/tmp/pulp"))

        check_call.assert_not_called()
        self.assertIn("patchelf not on PATH", out.getvalue())

    def test_fix_rpath_linux_sets_origin_when_patchelf_exists(self) -> None:
        binary = pathlib.Path("/tmp/pulp")
        with mock.patch.object(pc.shutil, "which", return_value="/usr/bin/patchelf"):
            with mock.patch.object(pc.subprocess, "check_call") as check_call:
                pc.fix_rpath_linux(binary)

        check_call.assert_called_once_with(["patchelf", "--set-rpath", "$ORIGIN", str(binary)])


class ArchiveTests(unittest.TestCase):
    def test_write_tarball_and_zip_preserve_inner_names(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            a = root / "a.bin"
            b = root / "b.bin"
            a.write_text("a", encoding="utf-8")
            b.write_text("b", encoding="utf-8")

            tar_path = root / "out" / "pulp.tar.gz"
            pc.write_tarball(tar_path, [a, b], ["pulp", "libwgpu_native.so"])
            with tarfile.open(tar_path, "r:gz") as tar:
                self.assertEqual(sorted(tar.getnames()), ["libwgpu_native.so", "pulp"])

            zip_path = root / "out" / "pulp.zip"
            pc.write_zip(zip_path, [a, b], ["pulp.exe", "wgpu_native.dll"])
            with zipfile.ZipFile(zip_path) as z:
                self.assertEqual(sorted(z.namelist()), ["pulp.exe", "wgpu_native.dll"])


class MainTests(unittest.TestCase):
    def test_main_returns_missing_binary_error(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            err = io.StringIO()
            with argv(
                [
                    "package_cli.py",
                    "--binary",
                    str(root / "missing-pulp"),
                    "--build-dir",
                    str(root / "build"),
                    "--platform",
                    "linux-x64",
                    "--out",
                    str(root / "pulp.tar.gz"),
                ]
            ):
                with contextlib.redirect_stderr(err):
                    rc = pc.main()

        self.assertEqual(rc, 2)
        self.assertIn("FAIL: binary not at", err.getvalue())

    def test_main_refuses_to_package_without_wgpu_library(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            binary = root / "pulp"
            binary.write_text("binary", encoding="utf-8")
            err = io.StringIO()

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
                        str(root / "pulp.tar.gz"),
                    ]
                ):
                    with contextlib.redirect_stderr(err):
                        rc = pc.main()

        self.assertEqual(rc, 1)
        self.assertIn("wgpu native library not found", err.getvalue())

    def test_main_packages_linux_tarball_with_bundled_wgpu(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            binary = root / "pulp-built"
            binary.write_text("binary", encoding="utf-8")
            wgpu = root / "libwgpu_native.so"
            wgpu.write_text("wgpu", encoding="utf-8")
            out = root / "pulp-linux-x64.tar.gz"

            with mock.patch.object(pc, "find_wgpu_lib", return_value=wgpu):
                with mock.patch.object(pc, "fix_rpath_linux") as fix_rpath:
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
                        rc = pc.main()

            self.assertEqual(rc, 0)
            fix_rpath.assert_called_once()
            with tarfile.open(out, "r:gz") as tar:
                self.assertEqual(sorted(tar.getnames()), ["libwgpu_native.so", "pulp"])

    def test_main_packages_dual_binary_tarball_with_cpp_binary(self) -> None:
        # Phase 8 contract: --cpp-binary bundles `pulp-cpp` alongside
        # `pulp` so the Rust upgrade --install can land both binaries
        # from a single archive. rpath rewrite runs on both.
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            pulp = root / "pulp-built"
            cpp = root / "pulp-cpp-built"
            pulp.write_text("rust-binary", encoding="utf-8")
            cpp.write_text("cpp-binary", encoding="utf-8")
            wgpu = root / "libwgpu_native.dylib"
            wgpu.write_text("wgpu", encoding="utf-8")
            out = root / "pulp-darwin-arm64.tar.gz"

            with mock.patch.object(pc, "find_wgpu_lib", return_value=wgpu):
                with mock.patch.object(pc, "fix_rpath_macos") as fix_rpath:
                    with argv(
                        [
                            "package_cli.py",
                            "--binary",
                            str(pulp),
                            "--cpp-binary",
                            str(cpp),
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
            # Both binaries must get rpath rewriting, otherwise the
            # delegate path (pulp-cpp) crashes on a clean machine.
            self.assertEqual(fix_rpath.call_count, 2)
            with tarfile.open(out, "r:gz") as tar:
                names = sorted(tar.getnames())
            self.assertEqual(
                names, ["libwgpu_native.dylib", "pulp", "pulp-cpp"]
            )

    def test_main_dual_binary_uses_exe_suffix_on_windows(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            pulp = root / "pulp-built.exe"
            cpp = root / "pulp-cpp-built.exe"
            pulp.write_text("rust", encoding="utf-8")
            cpp.write_text("cpp", encoding="utf-8")
            wgpu = root / "wgpu_native.dll"
            wgpu.write_text("wgpu", encoding="utf-8")
            out = root / "pulp-windows-x64.zip"

            with mock.patch.object(pc, "find_wgpu_lib", return_value=wgpu):
                with argv(
                    [
                        "package_cli.py",
                        "--binary",
                        str(pulp),
                        "--cpp-binary",
                        str(cpp),
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
            with zipfile.ZipFile(out) as z:
                names = sorted(z.namelist())
            self.assertEqual(
                names, ["pulp-cpp.exe", "pulp.exe", "wgpu_native.dll"]
            )

    def test_main_returns_missing_cpp_binary_error(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            pulp = root / "pulp-built"
            pulp.write_text("rust", encoding="utf-8")
            err = io.StringIO()
            with argv(
                [
                    "package_cli.py",
                    "--binary",
                    str(pulp),
                    "--cpp-binary",
                    str(root / "missing-pulp-cpp"),
                    "--build-dir",
                    str(root / "build"),
                    "--platform",
                    "linux-x64",
                    "--out",
                    str(root / "pulp.tar.gz"),
                ]
            ):
                with contextlib.redirect_stderr(err):
                    rc = pc.main()

        self.assertEqual(rc, 2)
        self.assertIn("FAIL: --cpp-binary not at", err.getvalue())

    def test_main_packages_triple_binary_tarball_with_mcp_binary(self) -> None:
        # #2067 contract: --mcp-binary bundles `pulp-mcp` alongside
        # `pulp` (and optionally `pulp-cpp`) so a fresh install drops
        # the Claude Code plugin's MCP server into ~/.pulp/bin/. rpath
        # rewrite runs on the MCP binary too so its bundled wgpu dylib
        # resolves via @loader_path on a clean user machine.
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            pulp = root / "pulp-built"
            cpp = root / "pulp-cpp-built"
            mcp = root / "pulp-mcp-built"
            pulp.write_text("rust-binary", encoding="utf-8")
            cpp.write_text("cpp-binary", encoding="utf-8")
            mcp.write_text("mcp-binary", encoding="utf-8")
            wgpu = root / "libwgpu_native.dylib"
            wgpu.write_text("wgpu", encoding="utf-8")
            out = root / "pulp-darwin-arm64.tar.gz"

            with mock.patch.object(pc, "find_wgpu_lib", return_value=wgpu):
                with mock.patch.object(pc, "fix_rpath_macos") as fix_rpath:
                    with argv(
                        [
                            "package_cli.py",
                            "--binary",
                            str(pulp),
                            "--cpp-binary",
                            str(cpp),
                            "--mcp-binary",
                            str(mcp),
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
            # All three binaries must get rpath rewriting, otherwise the
            # MCP binary crashes on a clean machine when the plugin
            # launcher exec's it.
            self.assertEqual(fix_rpath.call_count, 3)
            with tarfile.open(out, "r:gz") as tar:
                names = sorted(tar.getnames())
            self.assertEqual(
                names,
                ["libwgpu_native.dylib", "pulp", "pulp-cpp", "pulp-mcp"],
            )

    def test_main_mcp_binary_uses_exe_suffix_on_windows(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            pulp = root / "pulp-built.exe"
            mcp = root / "pulp-mcp-built.exe"
            pulp.write_text("rust", encoding="utf-8")
            mcp.write_text("mcp", encoding="utf-8")
            wgpu = root / "wgpu_native.dll"
            wgpu.write_text("wgpu", encoding="utf-8")
            out = root / "pulp-windows-x64.zip"

            with mock.patch.object(pc, "find_wgpu_lib", return_value=wgpu):
                with argv(
                    [
                        "package_cli.py",
                        "--binary",
                        str(pulp),
                        "--mcp-binary",
                        str(mcp),
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
            with zipfile.ZipFile(out) as z:
                names = sorted(z.namelist())
            self.assertEqual(
                names, ["pulp-mcp.exe", "pulp.exe", "wgpu_native.dll"]
            )

    def test_main_returns_missing_mcp_binary_error(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            pulp = root / "pulp-built"
            pulp.write_text("rust", encoding="utf-8")
            err = io.StringIO()
            with argv(
                [
                    "package_cli.py",
                    "--binary",
                    str(pulp),
                    "--mcp-binary",
                    str(root / "missing-pulp-mcp"),
                    "--build-dir",
                    str(root / "build"),
                    "--platform",
                    "linux-x64",
                    "--out",
                    str(root / "pulp.tar.gz"),
                ]
            ):
                with contextlib.redirect_stderr(err):
                    rc = pc.main()

        self.assertEqual(rc, 2)
        self.assertIn("FAIL: --mcp-binary not at", err.getvalue())

    def test_main_packages_without_mcp_when_flag_omitted(self) -> None:
        # Pre-#2067 release lanes still call package_cli.py with only
        # --binary and --cpp-binary. Make sure that contract is byte-
        # identical to the previous behavior — no pulp-mcp entry, no
        # error.
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            pulp = root / "pulp-built"
            pulp.write_text("rust", encoding="utf-8")
            wgpu = root / "libwgpu_native.so"
            wgpu.write_text("wgpu", encoding="utf-8")
            out = root / "pulp-linux-x64.tar.gz"

            with mock.patch.object(pc, "find_wgpu_lib", return_value=wgpu):
                with mock.patch.object(pc, "fix_rpath_linux"):
                    with argv(
                        [
                            "package_cli.py",
                            "--binary",
                            str(pulp),
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
            with tarfile.open(out, "r:gz") as tar:
                names = sorted(tar.getnames())
            self.assertNotIn("pulp-mcp", names)

    def test_main_packages_windows_zip_without_rpath_rewrite(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = pathlib.Path(td)
            binary = root / "pulp.exe-built"
            binary.write_text("binary", encoding="utf-8")
            wgpu = root / "wgpu_native.dll"
            wgpu.write_text("wgpu", encoding="utf-8")
            out = root / "pulp-windows-x64.zip"

            with mock.patch.object(pc, "find_wgpu_lib", return_value=wgpu):
                with mock.patch.object(pc, "fix_rpath_linux") as linux_rpath:
                    with mock.patch.object(pc, "fix_rpath_macos") as mac_rpath:
                        with argv(
                            [
                                "package_cli.py",
                                "--binary",
                                str(binary),
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
            linux_rpath.assert_not_called()
            mac_rpath.assert_not_called()
            with zipfile.ZipFile(out) as z:
                self.assertEqual(sorted(z.namelist()), ["pulp.exe", "wgpu_native.dll"])


if __name__ == "__main__":
    unittest.main(verbosity=2)

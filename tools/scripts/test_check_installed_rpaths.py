#!/usr/bin/env python3
"""The installed CLI layout must satisfy every binary's @rpath dependencies.

Real Mach-O fixtures stand in for the release: a ``libwgpu_native.dylib`` with
an ``@rpath`` install name and a ``pulp-cpp`` that links it through
``LC_RPATH @loader_path``, which is how the release binaries ship. The
installer case runs the real ``tools/install/install.sh`` against a fixture
archive whose broker activation fails, the path that stranded ``pulp-cpp``
without its runtime.
"""

from __future__ import annotations

import os
import platform
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
sys.path.insert(0, str(HERE))

import check_installed_rpaths as checker  # noqa: E402

ON_MACOS = platform.system() == "Darwin" and shutil.which("clang") and shutil.which("otool")

MOCK_PULP = """#!/bin/sh
case "${1:-}" in
    __control-broker-reconcile) exit 66 ;;
    --version) echo 0.0.0-test ;;
esac
"""

MOCK_UNAME = """#!/bin/sh
case "${1:-}" in
    -s) echo Darwin ;;
    -m) echo arm64 ;;
    *) echo Darwin ;;
esac
"""

MOCK_CURL = """#!/bin/sh
output=
while [ "$#" -gt 0 ]; do
    if [ "$1" = "-o" ]; then shift; output="$1"; fi
    shift
done
[ -n "$output" ] || exit 2
cp "$MOCK_ARCHIVE" "$output"
"""


def _write_exec(path: Path, body: str) -> None:
    path.write_text(body)
    path.chmod(0o755)


def build_fixtures(out: Path) -> None:
    """Compile the runtime dylib and a binary that links it via @rpath."""
    src = out / "src"
    src.mkdir()
    (src / "runtime.c").write_text("int wgpu_fixture(void) { return 7; }\n")
    (src / "main.c").write_text(
        "int wgpu_fixture(void);\nint main(void) { return wgpu_fixture() == 7 ? 0 : 1; }\n"
    )
    subprocess.run(
        [
            "clang", "-dynamiclib", str(src / "runtime.c"),
            "-install_name", "@rpath/libwgpu_native.dylib",
            "-o", str(out / "libwgpu_native.dylib"),
        ],
        check=True,
    )
    subprocess.run(
        [
            "clang", str(src / "main.c"), "-L", str(out), "-lwgpu_native",
            "-Wl,-rpath,@loader_path", "-o", str(out / "pulp-cpp"),
        ],
        check=True,
    )
    shutil.rmtree(src)


@unittest.skipUnless(ON_MACOS, "needs macOS with clang and otool")
class CheckerTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)
        build_fixtures(self.tmp)

    def test_flat_layout_resolves(self) -> None:
        checked, unresolved = checker.check_tree(self.tmp)
        self.assertEqual(checked, 2)
        self.assertEqual(unresolved, [])
        # The fixture really runs, so "resolves" means what dyld means.
        subprocess.run([str(self.tmp / "pulp-cpp")], check=True)

    def test_missing_runtime_is_reported(self) -> None:
        (self.tmp / "libwgpu_native.dylib").unlink()
        checked, unresolved = checker.check_tree(self.tmp)
        self.assertEqual(checked, 1)
        self.assertEqual(
            [(u.binary, u.dependency) for u in unresolved],
            [("pulp-cpp", "@rpath/libwgpu_native.dylib")],
        )
        self.assertEqual(checker.main([str(self.tmp)]), 1)

    def test_a_runtime_elsewhere_does_not_count(self) -> None:
        lib = self.tmp / "lib"
        lib.mkdir()
        (self.tmp / "libwgpu_native.dylib").rename(lib / "libwgpu_native.dylib")
        bin_dir = self.tmp / "bin"
        bin_dir.mkdir()
        (self.tmp / "pulp-cpp").rename(bin_dir / "pulp-cpp")
        _, unresolved = checker.check_tree(self.tmp)
        self.assertEqual([u.binary for u in unresolved], ["bin/pulp-cpp"])

    def test_an_empty_tree_is_not_a_pass(self) -> None:
        empty = self.tmp / "empty"
        empty.mkdir()
        self.assertEqual(checker.main([str(empty)]), 1)


@unittest.skipUnless(ON_MACOS, "needs macOS with clang and otool")
class InstallerLayoutTest(unittest.TestCase):
    """install.sh must leave every installed binary loadable, broker or not."""

    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp)
        payload = self.tmp / "payload"
        payload.mkdir()
        build_fixtures(payload)
        _write_exec(payload / "pulp", MOCK_PULP)
        (payload / "pulp-control-broker").write_text("broker-fixture\n")
        (payload / "pulp-control-standalone-host").write_text("host-fixture\n")
        (payload / "pulp-control-standalone-host.inspector-capabilities.json").write_text(
            '{"schema_version":1}\n'
        )
        self.archive = self.tmp / "pulp.tar.gz"
        with tarfile.open(self.archive, "w:gz") as tar:
            for member in sorted(payload.iterdir()):
                tar.add(member, arcname=member.name)
        self.mock_bin = self.tmp / "mock-bin"
        self.mock_bin.mkdir()
        _write_exec(self.mock_bin / "uname", MOCK_UNAME)
        _write_exec(self.mock_bin / "curl", MOCK_CURL)

    def run_installer(self, install_dir: Path) -> subprocess.CompletedProcess[str]:
        home = self.tmp / "home"
        home.mkdir(exist_ok=True)
        env = dict(os.environ)
        env.update(
            HOME=str(home),
            PATH=f"{self.mock_bin}:{env.get('PATH', '/usr/bin:/bin')}",
            MOCK_ARCHIVE=str(self.archive),
            PULP_VERSION="0.0.0-test",
            PULP_INSTALL_DIR=str(install_dir),
            PULP_NO_MODIFY_PATH="1",
            PULP_SKIP_SDK_INSTALL="1",
        )
        env.pop("PULP_ACCEPT_CONTROL_BROKER_CUSTOM_INSTALL_ROOT", None)
        return subprocess.run(
            ["bash", str(REPO / "tools" / "install" / "install.sh")],
            env=env, capture_output=True, text=True, timeout=120,
        )

    def test_failed_broker_activation_still_leaves_every_binary_loadable(self) -> None:
        install_dir = self.tmp / "opt" / "pulp" / "bin"
        result = self.run_installer(install_dir)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("control broker activation failed", result.stdout)
        checked, unresolved = checker.check_tree(install_dir)
        self.assertGreaterEqual(checked, 2, "the installed fixtures were not found")
        self.assertEqual(unresolved, [], [u.__dict__ for u in unresolved])
        subprocess.run([str(install_dir / "pulp-cpp")], check=True)


if __name__ == "__main__":
    unittest.main()

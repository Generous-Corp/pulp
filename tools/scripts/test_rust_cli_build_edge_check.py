#!/usr/bin/env python3
"""Self-test for rust_cli_build_edge_check.py.

The two fixtures are the cargo edges CMake generates for the always-run
`add_custom_target(pulp-rust-cli ALL COMMAND cargo ...)` form and for the
incremental `add_custom_command(OUTPUT ... DEPFILE ...)` form (paths
shortened).
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import rust_cli_build_edge_check as check  # noqa: E402

ALWAYS_RUN = """\
build rs/CMakeFiles/pulp-rust-cli rs/cargo-target/release/pulp | ${cmake_ninja_workdir}rs/CMakeFiles/pulp-rust-cli ${cmake_ninja_workdir}rs/cargo-target/release/pulp: CUSTOM_COMMAND
  COMMAND = cd /src/experimental/pulp-rs && cmake -E env CARGO_TARGET_DIR=/b/rs/cargo-target cargo build --release --manifest-path /src/experimental/pulp-rs/Cargo.toml
  DESC = Building pulp-rs (Rust CLI) via cargo (release profile)
  pool = console
  restat = 1

build pulp-rust-cli: phony rs/CMakeFiles/pulp-rust-cli rs/cargo-target/release/pulp
"""

INCREMENTAL = """\
build rs/cargo-target/release/pulp pulp | ${cmake_ninja_workdir}rs/cargo-target/release/pulp ${cmake_ninja_workdir}pulp: CUSTOM_COMMAND /src/experimental/pulp-rs/Cargo.toml /src/experimental/pulp-rs/Cargo.lock /src/experimental/pulp-rs/rust-toolchain.toml /src/experimental/pulp-rs/build.rs
  COMMAND = cd /src/experimental/pulp-rs && cmake -E env CARGO_TARGET_DIR=/b/rs/cargo-target cargo build --release --locked --manifest-path /src/experimental/pulp-rs/Cargo.toml && cmake -E touch /b/rs/cargo-target/release/pulp && cmake -E copy /b/rs/cargo-target/release/pulp /b/pulp
  DESC = Building pulp-rs (Rust CLI) via cargo (release profile)
  depfile = CMakeFiles/d/0123.d
  deps = gcc
  pool = console
  restat = 1

build pulp-rust-cli: phony rs/pulp-rust-cli
"""


class RustCliBuildEdgeCheckTest(unittest.TestCase):
    def test_incremental_edge_passes(self) -> None:
        self.assertEqual(check.check(INCREMENTAL), [])

    def test_always_run_edge_fails_on_every_property(self) -> None:
        problems = "\n".join(check.check(ALWAYS_RUN))
        self.assertIn("always dirty", problems)
        self.assertIn("no depfile", problems)
        self.assertIn("Cargo.toml", problems)
        self.assertIn("Cargo.lock", problems)

    def test_missing_depfile_alone_fails(self) -> None:
        text = INCREMENTAL.replace("  depfile = CMakeFiles/d/0123.d\n", "")
        self.assertEqual(len(check.check(text)), 1)

    def test_missing_lockfile_input_alone_fails(self) -> None:
        text = INCREMENTAL.replace(" /src/experimental/pulp-rs/Cargo.lock", "")
        problems = check.check(text)
        self.assertEqual(len(problems), 1)
        self.assertIn("Cargo.lock", problems[0])

    def test_always_run_edge_is_also_unlocked(self) -> None:
        self.assertIn("--locked", "\n".join(check.check(ALWAYS_RUN)))

    def test_missing_locked_flag_alone_fails(self) -> None:
        text = INCREMENTAL.replace(" --locked", "")
        self.assertNotEqual(text, INCREMENTAL)
        problems = check.check(text)
        self.assertEqual(len(problems), 1)
        self.assertIn("--locked", problems[0])

    def test_offline_and_locked_together_pass(self) -> None:
        text = INCREMENTAL.replace(" --locked", " --locked --offline")
        self.assertEqual(check.check(text), [])

    def test_locked_on_another_step_does_not_count(self) -> None:
        text = INCREMENTAL.replace(" --locked", "").replace(
            "cmake -E touch", "cmake -E echo --locked && cmake -E touch")
        self.assertIn("--locked", "\n".join(check.check(text)))

    def test_no_cargo_edge_is_reported_not_passed(self) -> None:
        self.assertTrue(check.check("build a: phony b\n"))

    def test_main_skips_without_build_ninja_and_reads_the_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(check.main(["--build-dir", tmp]), check.SKIP)
            (Path(tmp) / "build.ninja").write_text("build a: phony b\n", encoding="utf-8")
            self.assertEqual(check.main(["--build-dir", tmp]), check.SKIP)
            (Path(tmp) / "build.ninja").write_text(ALWAYS_RUN, encoding="utf-8")
            self.assertEqual(check.main(["--build-dir", tmp]), 1)
            (Path(tmp) / "build.ninja").write_text(INCREMENTAL, encoding="utf-8")
            self.assertEqual(check.main(["--build-dir", tmp]), 0)


@unittest.skipUnless(os.name == "posix" and shutil.which("cmake") and shutil.which("ninja"),
                     "requires POSIX executable fixtures, CMake and Ninja")
class RustCliCompilerBindingTest(unittest.TestCase):
    """Execute the real CMake bridge with competing compiler installations."""

    def run_fixture(self, overrides: dict[str, str], *, broken: bool = False, bound: bool = True) -> dict:
        root = Path(__file__).resolve().parents[2]
        with tempfile.TemporaryDirectory(prefix="pulp-rust-compiler-") as temporary:
            fixture = Path(temporary)
            selected = fixture / "validated tools" / "rustc"
            shadow = fixture / "path-tools" / "rustc"
            override = fixture / "override-tools" / "rustc"
            for compiler in (selected, shadow, override):
                compiler.parent.mkdir(parents=True)
                compiler.write_text("#!/bin/sh\nprintf '%s\\n' '" + compiler.parent.name + "'\n", encoding="utf-8")
                compiler.chmod(0o755)
            if broken:
                override.write_text("#!/bin/sh\nexit 1\n", encoding="utf-8")
            cargo = fixture / "cargo"
            cargo.write_text("#!" + sys.executable + "\n" + r"""
import json, os, pathlib, shutil, subprocess, sys
if sys.argv[1] == '--version':
    print('cargo fixture'); raise SystemExit(0)
compiler = os.environ.get('RUSTC') or os.environ.get('CARGO_BUILD_RUSTC') or shutil.which('rustc')
result = subprocess.run([compiler, '--version'], check=True, capture_output=True, text=True)
pathlib.Path(os.environ['COMPILER_RECEIPT']).write_text(json.dumps({'compiler': compiler, 'version': result.stdout.strip()}))
target = pathlib.Path(os.environ['CARGO_TARGET_DIR']) / 'release' / 'pulp'
target.parent.mkdir(parents=True, exist_ok=True)
target.write_text('compiler fixture')
target.with_suffix('.d').write_text(str(target) + ':\n')
""", encoding="utf-8")
            cargo.chmod(0o755)
            source = fixture / "source"
            source.mkdir()
            bridge = root / 'experimental/pulp-rs'
            if not bound:
                bridge = fixture / 'unbound-bridge'
                bridge.mkdir()
                text = (root / 'experimental/pulp-rs/CMakeLists.txt').read_text(encoding="utf-8")
                (bridge / 'CMakeLists.txt').write_text(text.replace('        "RUSTC=${RUSTC_EXECUTABLE}"\n', ''), encoding="utf-8")
                for name in ('Cargo.toml', 'Cargo.lock', 'rust-toolchain.toml', 'build.rs'):
                    shutil.copy2(root / 'experimental/pulp-rs' / name, bridge / name)
            (source / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.24)\n'
                'project(RustCompilerFixture VERSION 1.0.0 LANGUAGES NONE)\n'
                'add_subdirectory("' + str(bridge) + '" rs)\n', encoding="utf-8")
            receipt = fixture / "receipt.json"
            env = os.environ.copy()
            for name in ("RUSTC", "CARGO_BUILD_RUSTC", "PULP_SKIP_RUST_CLI"):
                env.pop(name, None)
            env.update({name: str(shadow if value == 'shadow' else override) for name, value in overrides.items()})
            env['PATH'] = str(shadow.parent) + os.pathsep + env['PATH']
            env['COMPILER_RECEIPT'] = str(receipt)
            command = [shutil.which('cmake'), '-S', str(source), '-B', str(fixture / 'build'),
                       '-G', 'Ninja', '-DCARGO_EXECUTABLE=' + str(cargo),
                       '-DRUSTC_EXECUTABLE=' + str(selected)]
            configured = subprocess.run(command, env=env, check=True, capture_output=True, text=True, timeout=30, encoding="utf-8")
            # Changing the shell after configure must not replace the validated compiler.
            env.pop('RUSTC', None)
            env.pop('CARGO_BUILD_RUSTC', None)
            subprocess.run([shutil.which('ninja'), '-C', str(fixture / 'build'), 'pulp-rust-cli'],
                           env=env, check=not broken, capture_output=True, text=True, timeout=30, encoding="utf-8")
            if broken:
                self.assertIn("not runnable", configured.stdout)
                self.assertFalse(receipt.exists(), 'unusable explicit compiler must skip the CLI')
                return {}
            return json.loads(receipt.read_text(encoding="utf-8"))

    def test_build_executes_validated_compiler_despite_competing_path(self) -> None:
        self.assertEqual(self.run_fixture({})['version'], 'validated tools')

    def test_unbound_control_reproduces_compiler_switch(self) -> None:
        self.assertEqual(self.run_fixture({}, bound=False)['version'], 'path-tools')

    def test_rustc_has_priority_over_cargo_build_rustc(self) -> None:
        result = self.run_fixture({'RUSTC': '', 'CARGO_BUILD_RUSTC': 'shadow'})
        self.assertEqual(result['version'], 'override-tools')

    def test_explicit_rustc_is_validated_and_used(self) -> None:
        self.assertEqual(self.run_fixture({'RUSTC': ''})['version'], 'override-tools')

    def test_cargo_build_rustc_is_validated_and_used(self) -> None:
        self.assertEqual(self.run_fixture({'CARGO_BUILD_RUSTC': ''})['version'], 'override-tools')

    def test_unusable_explicit_compiler_skips_instead_of_silently_falling_back(self) -> None:
        self.run_fixture({'RUSTC': ''}, broken=True)


if __name__ == "__main__":
    unittest.main()

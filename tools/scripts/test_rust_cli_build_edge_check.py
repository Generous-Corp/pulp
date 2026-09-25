#!/usr/bin/env python3
"""Self-test for rust_cli_build_edge_check.py.

The two fixtures are the cargo edges CMake generates for the always-run
`add_custom_target(pulp-rust-cli ALL COMMAND cargo ...)` form and for the
incremental `add_custom_command(OUTPUT ... DEPFILE ...)` form (paths
shortened).
"""

from __future__ import annotations

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
  COMMAND = cd /src/experimental/pulp-rs && cmake -E env CARGO_TARGET_DIR=/b/rs/cargo-target cargo build --release --manifest-path /src/experimental/pulp-rs/Cargo.toml && cmake -E touch /b/rs/cargo-target/release/pulp && cmake -E copy /b/rs/cargo-target/release/pulp /b/pulp
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

    def test_no_cargo_edge_is_reported_not_passed(self) -> None:
        self.assertTrue(check.check("build a: phony b\n"))

    def test_main_skips_without_build_ninja_and_reads_the_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(check.main(["--build-dir", tmp]), check.SKIP)
            (Path(tmp) / "build.ninja").write_text("build a: phony b\n")
            self.assertEqual(check.main(["--build-dir", tmp]), check.SKIP)
            (Path(tmp) / "build.ninja").write_text(ALWAYS_RUN)
            self.assertEqual(check.main(["--build-dir", tmp]), 1)
            (Path(tmp) / "build.ninja").write_text(INCREMENTAL)
            self.assertEqual(check.main(["--build-dir", tmp]), 0)


if __name__ == "__main__":
    unittest.main()

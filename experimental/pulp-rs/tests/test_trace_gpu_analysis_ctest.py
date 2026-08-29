#!/usr/bin/env python3
"""Planted controls for the GPU trace integration CTest wrapper."""

from __future__ import annotations

import importlib.util
import os
import stat
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

MODULE_PATH = Path(__file__).with_name("run_trace_gpu_analysis_integration.py")
SPEC = importlib.util.spec_from_file_location("trace_gpu_analysis_ctest", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
WRAPPER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(WRAPPER)


def make_executable(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"processor")
    path.chmod(path.stat().st_mode | stat.S_IXUSR)


class TraceGpuAnalysisCtestTests(unittest.TestCase):
    def test_missing_processor_is_an_honest_skip_before_cargo(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            with mock.patch.object(WRAPPER.subprocess, "run") as run:
                result = WRAPPER.run_integration(
                    cargo="cargo",
                    target_dir=Path(directory) / "target",
                    manifest=Path(directory) / "Cargo.toml",
                    environment={"HOME": directory},
                )
            self.assertEqual(result, WRAPPER.SKIP_RETURN_CODE)
            run.assert_not_called()

    def test_invalid_explicit_override_fails_instead_of_skipping(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            with mock.patch.object(WRAPPER.subprocess, "run") as run:
                result = WRAPPER.run_integration(
                    cargo="cargo",
                    target_dir=Path(directory) / "target",
                    manifest=Path(directory) / "Cargo.toml",
                    environment={"PULP_TRACE_PROCESSOR": str(Path(directory) / "missing")},
                )
            self.assertEqual(result, 1)
            run.assert_not_called()

    def test_pinned_processor_executes_the_real_cargo_test(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            processor = (
                root
                / "tools"
                / "trace-processor"
                / WRAPPER.PINNED_VERSION
                / "mac-arm64"
                / "trace_processor_shell"
            )
            make_executable(processor)
            with mock.patch.object(
                WRAPPER.platform, "system", return_value="Darwin"
            ), mock.patch.object(
                WRAPPER.platform, "machine", return_value="arm64"
            ), mock.patch.object(
                WRAPPER.subprocess,
                "run",
                return_value=subprocess.CompletedProcess([], 19),
            ) as run:
                result = WRAPPER.run_integration(
                    cargo="cargo-bin",
                    target_dir=root / "target",
                    manifest=root / "Cargo.toml",
                    environment={"PULP_HOME": str(root)},
                )
            self.assertEqual(result, 19)
            argv = run.call_args.args[0]
            self.assertEqual(argv[-2:], ["--test", "trace_gpu_analysis_tool_test"])
            child_environment = run.call_args.kwargs["env"]
            self.assertEqual(child_environment["PULP_TRACE_PROCESSOR"], str(processor))
            self.assertEqual(child_environment["CARGO_TARGET_DIR"], str(root / "target"))

    def test_explicit_processor_takes_precedence_over_the_pinned_cache(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            explicit = root / "explicit-processor"
            make_executable(explicit)
            resolved = WRAPPER.resolve_trace_processor(
                {
                    "PULP_TRACE_PROCESSOR": str(explicit),
                    "PULP_HOME": str(root / "ignored"),
                },
                system="Darwin",
                machine="arm64",
            )
            self.assertEqual(resolved, explicit)


if __name__ == "__main__":
    unittest.main()

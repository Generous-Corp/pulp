#!/usr/bin/env python3
"""Regression tests for CI build-directory isolation.

Self-hosted macOS runners keep workspaces warm between workflows. Reusing the
same CMake build directory across ordinary build jobs and sanitizer jobs can
carry cached sanitizer flags into the next build. These tests keep the
workflow files on distinct build dirs so a stale `CMakeCache.txt` cannot turn
the normal macOS build into an accidental ASan run.

Run:
    python3 tools/scripts/test_workflow_build_dirs.py
"""

from __future__ import annotations

import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
BUILD_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "build.yml"
SANITIZERS_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "sanitizers.yml"


class WorkflowBuildDirTests(unittest.TestCase):
    def test_build_matrix_uses_matrix_scoped_build_dir(self) -> None:
        text = BUILD_WORKFLOW.read_text(encoding="utf-8")

        self.assertIn("PULP_BUILD_DIR: build-${{ matrix.key }}", text)
        self.assertIn('cmake -S . -B "$PULP_BUILD_DIR"', text)
        self.assertIn("- name: Build\n        shell: bash", text)
        self.assertIn('cmake --build "$PULP_BUILD_DIR" --config Release', text)
        self.assertIn('ctest --test-dir "$PULP_BUILD_DIR"', text)
        self.assertIn('ctest --test-dir "%PULP_BUILD_DIR%"', text)

        self.assertNotIn("working-directory: build", text)

    def test_sanitizer_jobs_use_distinct_build_dirs(self) -> None:
        text = SANITIZERS_WORKFLOW.read_text(encoding="utf-8")

        expected_dirs = {
            "ASan": "build-asan",
            "TSan": "build-tsan",
            "UBSan": "build-ubsan",
            "RTSan": "build-rtsan",
        }
        for label, build_dir in expected_dirs.items():
            with self.subTest(label=label):
                self.assertIn(f"-B {build_dir}", text)
                self.assertIn(f"cmake --build {build_dir}", text)
                self.assertIn(f"ctest --test-dir {build_dir}", text)

        self.assertNotIn("-B build \\", text)
        self.assertNotIn("cmake --build build ", text)
        self.assertNotIn("ctest --test-dir build ", text)


if __name__ == "__main__":
    unittest.main(verbosity=2)

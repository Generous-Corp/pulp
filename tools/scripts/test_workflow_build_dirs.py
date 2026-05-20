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
COVERAGE_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "coverage.yml"
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
        self.assertIn('label_exclude="validation|slow"', text)
        self.assertIn('set "PULP_CTEST_LABEL_EXCLUDE=validation|slow"', text)

        self.assertNotIn("working-directory: build", text)

    def test_build_workflow_shipyard_dispatch_excludes_slow_ctests(self) -> None:
        text = BUILD_WORKFLOW.read_text(encoding="utf-8")

        self.assertIn(
            """if [ "${{ github.event_name }}" = "pull_request" ] || [ "${{ github.event_name }}" = "workflow_dispatch" ]; then
            label_exclude="validation|slow"
          fi""",
            text,
        )
        self.assertIn(
            'if "%GITHUB_EVENT_NAME%"=="workflow_dispatch" set "PULP_CTEST_LABEL_EXCLUDE=validation|slow"',
            text,
        )

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


class MacosNinjaGeneratorTests(unittest.TestCase):
    """The macOS build leg uses the Ninja generator.

    Ninja is faster than the default Makefile generator on the warm /
    incremental builds the self-hosted M1 mostly does. Because the
    self-hosted runners reuse `build-*` dirs (`clean: false`) and CMake
    refuses to reconfigure a dir created with a different generator, the
    Configure step must recreate the build dir when its cached generator
    is not Ninja — otherwise the first build after the switch fails on
    every warm runner with a generator-mismatch error.
    """

    def setUp(self) -> None:
        self.text = BUILD_WORKFLOW.read_text(encoding="utf-8")

    def test_macos_configure_uses_ninja(self) -> None:
        # The generator is scoped to macOS and passed to cmake configure.
        self.assertIn('if [ "${RUNNER_OS}" = "macOS" ]; then', self.text)
        self.assertIn("gen=()", self.text)
        self.assertIn("gen=(-G Ninja)", self.text)
        self.assertIn(
            'cmake -S . -B "$PULP_BUILD_DIR" "${gen[@]}" -DCMAKE_BUILD_TYPE=Release',
            self.text,
        )

    def test_configure_recreates_build_dir_on_generator_change(self) -> None:
        # Warm self-hosted build dirs created with Make must be wiped so
        # the first Ninja configure does not hit a generator mismatch.
        self.assertIn(
            "grep -q '^CMAKE_GENERATOR:INTERNAL=Ninja$' \"$cache\"", self.text
        )
        self.assertIn('rm -rf "$PULP_BUILD_DIR"', self.text)

    def test_ninja_availability_guard(self) -> None:
        # A runner missing ninja installs it rather than failing configure.
        self.assertIn(
            "command -v ninja >/dev/null 2>&1 || brew install ninja", self.text
        )


class CoverageWorkflowSkiaTests(unittest.TestCase):
    def setUp(self) -> None:
        self.text = COVERAGE_WORKFLOW.read_text(encoding="utf-8")

    def test_macos_coverage_fetches_skia_before_run_coverage(self) -> None:
        fetch_name = "- name: Fetch prebuilt Skia (macOS)"
        run_name = "- name: Run coverage suite"
        self.assertIn(fetch_name, self.text)
        self.assertIn(run_name, self.text)
        self.assertLess(self.text.index(fetch_name), self.text.index(run_name))

        start = self.text.index(fetch_name)
        end = self.text.index(run_name)
        step = self.text[start:end]
        self.assertIn("if: matrix.os == 'macos'", step)
        self.assertIn(
            "python3 tools/scripts/fetch_skia_for_release.py darwin-arm64",
            step,
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)

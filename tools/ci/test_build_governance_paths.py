#!/usr/bin/env python3
"""Static, no-build regression checks for the Phase 1 A1 build paths."""

from __future__ import annotations

import re
import subprocess
import sys
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "local-ci"))
from desktop_video_matrix_commands_cli import VIDEO_PROOF_DEMO_SCENARIOS  # noqa: E402


def read_source(relative: str) -> str:
    return (ROOT / relative).read_text()


class BuildGovernancePathTests(unittest.TestCase):
    def test_validate_build_routes_every_build_through_governor(self) -> None:
        source = read_source("validate-build.sh")
        build_lines = [line for line in source.splitlines() if "cmake --build" in line]
        self.assertGreaterEqual(len(build_lines), 2)
        for line in build_lines:
            self.assertIn("governed-build.sh", line)
            self.assertNotRegex(line, r"(?:^|\s)(?:-j\S*|--parallel(?:=|\s))")

    def test_desktop_video_build_recipes_route_through_governor(self) -> None:
        # Exercise the published scenario data, rather than merely checking a
        # comment or a single fixture. Design/video-only scenarios have no
        # prepare build and are intentionally skipped.
        checked = 0
        for scenario in VIDEO_PROOF_DEMO_SCENARIOS:
            for field in ("prepare_command", "command"):
                command = scenario.get(field, "")
                build_count = command.count("cmake --build")
                if not build_count:
                    continue
                checked += build_count
                self.assertEqual(
                    build_count,
                    command.count("governed-build.sh cmake --build"),
                    f"{scenario['id']} {field} has an ungoverned build",
                )
                self.assertNotRegex(command, r"cmake --build[^'\n]*(?:-j\S*|--parallel(?:=|\s))")
        self.assertGreaterEqual(checked, 1)

    def test_sanitizer_workflow_builds_use_governor(self) -> None:
        source = read_source(".github/workflows/sanitizers.yml")
        build_lines = [line for line in source.splitlines() if "cmake --build" in line]
        self.assertGreaterEqual(len(build_lines), 5)
        for line in build_lines:
            self.assertIn("tools/ci/governed-build.sh", line)
            self.assertNotRegex(line, r"(?:^|\s)(?:-j\S*|--parallel(?:=|\s))")

    def test_generated_wasm_fixture_build_uses_governor(self) -> None:
        source = read_source("test/cmake/gpu_audio_web_tests.cmake")
        self.assertIn(
            '"$PULP_ROOT/tools/ci/governed-build.sh" cmake --build "$WORK"',
            source,
        )
        self.assertNotRegex(source, r"cmake --build[^\n]*(?:-j\S*|--parallel(?:=|\s))")

    def test_mcp_build_keeps_governor_and_exit_status(self) -> None:
        source = read_source("tools/mcp/mcp_tools.cpp")
        self.assertIn('"tools" / "ci" / "governed-build.sh"', source)
        self.assertIn("exec_with_status(build)", source)
        self.assertIn("if (result.failed())", source)
        self.assertNotRegex(source, r'exec\(\s*"cmake --build')

    def test_design_build_keeps_governor_wrapper(self) -> None:
        source = read_source("tools/cli/cmd_design.cpp")
        self.assertIn('"tools" / "ci" / "governed-build.sh"', source)
        self.assertIn("run_with_spinner(build_command", source)
        self.assertNotIn('run_with_spinner("cmake --build', source)

    def test_import_validation_pulp_builds_route_through_governor(self) -> None:
        scripts = (
            "v0-roundtrip.sh",
            "figma-roundtrip.sh",
            "pencil-roundtrip.sh",
            "stitch-roundtrip.sh",
            "rn-roundtrip.sh",
            "designmd-roundtrip.sh",
            "jsx-roundtrip.sh",
        )
        for name in scripts:
            source = read_source(f"tools/import-validation/{name}")
            build_lines = [
                line
                for line in source.splitlines()
                if "cmake --build" in line and "Skip the cmake" not in line
            ]
            self.assertGreaterEqual(len(build_lines), 1, name)
            for line in build_lines:
                self.assertIn("governed-build.sh", line, name)
                self.assertNotRegex(line, r"(?:^|\s)(?:-j\S*|--parallel(?:=|\s))", name)
            if "PULP_BUILD_JOBS" in source:
                self.assertIn("deprecated; governed-build selects parallelism", source, name)

    def test_wasm_fixture_lane_routes_build_through_governor(self) -> None:
        source = read_source("tools/ci/wasm-fixture-lane.sh")
        build_lines = [
            line for line in source.splitlines()
            if "cmake --build" in line
        ]
        self.assertGreaterEqual(len(build_lines), 1)
        for line in build_lines:
            self.assertIn("governed-build.sh", line)
            self.assertNotRegex(line, r"(?:^|\s)(?:-j\S*|--parallel(?:=|\s))")
        self.assertIn('PULP_BUILD_JOBS="$jobs"', source)

    def test_tart_guest_build_routes_through_governor(self) -> None:
        source = read_source("tools/ci/tart-run-job.sh")
        build_lines = [
            line
            for line in source.splitlines()
            if "cmake --build" in line and not line.lstrip().startswith("#")
        ]
        self.assertEqual(len(build_lines), 1)
        self.assertIn("governed-build.sh\" cmake --build", build_lines[0])
        self.assertNotRegex(
            build_lines[0], r"cmake --build[^\n]*(?:-j\S*|--parallel(?:=|\s))"
        )

    def test_wclap_cloudflare_builds_route_through_governor(self) -> None:
        source = read_source(".github/workflows/wclap-cloudflare.yml")
        build_lines = [
            line for line in source.splitlines() if "cmake --build" in line
        ]
        self.assertEqual(len(build_lines), 4)
        for line in build_lines:
            self.assertIn('bash "$GITHUB_WORKSPACE/tools/ci/governed-build.sh" cmake --build', line)
            self.assertNotRegex(
                line, r"cmake --build[^\n]*(?:-j\S*|--parallel(?:=|\s))"
            )

    def test_sanitizer_and_coverage_helpers_route_builds_through_governor(self) -> None:
        """Local diagnostic lanes must keep their --jobs cap at the governor boundary."""
        for relative in (
            "scripts/run_asan.sh",
            "scripts/run_tsan.sh",
            "scripts/run_ubsan.sh",
            "scripts/run_coverage.sh",
        ):
            source = read_source(relative)
            build_lines = [
                line for line in source.splitlines() if "cmake --build" in line
            ]
            self.assertEqual(len(build_lines), 1, relative)
            self.assertIn("governed-build.sh", source, relative)
            self.assertIn('PULP_BUILD_JOBS="${JOBS}"', source, relative)
            self.assertNotRegex(
                source,
                r"cmake --build[^\n]*(?:-j\S*|--parallel(?:=|\s))",
                relative,
            )

    def test_governor_probe_is_bounded_and_has_a_receipt(self) -> None:
        source = read_source("tools/ci/governed-build.sh")
        self.assertIn('export CMAKE_BUILD_PARALLEL_LEVEL="$jobs"', source)
        self.assertIn('"$@" || rc=$?', source)
        self.assertIn('exit "$rc"', source)
        self.assertIn('if [ "${1:-}" = "--dry-run" ]', source)
        self.assertIn('if [ "${1:-}" = "--probe-jobs" ]', source)

        result = subprocess.run(
            ["bash", str(ROOT / "tools" / "ci" / "governed-build.sh"), "--probe-jobs"],
            cwd=ROOT,
            env={"PULP_TARTCI_LEASES": "0", "PULP_BUILD_METRICS": "0"},
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertRegex(result.stdout.strip(), r"^jobs=[1-9][0-9]* grant=tier0$")

        dry_run = subprocess.run(
            [
                "bash",
                str(ROOT / "tools" / "ci" / "governed-build.sh"),
                "--dry-run",
                "cmake",
                "--build",
                "build",
                "--target",
                "pulp-test-state",
            ],
            cwd=ROOT,
            env={"PULP_TARTCI_LEASES": "0", "PULP_BUILD_METRICS": "0"},
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(dry_run.returncode, 0, dry_run.stderr)
        self.assertRegex(
            dry_run.stdout.strip(),
            r"^governed-build dry-run receipt: jobs=[1-9][0-9]* grant=tier0 command=.*cmake --build build --target pulp-test-state$",
        )

    def test_intel_portability_build_routes_through_governor(self) -> None:
        source = read_source(".github/workflows/intel-portability.yml")
        build_lines = [
            line for line in source.splitlines()
            if "cmake --build" in line and not line.lstrip().startswith("#")
        ]
        self.assertEqual(len(build_lines), 1)
        self.assertIn("tools/ci/governed-build.sh cmake --build", build_lines[0])
        self.assertNotRegex(
            build_lines[0], r"cmake --build[^\n]*(?:-j\S*|--parallel(?:=|\s))"
        )

    def test_web_plugins_builds_route_through_governor(self) -> None:
        source = read_source(".github/workflows/web-plugins.yml")
        build_lines = [
            line for line in source.splitlines()
            if "cmake --build" in line and not line.lstrip().startswith("#")
        ]
        self.assertGreaterEqual(len(build_lines), 10)
        for line in build_lines:
            self.assertIn("governed-build.sh", line)
            self.assertNotRegex(
                line, r"cmake --build[^\n]*(?:-j\S*|--parallel(?:=|\s))"
            )
        # These two jobs set a nested working-directory; the wrapper must be
        # reached through the workspace root rather than that directory.
        self.assertIn(
            'bash "$GITHUB_WORKSPACE/tools/ci/governed-build.sh" cmake --build build',
            build_lines[0],
        )
        self.assertIn(
            'bash "$GITHUB_WORKSPACE/tools/ci/governed-build.sh" cmake --build build',
            build_lines[1],
        )

    def test_timeline_fuzz_builds_route_through_governor(self) -> None:
        source = read_source(".github/workflows/timeline-fuzz.yml")
        build_lines = [
            line for line in source.splitlines()
            if "cmake --build" in line and not line.lstrip().startswith("#")
        ]
        self.assertEqual(len(build_lines), 2)
        for line in build_lines:
            self.assertIn("tools/ci/governed-build.sh", line)
            self.assertNotRegex(
                line, r"cmake --build[^\n]*(?:-j\S*|--parallel(?:=|\s))"
            )


if __name__ == "__main__":
    unittest.main()

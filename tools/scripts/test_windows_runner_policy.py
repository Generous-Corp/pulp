#!/usr/bin/env python3
"""Lock the split between stable Windows runtime and latest-toolchain lanes.

The authoritative functional suite deliberately uses ``windows-2022`` while
release, coverage, scheduled, and standalone compile validation continue to
exercise ``windows-latest``.  Read every operative workflow here so the policy
cannot self-agree inside its documentation or Shipyard mirror while a real lane
silently drifts.

Run:  python3 tools/scripts/test_windows_runner_policy.py
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
WORKFLOWS = REPO_ROOT / ".github" / "workflows"


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def job(workflow: str, name: str) -> str:
    marker = f"\n  {name}:\n"
    if marker not in workflow:
        raise AssertionError(f"workflow job not found: {name}")
    remainder = workflow.split(marker, 1)[1]
    return re.split(r"\n  [A-Za-z0-9_-]+:\n", remainder, 1)[0]


class WindowsRunnerPolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.build = read(WORKFLOWS / "build.yml")
        cls.release = read(WORKFLOWS / "release-cli.yml")
        cls.coverage = read(WORKFLOWS / "coverage.yml")
        cls.nightly = read(WORKFLOWS / "cross-platform-check.yml")
        cls.release_resolver = read(
            REPO_ROOT / "tools" / "scripts" / "resolve_release_runners.py"
        )
        cls.profile = read(
            REPO_ROOT
            / ".shipyard"
            / "ci-profiles"
            / "normal-local-fast.toml"
        )

    def test_authoritative_functional_suite_uses_windows_2022(self) -> None:
        resolver = job(self.build, "resolve-provider")
        windows_call = resolver.split(
            '"--target-name", "Windows (x64)"', 1
        )[1].split("])", 1)[0]
        self.assertIn('"--github-hosted-label", "windows-2022"', windows_call)
        self.assertNotIn("windows-latest", windows_call)
        self.assertEqual(
            len(re.findall(r"(?m)^\s*windows_runs_on\s*=", resolver)), 1
        )
        self.assertIn(
            "matrix_json: ${{ steps.resolve.outputs.matrix_json }}", resolver
        )
        self.assertIn(
            '"runs_on_json": windows_runs_on', resolver
        )
        self.assertIn(
            'handle.write(f"matrix_json={json.dumps(matrix)}\\n")', resolver
        )

        consumer = job(self.build, "build")
        self.assertIn("needs: [resolve-provider, classify]", consumer)
        self.assertIn(
            "matrix: ${{ fromJSON(needs.resolve-provider.outputs.matrix_json) }}",
            consumer,
        )
        self.assertIn(
            "runs-on: ${{ fromJSON(matrix.runs_on_json) }}", consumer
        )

    def test_standalone_latest_toolchain_gates_stay_on_latest(self) -> None:
        for name in (
            "windows-msvc-release-gate",
            "windows-midi2-gate",
            "windows-ble-gate",
        ):
            with self.subTest(job=name):
                section = job(self.build, name)
                self.assertIn("runs-on: windows-latest", section)
                self.assertNotIn("windows-2022", section)

    def test_release_build_and_smoke_stay_on_latest(self) -> None:
        resolver = job(self.release, "resolve-macos-runner")
        self.assertIn(
            "python3 tools/scripts/resolve_release_runners.py --github-output",
            resolver,
        )
        self.assertRegex(
            self.release_resolver,
            r'(?m)^\s*"windows-x64":\s*"windows-latest",$',
        )
        self.assertIn(
            'print("map=" + json.dumps(resolved))', self.release_resolver
        )
        self.assertIn(
            "map: ${{ steps.resolve.outputs.map }}", resolver
        )
        clean_env = dict(os.environ)
        for name in (
            "DARWIN_ARM64",
            "DARWIN_X64",
            "LINUX_X64",
            "LINUX_ARM64",
            "WINDOWS_X64",
            "WINDOWS_ARM64",
            "LOCAL_MACOS",
            "NAMESPACE_JSON",
        ):
            clean_env.pop(name, None)
        emitted = subprocess.run(
            [
                sys.executable,
                str(
                    REPO_ROOT
                    / "tools"
                    / "scripts"
                    / "resolve_release_runners.py"
                ),
                "--github-output",
            ],
            capture_output=True,
            text=True,
            check=False,
            env=clean_env,
        )
        self.assertEqual(emitted.returncode, 0, emitted.stderr)
        map_line = next(
            line for line in emitted.stdout.splitlines() if line.startswith("map=")
        )
        emitted_map = json.loads(map_line.removeprefix("map="))
        self.assertEqual(emitted_map["windows-x64"], "windows-latest")
        for name in ("build-cli", "smoke-cli"):
            with self.subTest(job=name):
                section = job(self.release, name)
                self.assertRegex(
                    section, r"(?m)^\s*needs:.*resolve-macos-runner"
                )
                self.assertRegex(
                    section,
                    r"(?m)^\s*- os: windows-latest\n\s+platform: windows-x64$",
                )
                self.assertIn(
                    "runs-on: ${{ fromJSON(needs.resolve-macos-runner.outputs.map)"
                    "[matrix.platform] }}",
                    section,
                )
                self.assertNotIn("windows-2022", section)

    def test_coverage_stays_on_latest(self) -> None:
        resolver = job(self.coverage, "resolve-runners")
        self.assertIn("windows='\"windows-latest\"'", resolver)
        self.assertRegex(resolver, r"(?m)^\s+windows-latest\)$")
        self.assertNotIn("windows-2022", resolver)
        # Exactly the PR literal and non-PR resolver assignment. A later
        # reassignment before publication would silently sever this chain.
        self.assertEqual(len(re.findall(r"(?m)^\s*windows\s*=", resolver)), 2)
        self.assertIn(
            "windows_runs_on: ${{ steps.resolve.outputs.windows_runs_on }}",
            resolver,
        )
        self.assertIn('echo "windows_runs_on=${windows}"', resolver)

        matrix_config = job(self.coverage, "matrix-config")
        self.assertIn("needs: [resolve-runners]", matrix_config)
        self.assertIn(
            "WINDOWS_RUNS_ON_JSON: "
            "${{ needs.resolve-runners.outputs.windows_runs_on }}",
            matrix_config,
        )
        self.assertIn(
            '--argjson runs_on "${WINDOWS_RUNS_ON_JSON}"', matrix_config
        )
        self.assertIn(
            "outputs:\n      matrix: ${{ steps.build.outputs.matrix }}",
            matrix_config,
        )
        self.assertIn(
            'echo "matrix=${matrix}" >> "${GITHUB_OUTPUT}"', matrix_config
        )

        consumer = job(self.coverage, "coverage")
        self.assertRegex(consumer, r"(?m)^\s*needs:.*matrix-config")
        self.assertIn(
            "matrix: ${{ fromJSON(needs.matrix-config.outputs.matrix) }}",
            consumer,
        )
        self.assertIn(
            "runs-on: ${{ fromJSON(matrix.runs_on_json) }}", consumer
        )

    def test_scheduled_cross_platform_suite_stays_on_latest(self) -> None:
        windows = job(self.nightly, "windows")
        self.assertIn("runs-on: windows-latest", windows)
        self.assertNotIn("windows-2022", windows)

    def test_shipyard_mirror_preserves_runtime_and_latest_targets(self) -> None:
        pr_windows = self.profile.split(
            '[repo."danielraffel/pulp".pr.windows]', 1
        )[1].split("\n[", 1)[0]
        coverage_windows = self.profile.split(
            '[repo."danielraffel/pulp".coverage.windows]', 1
        )[1].split("\n[", 1)[0]
        scheduled = self.profile.split(
            '[repo."danielraffel/pulp".scheduled.nightly_intel]', 1
        )[1].split("\n[", 1)[0]
        runtime = self.profile.split(
            '[targets."github.windows-x64-runtime"]', 1
        )[1].split("\n[", 1)[0]
        latest = self.profile.split(
            '[targets."github.windows-x64"]', 1
        )[1].split("\n[", 1)[0]

        self.assertIn('targets = ["github.windows-x64-runtime"]', pr_windows)
        self.assertIn('targets = ["github.windows-x64"]', coverage_windows)
        self.assertIn('"github.windows-x64"', scheduled)
        self.assertIn('runs_on_json = "windows-2022"', runtime)
        self.assertIn('runs_on_json = "windows-latest"', latest)


if __name__ == "__main__":
    unittest.main(verbosity=2)

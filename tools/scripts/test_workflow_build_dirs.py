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

import re
import subprocess
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
BUILD_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "build.yml"
BUILD_MACOS_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "build-macos.yml"
COVERAGE_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "coverage.yml"
SANITIZERS_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "sanitizers.yml"
TIMELINE_FUZZ_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "timeline-fuzz.yml"
CONTROL_SDK_CONSUMER = REPO_ROOT / "test" / "cmake" / "test_control_sdk_consumer.cmake"


class WorkflowBuildDirTests(unittest.TestCase):
    def test_build_matrix_uses_matrix_scoped_build_dir(self) -> None:
        text = BUILD_WORKFLOW.read_text(encoding="utf-8")

        self.assertIn("PULP_BUILD_DIR: build-${{ matrix.key }}", text)
        self.assertIn('cmake -S . -B "$PULP_BUILD_DIR"', text)
        self.assertIn("- name: Build\n        shell: bash", text)
        self.assertIn('cmake --build "$PULP_BUILD_DIR" --config Release', text)
        self.assertIn('ctest --test-dir "$PULP_BUILD_DIR"', text)
        self.assertIn('ctest --test-dir "%PULP_BUILD_DIR%"', text)
        self.assertIn('label_exclude="validation|slow', text)
        self.assertIn(
            'set "PULP_CTEST_LABEL_EXCLUDE=validation|slow|windows-pr-quarantine',
            text,
        )

        self.assertNotIn("working-directory: build", text)

    def test_build_workflow_shipyard_dispatch_excludes_slow_ctests(self) -> None:
        """The three gating events share one reduced ctest label set.

        `pull_request`, `workflow_dispatch`, and `merge_group` all gate a
        landing, so they must exclude the same labels — the slow suite plus the
        relative-timing suites, which measure ratios against an in-run baseline
        and cannot be a required gate on a runner hosting concurrent build VMs.
        `push` deliberately keeps them (informational). Asserted by label
        membership rather than one literal line so adding a label to the set
        does not require restating the whole condition here.
        """
        text = BUILD_WORKFLOW.read_text(encoding="utf-8")

        # Deliberately single-line (`[^\n]*`): build.yml has other
        # `event_name == pull_request` guards, and a DOTALL match would span
        # from the first of them to this one and read the wrong label set.
        posix_condition = re.search(
            r'if \[ "\$\{\{ github\.event_name \}\}" = "pull_request" \][^\n]*\n'
            r'\s*label_exclude="(?P<labels>[^"]+)"\n',
            text,
        )
        self.assertIsNotNone(
            posix_condition, "reduced-ctest condition not found in build.yml"
        )
        guard = posix_condition.group(0)
        for event in ("pull_request", "workflow_dispatch", "merge_group"):
            with self.subTest(event=event, shell="posix"):
                self.assertIn(f'= "{event}"', guard)
        posix_labels = set(posix_condition.group("labels").split("|"))
        self.assertLessEqual(
            {"validation", "slow", "performance", "bench", "quality-lab"},
            posix_labels,
        )

        for event in ("pull_request", "workflow_dispatch", "merge_group"):
            with self.subTest(event=event, shell="cmd"):
                cmd = re.search(
                    rf'if "%GITHUB_EVENT_NAME%"=="{event}" '
                    rf'set "PULP_CTEST_LABEL_EXCLUDE=(?P<labels>[^"]+)"',
                    text,
                )
                self.assertIsNotNone(cmd, f"no cmd label exclude for {event}")
                self.assertLessEqual(
                    posix_labels | {"windows-pr-quarantine"},
                    set(cmd.group("labels").split("|")),
                )

    def test_build_workflow_shipyard_dispatch_skips_examples_with_gpu_off(self) -> None:
        text = BUILD_WORKFLOW.read_text(encoding="utf-8")

        self.assertIn(
            "cmake_args=(-DCMAKE_BUILD_TYPE=Release -DPULP_BUILD_EXAMPLES=OFF)",
            text,
        )
        self.assertIn(
            """if [ "${{ github.event_name }}" = "workflow_dispatch" ]; then
            cmake_args+=(-DPULP_ENABLE_GPU=OFF)
          fi""",
            text,
        )
        self.assertIn(
            'cmake -S . -B "$PULP_BUILD_DIR" "${gen[@]}" ${cmake_extra[@]+"${cmake_extra[@]}"} "${cmake_args[@]}"',
            text,
        )

    def test_build_workflow_fetches_event_pinned_capability_base(self) -> None:
        text = BUILD_WORKFLOW.read_text(encoding="utf-8")

        fetch_step = """- name: Fetch protected capability base
        if: github.event_name == 'pull_request' || github.event_name == 'workflow_dispatch'
        env:
          PULP_CAPABILITY_PR_BASE_SHA: ${{ github.event.pull_request.base.sha }}
        shell: bash
        run: |
          if [ -n "$PULP_CAPABILITY_PR_BASE_SHA" ]; then
            git fetch --no-tags --depth=1 origin \\
              "+$PULP_CAPABILITY_PR_BASE_SHA:refs/remotes/origin/main"
          else
            git fetch --no-tags --depth=1 origin main:refs/remotes/origin/main
          fi"""
        self.assertIn(fetch_step, text)
        self.assertLess(text.index(fetch_step), text.index("- name: Test (non-Windows)"))

    def test_macos_retarget_matches_required_gate_selection(self) -> None:
        text = BUILD_MACOS_WORKFLOW.read_text(encoding="utf-8")

        self.assertIn("- name: Fetch protected capability base", text)
        self.assertIn(
            'git fetch --no-tags --depth=1 origin "+refs/heads/main:refs/remotes/origin/main"',
            text,
        )
        self.assertIn(
            '-LE "validation|slow|performance|bench|quality-lab"',
            text,
        )
        self.assertIn("--repeat until-pass:2", text)
        self.assertLess(
            text.index("- name: Fetch protected capability base"),
            text.index("- name: Test"),
        )

    def test_installed_control_consumer_requests_cxx20(self) -> None:
        text = CONTROL_SDK_CONSUMER.read_text(encoding="utf-8")

        self.assertIn("std::jthread", text)
        self.assertIn("std::stop_token", text)
        self.assertIn(
            "set_target_properties(InstalledControlStandalone_Core PROPERTIES",
            text,
        )
        self.assertIn("CXX_STANDARD 20", text)
        self.assertIn("CXX_STANDARD_REQUIRED ON", text)
        self.assertIn(
            "target_compile_features(InstalledControlStandalone_Core PRIVATE cxx_std_20)",
            text,
            "the generated consumer uses C++20 threading and must pin the "
            "language level instead of inheriting a provider compiler default",
        )

    def test_event_pinned_fetch_repairs_a_shallow_pull_request_checkout(self) -> None:
        def git(
            repo: Path, *args: str, check: bool = True
        ) -> subprocess.CompletedProcess[str]:
            return subprocess.run(
                ["git", *args],
                cwd=repo,
                text=True,
                capture_output=True,
                check=check,
            )

        with tempfile.TemporaryDirectory(prefix="pulp-capability-base-source-") as source_tmp:
            source = Path(source_tmp)
            git(source, "init", "-q", "-b", "main")
            git(source, "config", "user.name", "Pulp CI test")
            git(source, "config", "user.email", "ci-test@pulp.audio")
            (source / "fixture.txt").write_text("base\n", encoding="utf-8")
            git(source, "add", "fixture.txt")
            git(source, "commit", "-q", "-m", "base")
            base_sha = git(source, "rev-parse", "HEAD").stdout.strip()

            git(source, "switch", "-q", "-c", "feature")
            (source / "fixture.txt").write_text("feature\n", encoding="utf-8")
            git(source, "commit", "-qam", "feature")
            head_sha = git(source, "rev-parse", "HEAD").stdout.strip()
            git(source, "switch", "-q", "main")
            git(source, "merge", "-q", "--no-ff", "feature", "-m", "synthetic merge")
            merge_sha = git(source, "rev-parse", "HEAD").stdout.strip()

            with tempfile.TemporaryDirectory(
                prefix="pulp-capability-base-checkout-"
            ) as checkout_tmp:
                checkout = Path(checkout_tmp)
                git(checkout, "init", "-q")
                git(checkout, "remote", "add", "origin", source.as_uri())
                git(
                    checkout,
                    "fetch",
                    "--no-tags",
                    "--depth=1",
                    "origin",
                    f"+{merge_sha}:refs/remotes/pull/1/merge",
                )

                missing_base = git(
                    checkout,
                    "cat-file",
                    "-e",
                    f"{base_sha}^{{commit}}",
                    check=False,
                )
                self.assertNotEqual(missing_base.returncode, 0)

                # A warm checkout may carry a newer origin/main. The force in
                # the workflow is required to restore the event-pinned base.
                git(
                    checkout,
                    "fetch",
                    "--no-tags",
                    "--depth=1",
                    "origin",
                    f"+{head_sha}:refs/remotes/origin/main",
                )
                git(
                    checkout,
                    "fetch",
                    "--no-tags",
                    "--depth=1",
                    "origin",
                    f"+{base_sha}:refs/remotes/origin/main",
                )
                resolved = git(
                    checkout,
                    "rev-parse",
                    "refs/remotes/origin/main^0",
                ).stdout.strip()
                self.assertEqual(resolved, base_sha)

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

    def test_jobs_use_canonical_sanitizer_configuration(self) -> None:
        """Named options own flags and test-only bundle-runtime classification."""
        text = SANITIZERS_WORKFLOW.read_text(encoding="utf-8")
        asan = text.split("  asan:", 1)[1].split("  tsan:", 1)[0]
        tsan = text.split("  tsan:", 1)[1].split("  ubsan:", 1)[0]
        ubsan = text.split("  ubsan:", 1)[1].split("  rtsan:", 1)[0]

        self.assertEqual(asan.count("-DPULP_SANITIZER=address"), 2)
        self.assertEqual(tsan.count("-DPULP_SANITIZER=thread"), 1)
        self.assertEqual(ubsan.count("-DPULP_SANITIZER=undefined"), 1)
        for job in (asan, tsan, ubsan):
            self.assertNotIn("-DCMAKE_CXX_FLAGS=", job)
            self.assertNotIn("-DCMAKE_C_FLAGS=", job)
            self.assertNotIn("-DCMAKE_OBJCXX_FLAGS=", job)
            self.assertNotIn("-DCMAKE_EXE_LINKER_FLAGS=", job)
            self.assertNotIn("-DCMAKE_SHARED_LINKER_FLAGS=", job)

    def test_ubsan_optimizes_instrumented_dsp_certifications(self) -> None:
        """UBSan keeps symbols without running production-scale DSP at O0."""
        text = SANITIZERS_WORKFLOW.read_text(encoding="utf-8")
        ubsan = text.split("  ubsan:", 1)[1].split("  rtsan:", 1)[0]

        self.assertIn("-DCMAKE_BUILD_TYPE=RelWithDebInfo", ubsan)
        self.assertIn("-DPULP_SANITIZER=undefined", ubsan)
        self.assertNotIn("-DCMAKE_BUILD_TYPE=Debug", ubsan)

    def test_tsan_selects_network_reader_thread_suites(self) -> None:
        text = SANITIZERS_WORKFLOW.read_text(encoding="utf-8")
        regex_line = next(
            line for line in text.splitlines() if "--tests-regex" in line
        )

        for suite in (
            "Osc",
            "WebSocket",
            "Channel",
            "Control",
            "Broker",
            "Grant",
            "Admission",
            "Receipt",
            "Artifact",
            "control protocol",
            "control artifact",
            "control service",
            "control client",
            "control main-thread",
            "per-call",
        ):
            with self.subTest(suite=suite):
                self.assertIn(suite, regex_line)

        pull_request_paths = text.split("  pull_request:", 1)[1].split(
            "  schedule:", 1
        )[0]
        self.assertIn("- 'inspect/**'", pull_request_paths)

    def test_fuzz_workflow_builds_and_runs_control_protocol_targets(self) -> None:
        text = TIMELINE_FUZZ_WORKFLOW.read_text(encoding="utf-8")

        self.assertIn("pulp-test-control-protocol-fuzz", text)
        self.assertIn("pulp-fuzz-control-protocol", text)
        self.assertIn("PULP_CONTROL_PROTOCOL_FUZZ_SEED:", text)
        self.assertIn("PULP_CONTROL_PROTOCOL_FUZZ_CASES:", text)
        self.assertIn("pulp-generate-control-protocol-fuzz-corpus", text)
        self.assertIn(
            "./build-libfuzzer/test/pulp-generate-control-protocol-fuzz-corpus",
            text,
        )
        self.assertIn("./build-libfuzzer/test/pulp-fuzz-control-protocol", text)
        self.assertRegex(
            text,
            r"pulp-fuzz-control-protocol \\\n\s+fuzz-corpus/control-protocol \\",
        )
        self.assertIn("fuzz-corpus/timeline", text)
        self.assertRegex(
            text,
            r"pulp-fuzz-control-protocol[\s\S]*?"
            r"-max_total_time=\$\{\{ inputs\.libfuzzer_seconds \|\| 900 \}\}",
        )
        self.assertIn("- 'inspect/include/pulp/inspect/control_protocol.hpp'", text)
        self.assertIn("- 'inspect/src/control_protocol*.cpp'", text)
        self.assertIn("- 'inspect/src/control_json_*.cpp'", text)

    def test_macos_build_runs_ios_compile_gate_after_configure(self) -> None:
        text = BUILD_WORKFLOW.read_text(encoding="utf-8")
        configure = 'cmake -S . -B "$PULP_BUILD_DIR"'
        compile_gate = (
            "bash test/cmake/test_ios_compile_gate.sh \\\n"
            '              "$GITHUB_WORKSPACE" "$PULP_BUILD_DIR-ios"'
        )
        build = 'cmake --build "$PULP_BUILD_DIR" --config Release'

        self.assertIn(compile_gate, text)
        self.assertLess(text.index(configure), text.index(compile_gate))
        self.assertLess(text.index(compile_gate), text.index(build))


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
            'cmake -S . -B "$PULP_BUILD_DIR" "${gen[@]}" ${cmake_extra[@]+"${cmake_extra[@]}"} "${cmake_args[@]}"',
            self.text,
        )

    def test_windows_configure_uses_short_fetchcontent_base(self) -> None:
        # MSBuild still evaluates some FetchContent subbuild paths through
        # MAX_PATH-limited metadata. Keep Windows dependency subbuilds short.
        self.assertIn('if [ "${RUNNER_OS}" = "Windows" ]; then', self.text)
        self.assertIn('cmake_extra=(-DFETCHCONTENT_BASE_DIR="$PWD/fc")', self.text)
        self.assertIn(
            'cmake -S . -B "$PULP_BUILD_DIR" "${gen[@]}" ${cmake_extra[@]+"${cmake_extra[@]}"} "${cmake_args[@]}"',
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


class CoverageWorkflowExampleGraphTests(unittest.TestCase):
    def setUp(self) -> None:
        self.text = COVERAGE_WORKFLOW.read_text(encoding="utf-8")

    def test_coverage_preserves_example_driven_tests_and_fetches_skia(self) -> None:
        self.assertIn(
            "-DPULP_BUILD_EXAMPLES=ON",
            self.text,
        )
        self.assertIn("fetch_skia_for_release.py darwin-arm64", self.text)


if __name__ == "__main__":
    unittest.main(verbosity=2)

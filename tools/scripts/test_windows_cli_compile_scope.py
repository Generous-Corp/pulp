#!/usr/bin/env python3
"""Tests for tools/scripts/windows_cli_compile_scope.py.

The positive cases are the ones the lane exists for; the negative cases are the
ones that would spend a hosted Windows runner on every pull request, which is
what build.yml removed Windows from PR heads to stop.

Run:  python3 tools/scripts/test_windows_cli_compile_scope.py
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import windows_cli_compile_scope as scope  # noqa: E402

WIN_GUARDED = "#ifdef _WIN32\nstatic int x;\n#endif\n"
PORTABLE = "int portable() { return 1; }\n"


def reader(files: dict[str, str]):
    return files.get


class Relevance(unittest.TestCase):
    def test_the_incident_file_is_relevant(self) -> None:
        # The MSVC-only C2668 that blocked three releases lived here.
        path = "tools/cli/kit_profile_verification.cpp"
        self.assertIsNotNone(scope.relevance(path, reader({path: PORTABLE})))

    def test_mcp_and_cmake_inputs_are_relevant(self) -> None:
        for path in (
            "tools/mcp/pulp_mcp_main.cpp",
            "tools/cli/CMakeLists.txt",
            "tools/cmake/PulpDependencies.cmake",
        ):
            with self.subTest(path=path):
                self.assertIsNotNone(scope.relevance(path, reader({})))

    def test_own_workflow_and_classifier_are_relevant(self) -> None:
        for path in sorted(scope.ALWAYS_RELEVANT_FILES):
            with self.subTest(path=path):
                self.assertIsNotNone(scope.relevance(path, reader({})))

    def test_core_source_with_a_windows_branch_is_relevant(self) -> None:
        for marker in ("_WIN32", "_WIN64", "_MSC_VER", "WIN32_LEAN_AND_MEAN"):
            with self.subTest(marker=marker):
                path = "core/runtime/src/process.cpp"
                text = f"#if defined({marker})\n#endif\n"
                self.assertIsNotNone(scope.relevance(path, reader({path: text})))

    def test_windows_platform_paths_are_relevant_without_reading(self) -> None:
        for path in (
            "core/midi/platform/win/winrt_midi_device.cpp",
            "core/midi/src/ble_midi_win.cpp",
            "core/platform/windows/registry.hpp",
            "core/render/src/surface_win32.cpp",
        ):
            with self.subTest(path=path):
                self.assertIsNotNone(scope.relevance(path, reader({})))

    def test_portable_core_source_is_not_relevant(self) -> None:
        path = "core/signal/src/biquad.cpp"
        self.assertIsNone(scope.relevance(path, reader({path: PORTABLE})))

    def test_names_that_merely_contain_win_are_not_relevant(self) -> None:
        # A substring match on "win" would put darwin and window code on the
        # Windows runner; neither is Windows-specific.
        for path in (
            "core/platform/src/darwin_paths.cpp",
            "core/view/src/window_host.cpp",
            "core/view/src/mac/window_host_gpu.mm.hpp",
        ):
            with self.subTest(path=path):
                self.assertIsNone(scope.relevance(path, reader({path: PORTABLE})))

    def test_tests_examples_and_external_are_out_of_scope(self) -> None:
        for path in (
            "test/test_runtime_process.cpp",
            "examples/pulp-synth/src/main.cpp",
            "external/choc/choc_Platform.h",
        ):
            with self.subTest(path=path):
                self.assertIsNone(scope.relevance(path, reader({path: WIN_GUARDED})))

    def test_root_cmakelists_is_not_relevant(self) -> None:
        # Every release bump edits its VERSION line; release-path-pr-gate.yml
        # already builds it.
        self.assertIsNone(
            scope.relevance("CMakeLists.txt", reader({"CMakeLists.txt": "if(WIN32)\n"}))
        )

    def test_nested_cmakelists_relevant_only_when_it_branches_on_windows(self) -> None:
        path = "core/runtime/CMakeLists.txt"
        self.assertIsNotNone(scope.relevance(path, reader({path: "if(WIN32)\nendif()\n"})))
        self.assertIsNotNone(scope.relevance(path, reader({path: "if(MSVC)\nendif()\n"})))
        self.assertIsNone(scope.relevance(path, reader({path: "add_library(x a.cpp)\n"})))

    def test_docs_and_scripts_are_not_relevant(self) -> None:
        for path in ("docs/guides/release-watchdog.md", "tools/scripts/gates.sh"):
            with self.subTest(path=path):
                self.assertIsNone(scope.relevance(path, reader({})))

    def test_deleted_portable_path_is_not_relevant(self) -> None:
        self.assertIsNone(scope.relevance("core/state/src/gone.cpp", reader({})))

    def test_classify_keeps_order_and_drops_irrelevant(self) -> None:
        files = {"core/a.cpp": WIN_GUARDED, "core/b.cpp": PORTABLE}
        hits = scope.classify(
            ["docs/x.md", "core/a.cpp", "core/b.cpp", "tools/cli/y.cpp"],
            reader(files),
        )
        self.assertEqual([path for path, _ in hits], ["core/a.cpp", "tools/cli/y.cpp"])


class GitDiffEndToEnd(unittest.TestCase):
    """Drive the real CLI against a scratch repository."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self.tmp.name)
        self.git("init", "-q", "-b", "main")
        self.git("config", "user.email", "t@example.com")
        self.git("config", "user.name", "t")
        self.write("README.md", "x\n")
        self.git("add", "README.md")
        self.git("commit", "-q", "-m", "base")
        self.base = self.git("rev-parse", "HEAD").strip()

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def git(self, *args: str) -> str:
        return subprocess.run(
            ["git", *args], cwd=self.repo, check=True, capture_output=True, text=True
        ).stdout

    def write(self, rel: str, text: str) -> None:
        path = self.repo / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def run_cli(self, *args: str) -> tuple[int, str, str]:
        out = Path(self.tmp.name) / "gh_output"
        out.write_text("", encoding="utf-8")
        env = {**os.environ, "GITHUB_OUTPUT": str(out)}
        result = subprocess.run(
            [sys.executable, str(Path(scope.__file__).resolve()), *args],
            cwd=self.repo,
            capture_output=True,
            text=True,
            env=env,
        )
        return result.returncode, result.stdout, out.read_text(encoding="utf-8")

    def commit(self, rel: str, text: str) -> str:
        self.write(rel, text)
        self.git("add", rel)
        self.git("commit", "-q", "-m", rel)
        return self.git("rev-parse", "HEAD").strip()

    def test_windows_guarded_change_is_relevant(self) -> None:
        head = self.commit("core/runtime/src/p.cpp", WIN_GUARDED)
        code, stdout, output = self.run_cli("--base", self.base, "--head", head)
        self.assertEqual(code, 0, stdout)
        self.assertIn("relevant=true", output)
        self.assertIn("core/runtime/src/p.cpp", stdout)

    def test_portable_change_is_not_relevant(self) -> None:
        # Positive control above uses the same instrument; this must be the
        # only difference between the two verdicts.
        head = self.commit("core/runtime/src/p.cpp", PORTABLE)
        code, _, output = self.run_cli("--base", self.base, "--head", head)
        self.assertEqual(code, 0)
        self.assertIn("relevant=false", output)

    def test_push_range_uses_two_dot(self) -> None:
        head = self.commit("tools/cli/new_command.cpp", PORTABLE)
        code, _, output = self.run_cli("--base", self.base, "--head", head, "--two-dot")
        self.assertEqual(code, 0)
        self.assertIn("relevant=true", output)

    def test_unreadable_diff_fails_toward_compiling(self) -> None:
        code, _, output = self.run_cli("--base", "0" * 40, "--head", "HEAD")
        self.assertEqual(code, 3)
        self.assertIn("relevant=true", output)


if __name__ == "__main__":
    unittest.main()

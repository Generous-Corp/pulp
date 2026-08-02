#!/usr/bin/env python3

import unittest
from pathlib import Path

import webclap_relevance


class WebclapRelevanceTests(unittest.TestCase):
    def test_web_build_surfaces_are_relevant(self) -> None:
        for path in (
            "core/view/src/view.cpp",
            "examples/web-demos/wclap-build/CMakeLists.txt",
            "examples/pulp-gain/PulpGain.h",
            "examples/pulp-pluck/PulpPluck.h",
            "examples/super-convolver/processor.cpp",
            "packages/pulp-web-player/src/index.ts",
            "tools/cmake/PulpWclap.cmake",
            "tools/deps/manifest.json",
            "tools/scripts/webclap_relevance.py",
            "tools/scripts/test_webclap_relevance.py",
            ".github/workflows/wclap-cloudflare.yml",
            "CMakeLists.txt",
        ):
            with self.subTest(path=path):
                self.assertTrue(webclap_relevance.is_relevant(path))

    def test_unrelated_surfaces_take_the_fast_path(self) -> None:
        for path in (
            "experimental/pulp-rs/src/fallthrough.rs",
            "docs/runner-operations.md",
            "tools/scripts/native-intel-runner.sh",
            ".github/workflows/build.yml",
            "README.md",
        ):
            with self.subTest(path=path):
                self.assertFalse(webclap_relevance.is_relevant(path))

    def test_expensive_workflow_steps_are_guarded(self) -> None:
        workflow = (
            Path(__file__).resolve().parents[2]
            / ".github"
            / "workflows"
            / "wclap-cloudflare.yml"
        ).read_text(encoding="utf-8")
        self.assertIn("id: relevance", workflow)
        self.assertIn("pulls/${PR_NUMBER}/files", workflow)
        self.assertIn(".previous_filename // empty", workflow)
        self.assertIn("contents/tools/scripts/webclap_relevance.py?ref=${BASE_SHA}", workflow)
        self.assertIn('CHANGED_FILE_COUNT: ${{ github.event.pull_request.changed_files }}', workflow)
        self.assertIn('"${CHANGED_FILE_COUNT:-0}" -ge 3000', workflow)
        self.assertIn("PR-controlled Python never", workflow)
        self.assertNotIn("          GH_TOKEN: ${{ github.token }}", workflow)
        self.assertIn('*) exit "$relevance_status"', workflow)
        self.assertEqual(
            workflow.count("steps.relevance.outputs.run == 'true'"),
            21,
            "every expensive step after relevance classification must be guarded",
        )


if __name__ == "__main__":
    unittest.main()

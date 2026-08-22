#!/usr/bin/env python3

import os
import subprocess
import tempfile
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
        self.assertIn("github.event.merge_group.base_sha", workflow)
        self.assertIn('merge_group)', workflow)
        self.assertIn(
            'git fetch --no-tags --depth=1 origin "$BASE_SHA"', workflow
        )
        self.assertIn(
            'git diff --name-only --no-renames "$BASE_SHA" "$GITHUB_SHA"',
            workflow,
        )
        self.assertIn("contents/tools/scripts/webclap_relevance.py?ref=${BASE_SHA}", workflow)
        self.assertIn('CHANGED_FILE_COUNT: ${{ github.event.pull_request.changed_files }}', workflow)
        self.assertIn('"${CHANGED_FILE_COUNT:-0}" -ge 3000', workflow)
        self.assertIn("PR-controlled Python never", workflow)
        self.assertNotIn("          GH_TOKEN: ${{ github.token }}", workflow)
        self.assertIn(
            'git show "$BASE_SHA:tools/scripts/generated_version_bump_check.py"',
            workflow,
        )
        self.assertIn('--event-path "${{ github.event_path }}"', workflow)
        self.assertIn("WebCLAP proof skipped: exact generated version-bump", workflow)
        self.assertLess(
            workflow.index('GH_TOKEN="${{ github.token }}" python3 "$trusted_bump_check"'),
            workflow.index("pulls/${PR_NUMBER}/files"),
            "the token-bearing verifier must execute only protected-base code",
        )
        self.assertIn('*) exit "$relevance_status"', workflow)
        self.assertEqual(
            workflow.count("steps.relevance.outputs.run == 'true'"),
            21,
            "every expensive step after relevance classification must be guarded",
        )

    def test_merge_group_diff_keeps_both_sides_of_a_rename(self) -> None:
        git_env = os.environ.copy()
        local_git_env = subprocess.check_output(
            ["git", "rev-parse", "--local-env-vars"], text=True, env=git_env
        ).splitlines()
        for name in local_git_env:
            git_env.pop(name, None)

        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)
            subprocess.run(["git", "init", "-q", repo], check=True, env=git_env)
            subprocess.run(
                ["git", "-C", repo, "config", "user.name", "test"],
                check=True,
                env=git_env,
            )
            subprocess.run(
                ["git", "-C", repo, "config", "user.email", "test@example.com"],
                check=True,
                env=git_env,
            )
            old_path = repo / "core" / "view" / "old.cpp"
            old_path.parent.mkdir(parents=True)
            old_path.write_text("content\n", encoding="utf-8")
            subprocess.run(
                ["git", "-C", repo, "add", "."], check=True, env=git_env
            )
            subprocess.run(
                ["git", "-C", repo, "commit", "-qm", "base"],
                check=True,
                env=git_env,
            )
            base = subprocess.check_output(
                ["git", "-C", repo, "rev-parse", "HEAD"],
                text=True,
                env=git_env,
            ).strip()
            new_path = repo / "docs" / "old.cpp"
            new_path.parent.mkdir(parents=True)
            old_path.rename(new_path)
            subprocess.run(
                ["git", "-C", repo, "add", "-A"], check=True, env=git_env
            )
            subprocess.run(
                ["git", "-C", repo, "commit", "-qm", "rename"],
                check=True,
                env=git_env,
            )
            head = subprocess.check_output(
                ["git", "-C", repo, "rev-parse", "HEAD"],
                text=True,
                env=git_env,
            ).strip()

            changed = subprocess.check_output(
                [
                    "git",
                    "-C",
                    repo,
                    "diff",
                    "--name-only",
                    "--no-renames",
                    base,
                    head,
                ],
                text=True,
                env=git_env,
            ).splitlines()

            self.assertEqual(changed, ["core/view/old.cpp", "docs/old.cpp"])
            self.assertTrue(any(webclap_relevance.is_relevant(path) for path in changed))


if __name__ == "__main__":
    unittest.main()

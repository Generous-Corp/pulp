#!/usr/bin/env python3
import tempfile
import unittest
from pathlib import Path
import json
import os
import subprocess

import shell_portability_check as check


class ShellPortabilityTests(unittest.TestCase):
    def assert_findings(self, text: str, expected: bool) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "fixture.sh"
            path.write_text(text, encoding="utf-8")
            self.assertEqual(bool(check.check_file(path)), expected)

    def test_braced_colon_is_safe(self) -> None:
        self.assert_findings('#!/bin/zsh\nprint -- "${root}:test/file.cpp"\n', False)

    def test_unbraced_colon_is_rejected(self) -> None:
        self.assert_findings('#!/bin/zsh\nprint -- "$root:test/file.cpp"\n', True)

    def test_pipestatus_requires_bash_boundary(self) -> None:
        self.assert_findings('#!/bin/zsh\nprint -- "${PIPESTATUS[1]}"\n', True)

    def test_bash_pipestatus_is_allowed(self) -> None:
        self.assert_findings('#!/usr/bin/env bash\nprint -- "${PIPESTATUS[0]}"\n', False)

    def test_bare_pipestatus_is_rejected_in_zsh(self) -> None:
        self.assert_findings('#!/bin/zsh\nprint -- "$PIPESTATUS"\n', True)

    def test_incidental_bash_word_does_not_change_file_shell_detection(self) -> None:
        self.assert_findings(
            "#!/bin/zsh\n# bash compatibility note\ntest ${PIPESTATUS[0]} -eq 0\n",
            True,
        )

    def test_ad_hoc_command_mode_catches_zsh_colon_modifier(self) -> None:
        self.assertEqual(
            bool(check.check_text('git show "$base:core/file.cpp"', "<command>", bash=False)),
            True,
        )

    def test_ad_hoc_command_mode_catches_bash_status_in_zsh(self) -> None:
        self.assertEqual(
            bool(check.check_text('test ${PIPESTATUS[0]} -eq 0', "<command>", bash=False)),
            True,
        )

    def test_ad_hoc_bash_command_allows_pipestatus(self) -> None:
        self.assertEqual(
            bool(check.check_text('test ${PIPESTATUS[0]} -eq 0', "<command>", bash=True)),
            False,
        )

    def test_single_quoted_query_data_is_not_shell_expansion(self) -> None:
        self.assertEqual(
            bool(check.check_text("gh api --raw 'query($owner:String!)'", "<command>")),
            False,
        )

    def test_bash_command_boundary_allows_pipestatus(self) -> None:
        self.assertEqual(
            bool(check.check_text("bash -lc 'test ${PIPESTATUS[0]} -eq 0'", "<command>")),
            False,
        )

    def test_outer_zsh_status_is_still_checked_after_inner_bash(self) -> None:
        self.assertEqual(
            bool(
                check.check_text(
                    "bash -lc 'test ${PIPESTATUS[0]} -eq 0'; test ${PIPESTATUS[0]} -eq 0",
                    "<command>",
                    bash=False,
                )
            ),
            True,
        )

    def test_env_variable_is_not_confused_with_zsh_colon_modifier(self) -> None:
        self.assertEqual(
            bool(check.check_text('print "$env:HOME"', "<command>")),
            False,
        )

    def test_build_output_filter_warns_when_status_is_not_preserved(self) -> None:
        findings = check.check_text(
            "cmake --build build 2>&1 | tail -20", "<command>", bash=False
        )
        self.assertEqual(len(findings), 1)
        self.assertIn("status may be masked", findings[0])
        self.assertIn("set -o pipefail", findings[0])

    def test_ctest_output_filter_warns_for_common_consumers(self) -> None:
        for consumer in ("head -30", "grep FAILED", "sed -n '1,20p'", "tee /tmp/ctest.log"):
            with self.subTest(consumer=consumer):
                findings = check.check_text(
                    f"ctest --test-dir build --output-on-failure | {consumer}",
                    "<command>",
                    bash=False,
                )
                self.assertEqual(len(findings), 1)

    def test_pipefail_makes_build_output_filter_safe(self) -> None:
        self.assertEqual(
            check.check_text(
                "set -euo pipefail\ncmake --build build 2>&1 | tail -20",
                "<script>",
                bash=True,
            ),
            [],
        )

    def test_explicit_pipeline_status_check_is_safe(self) -> None:
        self.assertEqual(
            check.check_text(
                "cmake --build build 2>&1 | tail -20\n"
                "status=${PIPESTATUS[0]}\n"
                "test ${status} -eq 0",
                "<script>",
                bash=True,
            ),
            [],
        )

    def test_pipefail_can_be_disabled_again(self) -> None:
        findings = check.check_text(
            "set -o pipefail\nset +o pipefail\nctest | tail -10",
            "<script>",
            bash=True,
        )
        self.assertEqual(len(findings), 1)

    def test_unrelated_command_after_pipefail_is_conservative(self) -> None:
        findings = check.check_text(
            "set -o pipefail\necho preparing\ncmake --build build | tail -10",
            "<script>",
            bash=True,
        )
        self.assertEqual(len(findings), 1)

    def test_same_line_pipefail_prefix_is_safe(self) -> None:
        self.assertEqual(
            check.check_text(
                "set -o pipefail; cmake --build build | tail -10",
                "<script>",
                bash=True,
            ),
            [],
        )

    def test_comments_are_not_reported_as_masked_pipelines(self) -> None:
        self.assertEqual(
            check.check_text(
                "# cmake --build build 2>&1 | tail -20\n",
                "<script>",
                bash=False,
            ),
            [],
        )

    def test_unrelated_output_filter_is_not_reported(self) -> None:
        self.assertEqual(
            check.check_text("git status --short | tail -20", "<command>", bash=False),
            [],
        )

    def test_quoted_build_text_is_not_reported_as_a_pipeline(self) -> None:
        self.assertEqual(
            check.check_text(
                'printf "%s\\n" "cmake --build build | tail -20"',
                "<command>",
                bash=False,
            ),
            [],
        )

    def test_arbitrary_status_word_does_not_prove_pipeline_safety(self) -> None:
        findings = check.check_text(
            "cmake --build build | tail -20\necho PIPESTATUS",
            "<script>",
            bash=True,
        )
        self.assertEqual(len(findings), 1)

    def test_quoted_pipefail_text_does_not_prove_pipeline_safety(self) -> None:
        findings = check.check_text(
            'echo "set -o pipefail"\ncmake --build build | tail -20',
            "<script>",
            bash=True,
        )
        self.assertEqual(len(findings), 1)

    def test_multiline_build_pipeline_is_checked(self) -> None:
        findings = check.check_text(
            "cmake --build build 2>&1 |\\\n  tail -20",
            "<script>",
            bash=False,
        )
        self.assertEqual(len(findings), 1)

    def test_advisory_hook_warns_without_blocking(self) -> None:
        hook = Path(__file__).parents[2] / "hooks/scripts/shell-portability-hint.sh"
        payload = json.dumps({"tool_input": {"command": "test ${PIPESTATUS[0]} -eq 0"}})
        env = os.environ.copy()
        env["TOOL_INPUT"] = payload
        env["PULP_AGENT_SHELL"] = "zsh"
        result = subprocess.run(
            ["bash", str(hook)],
            cwd=Path(__file__).parents[2],
            env=env,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0)
        self.assertIn("Bash-only PIPESTATUS", result.stdout)

    def test_advisory_hook_accepts_codex_exec_cmd_key(self) -> None:
        hook = Path(__file__).parents[2] / "hooks/scripts/shell-portability-hint.sh"
        payload = json.dumps({"tool_input": {"cmd": "test ${PIPESTATUS[0]} -eq 0"}})
        env = os.environ.copy()
        env["TOOL_INPUT"] = payload
        env["PULP_AGENT_SHELL"] = "zsh"
        result = subprocess.run(
            ["bash", str(hook)],
            cwd=Path(__file__).parents[2],
            env=env,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0)
        self.assertIn("Bash-only PIPESTATUS", result.stdout)

    def test_advisory_hook_does_not_warn_for_bash_shell(self) -> None:
        hook = Path(__file__).parents[2] / "hooks/scripts/shell-portability-hint.sh"
        payload = json.dumps({"tool_input": {"command": "test ${PIPESTATUS[0]} -eq 0"}})
        env = os.environ.copy()
        env["TOOL_INPUT"] = payload
        env["SHELL"] = "/bin/bash"
        result = subprocess.run(
            ["bash", str(hook)],
            cwd=Path(__file__).parents[2],
            env=env,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 0)
        self.assertNotIn("Bash-only PIPESTATUS", result.stdout)

    def test_manifest_is_valid(self) -> None:
        import json
        manifest = json.loads((Path(__file__).with_name("shell_portability_rules.json")).read_text())
        self.assertEqual(manifest["schema_version"], 1)
        self.assertTrue(all(rule["owner"] and rule["review_after"] for rule in manifest["rules"]))


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Tests for the event-dependent ctest argument rules.

Two kinds of assertion live here, and the split matters.

PARITY tests pin the values the required gate used before these rules moved out
of inline workflow shell. They exist so a future edit to the rules cannot
quietly change what the required macOS gate excludes; if one of them fails, the
gate's behaviour changed and that has to be a deliberate, reviewed decision
rather than a side effect.

WIRING tests read `.github/workflows/build.yml` and assert the workflow
actually calls this module and actually passes both of its outputs to ctest. A
decision module that is correct but unreferenced is the exact failure this work
exists to prevent: a guard that cannot fire reads identically to a guard that
never needed to.
"""

from __future__ import annotations

import pathlib
import re
import shlex
import subprocess
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import ctest_gate_args  # noqa: E402

SCRIPT = pathlib.Path(__file__).with_name("ctest_gate_args.py")
REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
BUILD_YML = REPO_ROOT / ".github" / "workflows" / "build.yml"

# The literal label set the workflow used for gate events before extraction.
LEGACY_GATE_LABEL_EXCLUDE = "validation|slow|performance|bench|quality-lab"
# The gate events additionally drop the source-only selftests, which the
# required `Enforce version & skill sync` context runs without a build.
GATE_LABEL_EXCLUDE = LEGACY_GATE_LABEL_EXCLUDE + "|source-selftest"
# The literal label set the workflow used for every other event.
LEGACY_FULL_LABEL_EXCLUDE = "validation"


class LabelExcludeParityTests(unittest.TestCase):
    """The gate's label set must not move without someone choosing to move it."""

    def test_gate_events_keep_the_legacy_reduced_set(self) -> None:
        for event in ("pull_request", "workflow_dispatch", "merge_group"):
            for runner_os in ("macOS", "Linux", "Windows"):
                with self.subTest(event=event, runner_os=runner_os):
                    self.assertEqual(
                        ctest_gate_args.label_exclude(event, runner_os),
                        GATE_LABEL_EXCLUDE,
                    )

    def test_push_on_steady_hosted_runners_keeps_the_heavier_set(self) -> None:
        for runner_os in ("Linux", "Windows"):
            with self.subTest(runner_os=runner_os):
                self.assertEqual(
                    ctest_gate_args.label_exclude("push", runner_os),
                    LEGACY_FULL_LABEL_EXCLUDE,
                )

    def test_unknown_event_falls_back_to_the_heavier_set(self) -> None:
        self.assertEqual(
            ctest_gate_args.label_exclude("schedule", "Linux"),
            LEGACY_FULL_LABEL_EXCLUDE,
        )


class PushMacosLabelTests(unittest.TestCase):
    """A push builds macOS on the shared gate hosts, so it inherits their exclusions.

    Without this the push detector would run `performance`/`bench`/`quality-lab`
    on a host that runs concurrent build VMs, where those relative-timing tests
    measure load rather than the product. A detector that cries wolf gets muted,
    which would reproduce the blind spot it was added to close.
    """

    def test_push_macos_uses_the_shared_host_label_set(self) -> None:
        """Push drops the timing tests but keeps the source-only selftests.

        Push is the only lane that runs the whole macOS suite on main, so the
        source-selftest exclusion that shortens the gate must not reach it.
        """
        self.assertEqual(
            ctest_gate_args.label_exclude("push", "macOS"),
            LEGACY_GATE_LABEL_EXCLUDE,
        )

    def test_runner_os_match_is_case_insensitive(self) -> None:
        for spelling in ("macOS", "macos", "MACOS", "Macos"):
            with self.subTest(spelling=spelling):
                self.assertEqual(
                    ctest_gate_args.label_exclude("push", spelling),
                    LEGACY_GATE_LABEL_EXCLUDE,
                )


class StopOnFailureTests(unittest.TestCase):
    def test_merge_group_stops_at_the_first_failure(self) -> None:
        self.assertEqual(
            ctest_gate_args.stop_on_failure("merge_group"),
            "--stop-on-failure",
        )

    def test_push_runs_the_whole_suite(self) -> None:
        """Push is the only complete macOS signal, so it must not stop early."""
        self.assertEqual(ctest_gate_args.stop_on_failure("push"), "")

    def test_non_gate_events_do_not_stop_early(self) -> None:
        for event in ("pull_request", "workflow_dispatch", "schedule", ""):
            with self.subTest(event=event):
                self.assertEqual(ctest_gate_args.stop_on_failure(event), "")


class ShellOutputTests(unittest.TestCase):
    def _run(self, *args: str) -> str:
        proc = subprocess.run(
            [sys.executable, str(SCRIPT), *args],
            text=True,
            capture_output=True,
            timeout=30,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        return proc.stdout

    def test_shell_output_is_eval_safe(self) -> None:
        """The workflow `eval`s this output, so it must survive shlex round-trip."""
        out = self._run("--event-name", "merge_group", "--runner-os", "macOS")
        parsed = dict(
            token.split("=", 1) for token in shlex.split(out.replace("\n", " "))
        )
        self.assertEqual(parsed["label_exclude"], GATE_LABEL_EXCLUDE)
        self.assertEqual(parsed["stop_on_failure"], "--stop-on-failure")

    def test_empty_stop_flag_round_trips_as_an_empty_token(self) -> None:
        """An unquoted empty value would vanish and shift the eval'd assignments."""
        out = self._run("--event-name", "push", "--runner-os", "Linux")
        self.assertIn("stop_on_failure=''", out)

    def test_json_format(self) -> None:
        import json

        payload = json.loads(
            self._run("--event-name", "push", "--runner-os", "macOS", "--format", "json")
        )
        self.assertEqual(payload["label_exclude"], LEGACY_GATE_LABEL_EXCLUDE)
        self.assertEqual(payload["stop_on_failure"], "")


class WorkflowWiringTests(unittest.TestCase):
    """The rules above are inert unless build.yml actually uses them."""

    @classmethod
    def setUpClass(cls) -> None:
        if not BUILD_YML.is_file():
            raise unittest.SkipTest(f"{BUILD_YML} not present")
        cls.text = BUILD_YML.read_text(encoding="utf-8")

    def test_workflow_invokes_the_decision_script(self) -> None:
        self.assertTrue(
            "tools/ci/ctest_gate_args.py" in self.text,
            "build.yml no longer calls the ctest gate-argument rules; the "
            "stop-on-failure and label-set decisions would silently revert.",
        )

    def test_ctest_command_consumes_the_stop_flag(self) -> None:
        """The flag must reach the ctest command line, not merely be computed."""
        # Only the FULL-suite run is in scope. The workflow also issues
        # single-test `-R` invocations for targeted contract checks, and
        # stopping those at the first failure would mean nothing.
        suite_lines = [
            line
            for line in self.text.splitlines()
            if re.search(r"^\s*ctest --test-dir", line)
            and '-LE "$label_exclude"' in line
        ]
        self.assertTrue(
            suite_lines,
            "no full-suite ctest invocation (the one passing -LE "
            '"$label_exclude") found in build.yml',
        )
        for line in suite_lines:
            self.assertIn(
                "${stop_on_failure}",
                line,
                "the non-Windows ctest command does not pass the computed "
                "stop-on-failure flag, so a merge_group batch would still run "
                "the whole suite after it already knows it failed.",
            )

    def test_ctest_command_consumes_the_label_exclude(self) -> None:
        self.assertTrue(
            re.search(r'ctest --test-dir "\$PULP_BUILD_DIR".*-LE "\$label_exclude"', self.text),
            "the non-Windows ctest command no longer passes the computed label set.",
        )

    def test_workflow_no_longer_hardcodes_the_gate_label_set(self) -> None:
        """A leftover inline copy would be a second source of truth, free to drift."""
        self.assertFalse(
            f'label_exclude="{LEGACY_GATE_LABEL_EXCLUDE}"' in self.text,
            "build.yml still assigns the gate label set inline; that copy will "
            "drift from the tested rules.",
        )


if __name__ == "__main__":
    unittest.main()

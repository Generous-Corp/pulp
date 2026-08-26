#!/usr/bin/env python3
"""Regression tests: an iOS-gate SKIP must not red the required macos check.

`test/cmake/test_ios_compile_gate.sh` reports "not applicable on this runner"
with exit 77, the autotools skip convention. 77 is still nonzero, and the Build
step in `build.yml` runs under GitHub's `shell: bash`, which is
`bash --noprofile --norc -eo pipefail`. Calling the gate bare therefore turns
every skip into a failure of the REQUIRED `macos` check, and `-e` aborts the
step before `cmake --build` runs — so the build the check exists to perform
never happens.

The skip is reachable rather than hypothetical: the gate requires the
`iphonesimulator` and `iphoneos` SDKs, and `tools/ci/tart-provision.sh`
deliberately trims simulator payload out of the golden VM image.

These tests exercise the real shell semantics rather than asserting on the YAML
text, because the failure is a property of how bash treats a nonzero status, not
of how the call is spelled.

Run:
    python3 tools/scripts/test_ios_gate_skip_contract.py
"""

from __future__ import annotations

import re
import subprocess
import tempfile
import unittest
from pathlib import Path

import yaml

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
BUILD_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "build.yml"
GATE = REPO_ROOT / "test" / "cmake" / "test_ios_compile_gate.sh"

# Exactly GitHub's `shell: bash`.
GITHUB_BASH = ["bash", "--noprofile", "--norc", "-eo", "pipefail"]


def run_step(
    body: str, gate_exit: int, *, ios_compile_required: str = "true"
) -> tuple[int, str]:
    """Run `body` under GitHub's shell with the gate stubbed to `gate_exit`."""
    with tempfile.TemporaryDirectory() as tmp:
        stub = Path(tmp) / "gate.sh"
        stub.write_text(
            "#!/usr/bin/env bash\n"
            f'echo "stub gate exiting {gate_exit}"\n'
            f"exit {gate_exit}\n",
            encoding="utf-8",
        )
        stub.chmod(0o755)
        step = Path(tmp) / "step.sh"
        step.write_text(body.replace("@GATE@", str(stub)), encoding="utf-8")
        proc = subprocess.run(
            GITHUB_BASH + [str(step)], capture_output=True, text=True,
            env={
                "RUNNER_OS": "macOS",
                "IOS_COMPILE_REQUIRED": ios_compile_required,
                "PATH": "/usr/bin:/bin:/usr/sbin:/sbin",
            },
        )
        return proc.returncode, proc.stdout + proc.stderr


BARE_CALL = '''
if [ "${RUNNER_OS}" = "macOS" ]; then
  bash @GATE@
fi
echo "BUILD_RAN"
'''

def workflow_build_step() -> str:
    """Return the real Build step with only expensive commands stubbed."""
    doc = yaml.safe_load(BUILD_WORKFLOW.read_text(encoding="utf-8"))
    step = next(
        value
        for value in doc["jobs"]["build"]["steps"]
        if value.get("name") == "Build"
    )
    body = step["run"]
    gate_call = (
        '    bash test/cmake/test_ios_compile_gate.sh \\\n'
        '      "$GITHUB_WORKSPACE" "$PULP_BUILD_DIR-ios"'
    )
    if body.count(gate_call) != 1:
        raise AssertionError("expected exactly one iOS compile-gate call")
    body = body.replace(gate_call, "bash @GATE@")
    body, build_count = re.subn(
        r'(?m)^\s*cmake --build "\$PULP_BUILD_DIR".*$',
        'echo "BUILD_RAN"',
        body,
    )
    body, cleanup_count = re.subn(
        r'(?m)^\s*rm -f "\$PULP_BUILD_DIR/\.pulp-build-incomplete"$',
        ':',
        body,
    )
    if (build_count, cleanup_count) != (1, 1):
        raise AssertionError("Build step shape changed; update the focused stubs")
    return body


class TestSkipIsNotAFailure(unittest.TestCase):
    def test_bare_call_turns_a_skip_into_a_red_required_check(self):
        """The bug, stated as a test: this is what the workflow must not do."""
        rc, out = run_step(BARE_CALL, 77)
        self.assertEqual(rc, 77, "a bare call propagates the skip as failure")
        self.assertNotIn("BUILD_RAN", out,
                         "-e also aborts before the build the step exists to run")

    def test_guarded_call_treats_77_as_skip_and_still_builds(self):
        rc, out = run_step(workflow_build_step(), 77)
        self.assertEqual(rc, 0)
        self.assertIn("BUILD_RAN", out)

    def test_guarded_call_still_fails_on_a_real_gate_failure(self):
        """The control. Without this, 'tolerate 77' could become 'tolerate
        everything', which would silently retire the gate instead of fixing it."""
        for bad in (1, 2):
            with self.subTest(gate_exit=bad):
                rc, out = run_step(workflow_build_step(), bad)
                self.assertEqual(rc, bad, "a real gate failure must still red the check")
                self.assertNotIn("BUILD_RAN", out)

    def test_guarded_call_passes_a_successful_gate_through(self):
        rc, out = run_step(workflow_build_step(), 0)
        self.assertEqual(rc, 0)
        self.assertIn("BUILD_RAN", out)

    def test_exact_changed_surface_false_skips_gate_and_still_builds(self):
        rc, out = run_step(
            workflow_build_step(), 2, ios_compile_required="false"
        )
        self.assertEqual(rc, 0)
        self.assertNotIn("stub gate exiting", out)
        self.assertIn("BUILD_RAN", out)

    def test_missing_or_malformed_authorization_runs_gate(self):
        for value in ("", "False", "0", "malformed"):
            with self.subTest(value=value):
                rc, out = run_step(
                    workflow_build_step(), 2, ios_compile_required=value
                )
                self.assertEqual(rc, 2)
                self.assertIn("stub gate exiting 2", out)
                self.assertNotIn("BUILD_RAN", out)


class TestWorkflowUsesTheGuardedForm(unittest.TestCase):
    def test_build_workflow_guards_the_ios_gate_exit(self):
        text = BUILD_WORKFLOW.read_text(encoding="utf-8")
        self.assertIn("test_ios_compile_gate.sh", text)
        self.assertRegex(
            text, r"ios_gate_rc=\$\?",
            "build.yml must capture the gate's status instead of letting -e take it")
        self.assertRegex(
            text, r'\[ "\$ios_gate_rc" = 77 \]',
            "build.yml must treat 77 as skip")

    def test_gate_still_uses_77_for_its_skips(self):
        """If the gate ever stops using 77, this guard is protecting nothing."""
        text = GATE.read_text(encoding="utf-8")
        self.assertRegex(text, r"SKIP:.*\n\s*exit 77",
                         "the gate's skip convention changed; revisit build.yml")


if __name__ == "__main__":
    unittest.main(verbosity=2)

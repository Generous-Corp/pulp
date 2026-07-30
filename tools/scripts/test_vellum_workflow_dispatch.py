#!/usr/bin/env python3
"""The Vellum gates must resolve the same PR refs however they were triggered.

Both gates post or gate a REQUIRED check, and both grew a `workflow_dispatch`
path so a wedged or cancelled run can be re-run without pushing a commit. That
path shares no code with the event path except by construction, and lives inside
embedded YAML that no other test executes.

So this extracts the resolve step from each workflow and runs it, with `gh` and
`git` faked. Re-implementing the branch here would pass forever after the
workflow stopped doing it.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS = REPO_ROOT / ".github" / "workflows"

OPEN_PR = '{"state":"open","base":{"sha":"BASE_OPEN"},"head":{"sha":"HEAD_OPEN"}}'
CLOSED_PR = '{"state":"closed","base":{"sha":"BASE_SHUT"},"head":{"sha":"HEAD_SHUT"}}'

FAKE_GH = textwrap.dedent(
    """\
    #!/bin/sh
    body=''
    case "$*" in
      *pulls/111*) body='%s' ;;
      *pulls/222*) body='%s' ;;
    esac
    prev=''
    for a in "$@"; do
      if [ "$prev" = "--jq" ]; then printf '%%s' "$body" | jq -r "$a"; exit 0; fi
      prev="$a"
    done
    printf '%%s\\n' "$body"
    """
) % (OPEN_PR, CLOSED_PR)

# The dispatch path fetches the base and reads the already-checked-out merge
# commit; neither should reach the network in a test.
FAKE_GIT = textwrap.dedent(
    """\
    #!/bin/sh
    case "$1" in
      rev-parse) echo MERGE_COMMIT ;;
      *) exit 0 ;;
    esac
    """
)


def _step_script(workflow: str, job: str, step_id: str) -> str:
    try:
        import yaml
    except ImportError:  # pragma: no cover - environment-dependent
        raise unittest.SkipTest("PyYAML not installed")
    doc = yaml.safe_load((WORKFLOWS / workflow).read_text())
    steps = doc["jobs"][job]["steps"]
    step = next((s for s in steps if s.get("id") == step_id), None)
    if step is None:
        raise AssertionError(
            f"{workflow}: no step id={step_id!r} in job {job!r} — the dispatch "
            "plumbing was renamed or removed"
        )
    return step["run"]


def _run(script: str, env: dict[str, str]) -> tuple[int, dict[str, str], str]:
    with tempfile.TemporaryDirectory() as tmp:
        binp = Path(tmp) / "bin"
        binp.mkdir()
        (binp / "gh").write_text(FAKE_GH)
        (binp / "git").write_text(FAKE_GIT)
        for tool in ("gh", "git"):
            (binp / tool).chmod(0o755)

        body = Path(tmp) / "step.sh"
        body.write_text(script)
        out = Path(tmp) / "github_output"
        out.write_text("")

        # Drop the ambient GITHUB_* namespace before building the fake one.
        # These tests run INSIDE Actions, where every one of these is already
        # set to the real run's values — so a `setdefault` silently keeps the
        # runner's value and the case under test never happens. That is not
        # hypothetical: GITHUB_SHA defaulted to "EVENT_MERGE" locally and to the
        # real head SHA on CI, so `test_pull_request_path_unchanged` passed on a
        # laptop and failed in CI on the same commit. Scrubbing the prefix
        # rather than overwriting the four names read today keeps a step that
        # starts reading a fifth from inheriting the runner's.
        full = {k: v for k, v in os.environ.items() if not k.startswith("GITHUB_")}
        full["PATH"] = f"{binp}:{os.environ['PATH']}"
        full["GITHUB_OUTPUT"] = str(out)
        full["GITHUB_REPOSITORY"] = "Generous-Corp/pulp"
        full["GH_TOKEN"] = "x"
        full["GITHUB_SHA"] = "EVENT_MERGE"
        for key in ("DISPATCH_PR", "PR_BASE", "PR_SOURCE_HEAD", "MERGE_BASE",
                    "MERGE_HEAD", "EVENT_BASE", "EVENT_HEAD", "EVENT_NUMBER"):
            full[key] = ""
        full.update(env)

        proc = subprocess.run(["bash", str(body)], env=full, cwd=tmp,
                              capture_output=True, text=True)
        parsed = dict(
            line.split("=", 1)
            for line in out.read_text().splitlines()
            if "=" in line
        )
        return proc.returncode, parsed, proc.stderr


class TrustedGateResolve(unittest.TestCase):
    def script(self) -> str:
        return _step_script("vellum-trusted-gate.yml", "trusted-gate", "pr")

    def test_event_path_uses_the_payload_and_no_api(self):
        rc, out, _ = _run(self.script(), {
            "GITHUB_EVENT_NAME": "pull_request_target",
            "EVENT_BASE": "B", "EVENT_HEAD": "H", "EVENT_NUMBER": "7",
        })
        self.assertEqual(rc, 0)
        self.assertEqual(out, {"base_sha": "B", "head_sha": "H", "number": "7"})

    def test_dispatch_path_resolves_from_the_api(self):
        rc, out, _ = _run(self.script(), {
            "GITHUB_EVENT_NAME": "workflow_dispatch", "DISPATCH_PR": "111",
        })
        self.assertEqual(rc, 0)
        self.assertEqual(out["base_sha"], "BASE_OPEN")
        self.assertEqual(out["head_sha"], "HEAD_OPEN")
        self.assertEqual(out["number"], "111")

    def test_dispatch_refuses_a_closed_pr(self):
        # This job posts a commit status. Doing that for a merged PR would put a
        # fresh pending/failure row on history nobody can act on.
        rc, out, err = _run(self.script(), {
            "GITHUB_EVENT_NAME": "workflow_dispatch", "DISPATCH_PR": "222",
        })
        self.assertEqual(rc, 1)
        self.assertEqual(out, {})
        self.assertIn("closed", err)


class FreezeCheckResolve(unittest.TestCase):
    def script(self) -> str:
        return _step_script("vellum-freeze-check.yml", "freeze-check", "comparison")

    def test_pull_request_path_unchanged(self):
        rc, out, _ = _run(self.script(), {
            "GITHUB_EVENT_NAME": "pull_request",
            "PR_BASE": "B", "PR_SOURCE_HEAD": "SH",
        })
        self.assertEqual(rc, 0)
        self.assertEqual(out, {"base": "B", "head": "EVENT_MERGE", "source_head": "SH"})

    def test_merge_group_path_unchanged(self):
        rc, out, _ = _run(self.script(), {
            "GITHUB_EVENT_NAME": "merge_group",
            "MERGE_BASE": "MB", "MERGE_HEAD": "MH",
        })
        self.assertEqual(rc, 0)
        self.assertEqual(out, {"base": "MB", "head": "MH", "source_head": "MH"})

    def test_dispatch_path_resolves_from_the_api(self):
        rc, out, _ = _run(self.script(), {
            "GITHUB_EVENT_NAME": "workflow_dispatch", "DISPATCH_PR": "111",
        })
        self.assertEqual(rc, 0)
        self.assertEqual(out["base"], "BASE_OPEN")
        self.assertEqual(out["source_head"], "HEAD_OPEN")
        # head is the checked-out merge commit, matching what the pull_request
        # path gets from GITHUB_SHA.
        self.assertEqual(out["head"], "MERGE_COMMIT")

    def test_dispatch_refuses_a_closed_pr(self):
        rc, _, err = _run(self.script(), {
            "GITHUB_EVENT_NAME": "workflow_dispatch", "DISPATCH_PR": "222",
        })
        self.assertEqual(rc, 1)
        self.assertIn("closed", err)


class DispatchIsDeclared(unittest.TestCase):
    def test_both_required_gates_can_be_rerun_by_hand(self):
        try:
            import yaml
        except ImportError:  # pragma: no cover
            raise unittest.SkipTest("PyYAML not installed")
        for name in ("vellum-trusted-gate.yml", "vellum-freeze-check.yml"):
            doc = yaml.safe_load((WORKFLOWS / name).read_text())
            on = doc.get(True) or doc.get("on")
            self.assertIn(
                "workflow_dispatch", on,
                f"{name} posts a required check; without workflow_dispatch a "
                "wedged run can only be recovered by pushing a commit",
            )
            self.assertIn("pr_number", on["workflow_dispatch"]["inputs"])


if __name__ == "__main__":
    unittest.main(verbosity=2)

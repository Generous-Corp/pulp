#!/usr/bin/env python3
"""build.yml's `pr_number` dispatch tests the pull request's merge ref.

The fleet App moves an approved head to the full lane by dispatching
build.yml. Without `pr_number` a dispatch tests the bare branch head, so a
head that is behind main fails freshness checks, and its Linux leg asks for an
operator runner nothing serves. With it the run binds the dispatch to the pull
request's head, checks out refs/pull/<n>/merge, records the ref and merge sha
it tested, and routes Linux as a pull_request run does. Without it nothing
changes. Runs the workflow's own bash and resolver, against a fake `gh` and a
throwaway git repository.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / ".github/workflows/build.yml"
REPO = "Generous-Corp/pulp"
MERGE_REF = "${{ github.event_name == 'workflow_dispatch' && inputs.pr_number != '' && format('refs/pull/{0}/merge', inputs.pr_number) || '' }}"
PR_DISPATCH = "github.event_name == 'workflow_dispatch' && inputs.pr_number != ''"
LOCAL_LINUX = ["self-hosted", "Linux", "X64", "pulp-build-linux-x64", "pulp-host-macpro"]

FAKE_GH = """#!/usr/bin/env python3
import os, sys
answer = os.environ["FAKE_GH_ANSWER"]
if answer == "!FAIL":
    sys.stderr.write("HTTP 404: Not Found\\n")
    sys.exit(1)
sys.stdout.write(answer)
"""


def workflow() -> dict:
    return yaml.safe_load(BUILD.read_text(encoding="utf-8"))


def steps(job: str) -> dict[str, dict]:
    return {step.get("name") or step.get("id") or step.get("uses"): step
            for step in workflow()["jobs"][job]["steps"]}


def bash(script: str, env: dict[str, str]) -> tuple[subprocess.CompletedProcess, dict[str, str]]:
    with tempfile.TemporaryDirectory() as tmp:
        output, summary = Path(tmp) / "output", Path(tmp) / "summary"
        output.write_text("", encoding="utf-8")
        summary.write_text("", encoding="utf-8")
        proc = subprocess.run(["bash", "-c", script],
                              env={**os.environ, "GITHUB_OUTPUT": str(output),
                                   "GITHUB_STEP_SUMMARY": str(summary), **env},
                              cwd=env.get("PWD_OVERRIDE", tmp), capture_output=True,
                              text=True, encoding="utf-8", check=False)
        outputs = dict(line.split("=", 1) for line in output.read_text(encoding="utf-8").splitlines())
        outputs["<summary>"] = summary.read_text(encoding="utf-8")
        return proc, outputs


class InputAndCheckoutTest(unittest.TestCase):
    def test_the_input_is_an_optional_string(self) -> None:
        triggers = workflow().get("on", workflow().get(True, {}))
        pr_number = triggers["workflow_dispatch"]["inputs"]["pr_number"]
        self.assertEqual((pr_number["type"], pr_number["default"], pr_number["required"]),
                         ("string", "", False))

    def test_classify_and_build_check_out_the_merge_ref_only_on_a_pr_dispatch(self) -> None:
        for job in ("classify", "build"):
            with self.subTest(job=job):
                checkout = next(step for step in workflow()["jobs"][job]["steps"]
                                if str(step.get("uses", "")).startswith("actions/checkout"))
                self.assertEqual(checkout["with"]["ref"], MERGE_REF)

    def test_the_ref_expression_is_empty_off_the_pr_dispatch(self) -> None:
        # Mirrors GitHub's && / || short-circuit: empty means the event's ref.
        def ref(event: str, pr_number: str) -> str:
            return (event == "workflow_dispatch" and pr_number != ""
                    and f"refs/pull/{pr_number}/merge") or ""
        self.assertEqual(ref("workflow_dispatch", "11040"), "refs/pull/11040/merge")
        for event, pr_number in (("workflow_dispatch", ""), ("pull_request", ""),
                                 ("merge_group", ""), ("push", "")):
            with self.subTest(event=event):
                self.assertEqual(ref(event, pr_number), "")

    def test_the_tested_ref_is_verified_and_recorded_before_anything_uses_the_tree(self) -> None:
        names = [step.get("name") for step in workflow()["jobs"]["build"]["steps"]]
        checkout = next(i for i, step in enumerate(workflow()["jobs"]["build"]["steps"])
                        if str(step.get("uses", "")).startswith("actions/checkout"))
        self.assertEqual(names[checkout + 1], "Verify the tested merge ref")
        self.assertTrue(names[checkout + 2].startswith("Tested ${{ steps.tested.outputs.tested_ref }}"))
        self.assertEqual(names[checkout + 3], "Fetch protected capability base")
        verify = steps("build")["Verify the tested merge ref"]
        self.assertEqual(verify["if"], PR_DISPATCH)
        self.assertEqual(
            steps("build")["Fetch protected capability base"]["env"]["PULP_CAPABILITY_PR_BASE_SHA"],
            "${{ github.event.pull_request.base.sha || steps.tested.outputs.merged_base }}")


class VerifyMergeRefTest(unittest.TestCase):
    """The verify step's own bash, on a real merge commit."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.script = steps("build")["Verify the tested merge ref"]["run"]

    def _repo(self, tmp: Path) -> dict[str, str]:
        def git(*args: str) -> str:
            return subprocess.run(["git", "-C", str(tmp), *args], check=True, capture_output=True,
                                  text=True, encoding="utf-8",
                                  env={**os.environ, "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t",
                                       "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@t"}).stdout.strip()
        git("init", "-q", "-b", "main")
        git("commit", "-q", "--allow-empty", "-m", "base")
        git("checkout", "-q", "-b", "topic")
        git("commit", "-q", "--allow-empty", "-m", "head")
        head = git("rev-parse", "HEAD")
        git("checkout", "-q", "main")
        git("commit", "-q", "--allow-empty", "-m", "main moved")
        base = git("rev-parse", "HEAD")
        git("merge", "-q", "--no-ff", "-m", "merge", "topic")
        return {"head": head, "base": base, "merge": git("rev-parse", "HEAD")}

    def test_a_merge_of_this_head_is_recorded(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            shas = self._repo(Path(tmp))
            proc, outputs = bash(self.script, {"GITHUB_SHA": shas["head"],
                                               "TESTED_REF": "refs/pull/7/merge", "PWD_OVERRIDE": tmp})
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual((outputs["tested_ref"], outputs["tested_sha"], outputs["merged_base"]),
                         ("refs/pull/7/merge", shas["merge"], shas["base"]))
        self.assertIn(f"| tested_sha | `{shas['merge']}` |", outputs["<summary>"])

    def test_a_merge_of_another_head_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            shas = self._repo(Path(tmp))
            proc, outputs = bash(self.script, {"GITHUB_SHA": "f" * 40,
                                               "TESTED_REF": "refs/pull/7/merge", "PWD_OVERRIDE": tmp})
        self.assertEqual(proc.returncode, 1)
        self.assertIn("does not merge this run's head", proc.stdout)
        self.assertNotIn("tested_sha", outputs)


class BindingTest(unittest.TestCase):
    """The resolve-provider step that binds the dispatch to the pull request."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.step = steps("resolve-provider")["Bind the pull-request dispatch to its head"]

    def _run(self, pr_number: str, answer: str) -> tuple[subprocess.CompletedProcess, dict[str, str]]:
        with tempfile.TemporaryDirectory() as tmp:
            bin_dir = Path(tmp) / "bin"
            bin_dir.mkdir()
            (bin_dir / "gh").write_text(FAKE_GH, encoding="utf-8")
            (bin_dir / "gh").chmod(0o755)
            return bash(self.step["run"], {
                "PATH": f"{bin_dir}{os.pathsep}{os.environ['PATH']}", "FAKE_GH_ANSWER": answer,
                "DISPATCH_PR_NUMBER": pr_number, "GITHUB_REPOSITORY": REPO,
                "GITHUB_REF_NAME": "topic", "GITHUB_SHA": "a" * 40, "GH_TOKEN": "x"})

    def test_it_runs_only_on_a_pr_dispatch(self) -> None:
        self.assertEqual(self.step["if"], PR_DISPATCH)

    def test_a_dispatch_on_the_pull_requests_head_is_bound(self) -> None:
        proc, outputs = self._run("7", f"open {REPO} topic {'a' * 40}\n")
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(outputs["tested_ref"], "refs/pull/7/merge")

    def test_anything_else_fails_closed(self) -> None:
        for case, pr_number, answer in (
                ("closed", "7", f"closed {REPO} topic {'a' * 40}\n"),
                ("fork head", "7", f"open someone/pulp topic {'a' * 40}\n"),
                ("another branch", "7", f"open {REPO} other {'a' * 40}\n"),
                ("head moved", "7", f"open {REPO} topic {'b' * 40}\n"),
                ("unreadable", "7", "!FAIL"),
                ("not digits", "7; rm -rf /", f"open {REPO} topic {'a' * 40}\n"),
                ("zero", "0", f"open {REPO} topic {'a' * 40}\n")):
            with self.subTest(case=case):
                proc, outputs = self._run(pr_number, answer)
                self.assertEqual(proc.returncode, 1, proc.stdout)
                self.assertNotIn("tested_ref", outputs)


class LinuxRoutingTest(unittest.TestCase):
    """The real resolver: a PR dispatch routes Linux as a pull_request run."""

    @classmethod
    def setUpClass(cls) -> None:
        for step in workflow()["jobs"]["resolve-provider"]["steps"]:
            body = step.get("run", "")
            if "matrix_json=" in body:
                cls.resolver = textwrap.dedent(body.split("python3 - <<'PY'", 1)[1].rsplit("PY", 1)[0])
                cls.env = step["env"]
                return
        raise AssertionError("resolve-provider matrix step not found")

    def _linux(self, event: str, *, pr_number: str = "", explicit: str = "",
               dispatch_selector: str = "") -> list | str:
        env = {k: v for k, v in os.environ.items()
               if not k.startswith(("GITHUB_", "PULP_", "EXPLICIT_", "NAMESPACE_"))}
        env.update({
            "REQUESTED_PROVIDER": "github-hosted", "GITHUB_EVENT_NAME": event,
            "GITHUB_REPOSITORY": REPO, "GITHUB_WORKSPACE": str(ROOT),
            "WORKFLOW_REF": f"{REPO}/.github/workflows/build.yml@refs/heads/topic",
            "EXPLICIT_LINUX_RUNNER_SELECTOR_JSON": explicit,
            "EXPLICIT_WINDOWS_RUNNER_SELECTOR_JSON": "",
            "CONFIGURED_LINUX_RUNNER_SELECTOR_JSON": json.dumps(LOCAL_LINUX),
            "WORKFLOW_DISPATCH_LINUX_SELECTOR": dispatch_selector,
            "DISPATCH_PR_NUMBER": pr_number, "WORKFLOW_DISPATCH_RUN_WINDOWS": "true",
            "WORKFLOW_DISPATCH_MACOS_SELECTOR": "",
            "LOCAL_MACOS_RUNS_ON_JSON": json.dumps(["self-hosted", "macOS", "ARM64", "pulp-build"]),
            # Keeps macOS local, so the resolver never probes runners live.
            "OVERFLOW_MACOS_RUNS_ON_JSON": "local-only",
            "NAMESPACE_LINUX_RUNS_ON_JSON": "", "NAMESPACE_WINDOWS_RUNS_ON_JSON": "",
            "WORKFLOW_DISPATCH_ALLOW_HOSTED_WINDOWS": "", "ALLOW_HOSTED_WINDOWS_VAR": "",
        })
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "out"
            output.write_text("", encoding="utf-8")
            env["GITHUB_OUTPUT"] = str(output)
            proc = subprocess.run([sys.executable, "-c", self.resolver], cwd=ROOT, env=env,
                                  capture_output=True, text=True, encoding="utf-8", check=False)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            for line in output.read_text(encoding="utf-8").splitlines():
                if line.startswith("matrix_json="):
                    legs = json.loads(line.split("=", 1)[1])["include"]
                    linux = next(leg for leg in legs if leg["key"] == "linux")
                    return json.loads(linux["runs_on_json"])
        raise AssertionError("resolver emitted no matrix_json")

    def test_a_pr_dispatch_routes_linux_to_hosted(self) -> None:
        # The workflow's own expressions blank both operator selectors here.
        self.assertEqual(self._linux("workflow_dispatch", pr_number="11040"), "ubuntu-latest")

    def test_a_plain_dispatch_keeps_todays_operator_route(self) -> None:
        # Control: today's dispatch, with the operator selector its
        # expression supplies, still routes to the local runner.
        self.assertEqual(self._linux("workflow_dispatch", explicit=json.dumps(LOCAL_LINUX)),
                         LOCAL_LINUX)

    def test_the_operator_selectors_are_blanked_on_a_pr_dispatch(self) -> None:
        self.assertEqual(self.env["EXPLICIT_LINUX_RUNNER_SELECTOR_JSON"],
                         "${{ inputs.pr_number == '' && (inputs.linux_runner_selector_json || "
                         "(github.event_name == 'workflow_dispatch' && vars.PULP_LOCAL_LINUX_RUNS_ON_JSON)) || '' }}")
        self.assertEqual(self.env["WORKFLOW_DISPATCH_LINUX_SELECTOR"],
                         "${{ inputs.pr_number == '' && inputs.linux_runner_selector_json || '' }}")
        self.assertEqual(self.env["DISPATCH_PR_NUMBER"], "${{ inputs.pr_number }}")


if __name__ == "__main__":
    unittest.main()

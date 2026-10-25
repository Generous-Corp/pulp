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
CLASSIFY_REF = "${{ steps.pr_dispatch.outputs.tested_sha || '' }}"
BUILD_REF = "${{ needs.classify.outputs.tested_sha || '' }}"
PR_DISPATCH = "github.event_name == 'workflow_dispatch' && inputs.pr_number != ''"
LOCAL_LINUX = ["self-hosted", "Linux", "X64", "pulp-build-linux-x64", "pulp-host-macpro"]

# Answers `gh api <path>` from FAKE_GH_ANSWERS ({path-substring: body}); a
# body of "!FAIL" exits non-zero.
FAKE_GH = """#!/usr/bin/env python3
import json, os, sys
answers = json.loads(os.environ["FAKE_GH_ANSWERS"])
text = " ".join(sys.argv[1:])
for key, body in answers.items():
    if key in text:
        if body == "!FAIL":
            sys.stderr.write("HTTP 404: Not Found\\n")
            sys.exit(1)
        sys.stdout.write(body)
        break
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


def merge_repo(tmp: Path) -> dict[str, str]:
    """A repository whose HEAD merges `topic` onto a `main` that moved."""
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


class InputAndCheckoutTest(unittest.TestCase):
    def test_the_input_is_an_optional_string(self) -> None:
        triggers = workflow().get("on", workflow().get(True, {}))
        pr_number = triggers["workflow_dispatch"]["inputs"]["pr_number"]
        self.assertEqual((pr_number["type"], pr_number["default"], pr_number["required"]),
                         ("string", "", False))

    def test_both_jobs_check_out_the_one_bound_merge_commit(self) -> None:
        # Resolved once, in classify's bind step; build checks out classify's
        # output. Neither checks out the moving refs/pull/<n>/merge.
        jobs = workflow()["jobs"]
        checkouts = {job: next(step for step in jobs[job]["steps"]
                               if str(step.get("uses", "")).startswith("actions/checkout"))
                     for job in ("classify", "build")}
        self.assertEqual(checkouts["classify"]["with"]["ref"], CLASSIFY_REF)
        self.assertEqual(checkouts["build"]["with"]["ref"], BUILD_REF)
        outputs = jobs["classify"]["outputs"]
        for name in ("tested_ref", "tested_sha", "merged_base"):
            self.assertEqual(outputs[name], f"${{{{ steps.pr_dispatch.outputs.{name} }}}}")
        self.assertIn("classify", jobs["build"]["needs"])
        self.assertNotIn("refs/pull/{0}/merge", BUILD.read_text(encoding="utf-8"))

    def test_the_bind_runs_before_classify_checks_anything_out(self) -> None:
        first = workflow()["jobs"]["classify"]["steps"][0]
        self.assertEqual((first["name"], first["id"]), ("Bind the pull-request dispatch to its head",
                                                       "pr_dispatch"))
        self.assertEqual(first["if"], PR_DISPATCH)
        self.assertNotIn("Bind the pull-request dispatch to its head", steps("resolve-provider"))

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

    def test_a_merge_of_this_head_is_recorded(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            shas = merge_repo(Path(tmp))
            proc, outputs = bash(self.script, {"GITHUB_SHA": shas["head"], "BOUND_SHA": shas["merge"],
                                               "TESTED_REF": "refs/pull/7/merge", "PWD_OVERRIDE": tmp})
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual((outputs["tested_ref"], outputs["tested_sha"], outputs["merged_base"]),
                         ("refs/pull/7/merge", shas["merge"], shas["base"]))
        self.assertIn(f"| tested_sha | `{shas['merge']}` |", outputs["<summary>"])

    def test_a_merge_of_another_head_fails(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            shas = merge_repo(Path(tmp))
            proc, outputs = bash(self.script, {"GITHUB_SHA": "f" * 40, "BOUND_SHA": shas["merge"],
                                               "TESTED_REF": "refs/pull/7/merge", "PWD_OVERRIDE": tmp})
        self.assertEqual(proc.returncode, 1)
        self.assertIn("does not merge this run's head", proc.stdout)
        self.assertNotIn("tested_sha", outputs)

    def test_a_checkout_other_than_the_bound_sha_fails(self) -> None:
        # Belt and braces: build re-checks classify's binding.
        with tempfile.TemporaryDirectory() as tmp:
            shas = merge_repo(Path(tmp))
            for bound in ("e" * 40, ""):
                with self.subTest(bound=bound):
                    proc, outputs = bash(self.script, {"GITHUB_SHA": shas["head"], "BOUND_SHA": bound,
                                                       "TESTED_REF": "refs/pull/7/merge",
                                                       "PWD_OVERRIDE": tmp})
                    self.assertEqual(proc.returncode, 1)
                    self.assertIn("not the merge commit classify bound", proc.stdout)
                    self.assertNotIn("tested_sha", outputs)

    def test_it_reads_the_binding_from_classify(self) -> None:
        env = steps("build")["Verify the tested merge ref"]["env"]
        self.assertEqual(env["BOUND_SHA"], "${{ needs.classify.outputs.tested_sha }}")
        self.assertEqual(env["TESTED_REF"], "${{ needs.classify.outputs.tested_ref }}")


class ClassifyBaseTest(unittest.TestCase):
    """classify diffs a PR dispatch's merge commit against its bound base."""

    START = 'if [ "$EVENT_NAME" = workflow_dispatch ] && [ -n "${DISPATCH_PR_NUMBER:-}" ]; then'

    @classmethod
    def setUpClass(cls) -> None:
        step = next(s for s in workflow()["jobs"]["classify"]["steps"] if s.get("id") == "classify")
        cls.env = step["env"]
        lines = step["run"].splitlines()
        start = next(i for i, line in enumerate(lines) if line.strip() == cls.START)
        end = next(i for i in range(start, len(lines)) if lines[i].strip() == "fi")
        cls.snippet = "\n".join(lines[start:end + 1])

    def _base(self, event: str, pr_number: str, merged_base: str) -> str:
        script = f"set -euo pipefail\nbase=origin/main\n{self.snippet}\necho \"BASE=$base\""
        proc, _ = bash(script, {"EVENT_NAME": event, "DISPATCH_PR_NUMBER": pr_number,
                                "DISPATCH_MERGED_BASE": merged_base})
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        return next(line for line in proc.stdout.splitlines() if line.startswith("BASE="))[5:]

    def test_a_pr_dispatch_diffs_against_the_bound_base(self) -> None:
        self.assertEqual(self.env["DISPATCH_MERGED_BASE"], "${{ steps.pr_dispatch.outputs.merged_base }}")
        self.assertEqual(self._base("workflow_dispatch", "7", "c" * 40), "c" * 40)
        # Not a PR dispatch: the resolver's base stands.
        self.assertEqual(self._base("workflow_dispatch", "", ""), "origin/main")
        self.assertEqual(self._base("pull_request", "7", "c" * 40), "origin/main")
        # No bound base: no sha, so the caller requires the native build.
        self.assertEqual(self._base("workflow_dispatch", "7", ""), "")


class BindingTest(unittest.TestCase):
    """classify's bind step: the pull request, its head and its merge commit."""

    HEAD, MERGE, BASE = "a" * 40, "d" * 40, "c" * 40

    @classmethod
    def setUpClass(cls) -> None:
        cls.step = steps("classify")["Bind the pull-request dispatch to its head"]

    def _pull(self, state="open", repo=REPO, ref="topic", head=None, merge=None, mergeable="true"):
        merge = self.MERGE if merge is None else merge
        return f"{state} {repo} {ref} {head or self.HEAD} {merge} {mergeable}\n"

    def _run(self, pull: str, parents: str | None = None, pr_number: str = "7"):
        answers = {"/pulls/": pull,
                   "/commits/": parents if parents is not None else f"{self.BASE} {self.HEAD}\n"}
        with tempfile.TemporaryDirectory() as tmp:
            bin_dir = Path(tmp) / "bin"
            bin_dir.mkdir()
            (bin_dir / "gh").write_text(FAKE_GH, encoding="utf-8")
            (bin_dir / "gh").chmod(0o755)
            # The wait for GitHub's mergeability is real time; tests skip it.
            # Actions substitutes the token expression before bash runs.
            script = (self.step["run"].replace("sleep $((attempt * 5))", ":")
                      .replace("${{ github.token }}", "x"))
            return bash(script, {
                "PATH": f"{bin_dir}{os.pathsep}{os.environ['PATH']}",
                "FAKE_GH_ANSWERS": json.dumps(answers), "DISPATCH_PR_NUMBER": pr_number,
                "GITHUB_REPOSITORY": REPO, "GITHUB_REF_NAME": "topic", "GITHUB_SHA": self.HEAD,
                "GH_TOKEN": "x"})

    def test_it_runs_only_on_a_pr_dispatch(self) -> None:
        self.assertEqual(self.step["if"], PR_DISPATCH)
        # The token reaches its reads only, never the job's env.
        self.assertNotIn("GH_TOKEN", self.step.get("env", {}))

    def test_a_dispatch_on_the_pull_requests_head_binds_its_merge_commit(self) -> None:
        proc, outputs = self._run(self._pull())
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual((outputs["tested_ref"], outputs["tested_sha"], outputs["merged_base"]),
                         ("refs/pull/7/merge", self.MERGE, self.BASE))

    def test_anything_else_fails_closed(self) -> None:
        cases = (
            ("closed", self._pull(state="closed"), None, "7"),
            ("fork head", self._pull(repo="someone/pulp"), None, "7"),
            ("another branch", self._pull(ref="other"), None, "7"),
            ("head moved", self._pull(head="b" * 40), None, "7"),
            ("unreadable", "!FAIL", None, "7"),
            ("not digits", self._pull(), None, "7; rm -rf /"),
            ("zero", self._pull(), None, "0"),
            ("conflicting", self._pull(mergeable="false"), None, "7"),
            ("never computed", self._pull(mergeable="null"), None, "7"),
            ("no merge commit", self._pull(merge=""), None, "7"),
            ("merges another head", self._pull(), f"{self.BASE} {'b' * 40}\n", "7"),
            ("head as first parent", self._pull(), f"{self.HEAD} {self.BASE}\n", "7"),
            ("three parents", self._pull(), f"{self.BASE} {self.HEAD} {'e' * 40}\n", "7"),
            ("one parent", self._pull(), f"{self.HEAD}\n", "7"),
            ("unreadable merge commit", self._pull(), "!FAIL", "7"),
        )
        for case, pull, parents, pr_number in cases:
            with self.subTest(case=case):
                proc, outputs = self._run(pull, parents, pr_number)
                self.assertEqual(proc.returncode, 1, proc.stdout)
                self.assertNotIn("tested_sha", outputs)


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

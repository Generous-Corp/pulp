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

    def _run(self, pull: str, parents: str | None = None, pr_number: str = "7",
             ref_name: str = "topic", default_branch: str = "main",
             protected: str = "false\n", rules: str = "0\n"):
        # First match wins, so the rules path goes before the branch path.
        answers = {"/rules/branches/": rules, "/branches/": protected, "/pulls/": pull,
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
                "GITHUB_REPOSITORY": REPO, "GITHUB_REF_NAME": ref_name, "GITHUB_SHA": self.HEAD,
                "DEFAULT_BRANCH": default_branch, "GH_TOKEN": "x"})

    def test_it_runs_only_on_a_pr_dispatch(self) -> None:
        self.assertEqual(self.step["if"], PR_DISPATCH)
        # The token reaches its reads only, never the job's env.
        self.assertNotIn("GH_TOKEN", self.step.get("env", {}))

    def test_a_dispatch_on_the_pull_requests_head_binds_its_merge_commit(self) -> None:
        proc, outputs = self._run(self._pull())
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual((outputs["tested_ref"], outputs["tested_sha"], outputs["merged_base"]),
                         ("refs/pull/7/merge", self.MERGE, self.BASE))

    def test_a_protected_or_release_ref_is_refused(self) -> None:
        for case, kwargs in (
                ("release branch", {"ref_name": "release/1.2"}),
                ("classic protection", {"protected": "true\n"}),
                ("a ruleset applies", {"rules": "2\n"}),
                ("protection unreadable", {"protected": "!FAIL"}),
                ("rules unreadable", {"rules": "!FAIL"})):
            with self.subTest(case=case):
                ref_name = kwargs.get("ref_name", "topic")
                proc, outputs = self._run(self._pull(ref=ref_name), **kwargs)
                self.assertEqual(proc.returncode, 1, proc.stdout)
                self.assertNotIn("tested_sha", outputs)

    def test_a_dispatch_on_the_default_branch_is_refused(self) -> None:
        # Its caches would land in the default branch's scope. Even a pull
        # request headed by main itself (into develop/**) is refused.
        for ref_name, default_branch in (("main", "main"), ("topic", "")):
            with self.subTest(ref_name=ref_name, default_branch=default_branch):
                proc, outputs = self._run(self._pull(ref=ref_name), ref_name=ref_name,
                                          default_branch=default_branch)
                self.assertEqual(proc.returncode, 1, proc.stdout)
                self.assertNotIn("tested_sha", outputs)
        self.assertEqual(self.step["env"]["DEFAULT_BRANCH"],
                         "${{ github.event.repository.default_branch }}")

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



class NoCacheOrArtifactWriteTest(unittest.TestCase):
    """A pr_number dispatch runs a pull request's code: it writes nothing a
    later run reads. Its caches only restore, and its consumable artifacts are
    not uploaded."""

    NO_PR = "inputs.pr_number == ''"
    # The SDK upload's retry runs only when that upload failed, so it is
    # skipped with it.
    SKIPPED_UPLOADS = ("Record iOS gate digest", "Upload exact GPU-audio SDK (macOS ARM64)",
                       "Upload reuse replay record (macOS)", "Upload plugin bundles (macOS)",
                       "Upload plugin bundles (Windows)", "Upload plugin bundles (Linux)")

    @classmethod
    def setUpClass(cls) -> None:
        cls.jobs = workflow()["jobs"]

    def _condition(self, step: dict) -> str:
        return " ".join(str(step.get("if", "")).split())

    def test_no_cache_step_can_save_on_the_pr_dispatch_path(self) -> None:
        seen = 0
        for job in ("classify", "build"):
            for step in self.jobs[job]["steps"]:
                uses = str(step.get("uses", ""))
                with self.subTest(job=job, step=step.get("name") or uses):
                    if uses.startswith("actions/cache/restore@"):
                        seen += 1
                        continue  # restore-only by construction
                    if uses.startswith(("actions/cache@", "actions/cache/save@")) \
                            or "cache-action" in uses or "namespace-macos-cache" in uses:
                        seen += 1
                        condition = self._condition(step)
                        # Either push-to-main only, or explicitly off a pr_number dispatch.
                        self.assertTrue("github.event_name == 'push'" in condition
                                        or self.NO_PR in condition, condition)
                    self.assertNotIn("actions/cache@", uses,
                                     "actions/cache saves in its post step; use cache/restore")
        self.assertGreaterEqual(seen, 4, "control: the scan found the cache steps")

    def test_setup_actions_do_not_save_caches_on_the_pr_dispatch_path(self) -> None:
        for job in ("classify", "build"):
            for step in self.jobs[job]["steps"]:
                uses = str(step.get("uses", ""))
                cache = (step.get("with") or {}).get("cache")
                if uses.startswith("actions/setup-") and cache:
                    with self.subTest(step=step["name"]):
                        self.assertEqual(cache, "${{ inputs.pr_number == '' && 'npm' || '' }}")

    def test_consumable_artifacts_are_not_uploaded_on_the_pr_dispatch_path(self) -> None:
        build = steps("build")
        for name in self.SKIPPED_UPLOADS:
            with self.subTest(step=name):
                self.assertIn(self.NO_PR, self._condition(build[name]))
        self.assertEqual(build["Upload exact GPU-audio SDK (macOS ARM64, retry)"]["if"],
                         "steps.gpu_audio_sdk_upload.outcome == 'failure'")
        # The protected receipt and the A2T attestation are pull_request-only already.
        self.assertIn("github.event_name == 'pull_request'",
                      self._condition(build["Issue exact protected-validation receipt"]))


class RoutingParityTest(LinuxRoutingTest):
    """A pr_number dispatch routes like a pull_request run: macOS on the PR-head
    class, Linux hosted, no Windows, and no operator routing input applies."""

    def _matrix(self, event: str, **env: str) -> dict:
        base = {k: v for k, v in os.environ.items()
                if not k.startswith(("GITHUB_", "PULP_", "EXPLICIT_", "NAMESPACE_"))}
        base.update({
            "REQUESTED_PROVIDER": "github-hosted", "GITHUB_EVENT_NAME": event,
            "GITHUB_REPOSITORY": REPO, "GITHUB_WORKSPACE": str(ROOT),
            "WORKFLOW_REF": f"{REPO}/.github/workflows/build.yml@refs/heads/topic",
            "EXPLICIT_LINUX_RUNNER_SELECTOR_JSON": "", "EXPLICIT_WINDOWS_RUNNER_SELECTOR_JSON": "",
            "CONFIGURED_LINUX_RUNNER_SELECTOR_JSON": json.dumps(LOCAL_LINUX),
            "WORKFLOW_DISPATCH_LINUX_SELECTOR": "", "WORKFLOW_DISPATCH_RUN_WINDOWS": "true",
            "WORKFLOW_DISPATCH_MACOS_SELECTOR": "",
            "LOCAL_MACOS_RUNS_ON_JSON": json.dumps(["self-hosted", "macOS", "ARM64", "pulp-build"]),
            "OVERFLOW_MACOS_RUNS_ON_JSON": "local-only",
            "NAMESPACE_LINUX_RUNS_ON_JSON": "", "NAMESPACE_WINDOWS_RUNS_ON_JSON": "",
            "WORKFLOW_DISPATCH_ALLOW_HOSTED_WINDOWS": "", "ALLOW_HOSTED_WINDOWS_VAR": "",
            "DISPATCH_PR_NUMBER": "",
        })
        base.update(env)
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "out"
            output.write_text("", encoding="utf-8")
            base["GITHUB_OUTPUT"] = str(output)
            proc = subprocess.run([sys.executable, "-c", self.resolver], cwd=ROOT, env=base,
                                  capture_output=True, text=True, encoding="utf-8", check=False)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            for line in output.read_text(encoding="utf-8").splitlines():
                if line.startswith("matrix_json="):
                    return {leg["key"]: json.loads(leg["runs_on_json"])
                            for leg in json.loads(line.split("=", 1)[1])["include"]}
        raise AssertionError("resolver emitted no matrix_json")

    def test_a_pr_dispatch_matches_a_pull_request_runs_legs(self) -> None:
        pr_run = self._matrix("pull_request")
        dispatch = self._matrix("workflow_dispatch", DISPATCH_PR_NUMBER="11053",
                                # Even with every Windows opt-in a plain dispatch honours:
                                WORKFLOW_DISPATCH_ALLOW_HOSTED_WINDOWS="true",
                                ALLOW_HOSTED_WINDOWS_VAR="1")
        self.assertEqual(sorted(dispatch), sorted(pr_run))
        self.assertNotIn("windows", dispatch)
        self.assertIn("pulp-build-pr-head", dispatch["macos"])
        self.assertNotIn("pulp-build-merge-group", dispatch["macos"])
        self.assertEqual(dispatch["macos"], pr_run["macos"])
        self.assertEqual(dispatch["linux"], "ubuntu-latest")
        # Control: a plain dispatch with the same opt-ins does schedule Windows.
        # (with the operator Linux selector a plain dispatch's expression supplies).
        self.assertIn("windows", self._matrix("workflow_dispatch", WORKFLOW_DISPATCH_ALLOW_HOSTED_WINDOWS="true",
                                              EXPLICIT_LINUX_RUNNER_SELECTOR_JSON=json.dumps(LOCAL_LINUX)))

    def test_no_operator_routing_input_reaches_the_pr_dispatch(self) -> None:
        env = self.env
        for key, expected in (
                ("REQUESTED_PROVIDER", "${{ inputs.pr_number == '' && inputs.runner_provider || vars.PULP_DEFAULT_RUNNER_PROVIDER || 'github-hosted' }}"),
                ("EXPLICIT_WINDOWS_RUNNER_SELECTOR_JSON", "${{ inputs.pr_number == '' && inputs.windows_runner_selector_json || '' }}"),
                ("WORKFLOW_DISPATCH_MACOS_SELECTOR", "${{ inputs.pr_number == '' && inputs.macos_runner_selector_json || '' }}")):
            with self.subTest(key=key):
                self.assertEqual(env[key], expected)

    def test_the_build_dir_is_the_same_on_every_event(self) -> None:
        # The self-hosted warm dir and host caches follow the PR-head class
        # and the matrix key, never the event.
        self.assertEqual(workflow()["jobs"]["build"]["env"]["PULP_BUILD_DIR"], "build-${{ matrix.key }}")


class TrustAndIsolationTest(unittest.TestCase):
    """No trust-bearing output, no cancelled PR or queue run, no extra secret."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.workflow = workflow()
        cls.text = BUILD.read_text(encoding="utf-8")

    @staticmethod
    def _condition(step: dict) -> str:
        return " ".join(str(step.get("if", "")).split())

    def test_only_diagnostic_artifacts_upload_on_the_pr_dispatch_path(self) -> None:
        reachable = set()
        for step in self.workflow["jobs"]["build"]["steps"]:
            if not str(step.get("uses", "")).startswith("actions/upload-artifact"):
                continue
            condition = self._condition(step)
            if any(gate in condition for gate in (
                    "inputs.pr_number == ''", "github.event_name == 'pull_request'",
                    "github.event_name == 'push'", "github.event_name == 'merge_group'",
                    "steps.gpu_audio_sdk_upload.outcome == 'failure'",
                    # Publishes only what the pull_request-only issue step made.
                    "steps.protected_receipt.outcome == 'success'",
                    "a2t_receipt_attestation_required == 'true'")):
                continue
            reachable.add(step["name"])
        self.assertEqual(reachable, {"Upload the Rust fallback receipt", "Upload ctest logs and JUnit report"})

    def test_the_protected_receipt_is_pull_request_only(self) -> None:
        build = steps("build")
        self.assertIn("github.event_name == 'pull_request'",
                      self._condition(build["Issue exact protected-validation receipt"]))
        self.assertEqual(build["Publish exact protected-validation receipt"]["if"],
                         "steps.protected_receipt.outcome == 'success'")

    def test_no_workflow_run_consumer_of_build_and_test_reads_artifacts(self) -> None:
        for path in sorted((ROOT / ".github/workflows").glob("*.y*ml")):
            data = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
            triggers = data.get("on", data.get(True)) or {}
            if isinstance(triggers, dict) and "Build and Test" in ((triggers.get("workflow_run") or {}).get("workflows") or []):
                with self.subTest(workflow=path.name):
                    text = path.read_text(encoding="utf-8")
                    self.assertNotIn("download-artifact", text)
                    self.assertNotIn("/artifacts", text)

    def test_artifact_consumers_never_read_dispatch_runs(self) -> None:
        # Each reads only the trusted gate events, so even an artifact a
        # dispatch did upload (the earlier proof run's reuse record) is never
        # consumed.
        sys.path.insert(0, str(ROOT / "tools/ci"))
        import ios_gate_digest  # noqa: E402
        self.assertEqual(ios_gate_digest.TRUSTED_EVENTS, ("merge_group", "pull_request"))
        for rel in ("tools/scripts/reuse_replay_collect.py", "tools/ci/binary_identity_shadow.py",
                    "tools/ci/base_poison_detector.py", "tools/ci/ios_gate_digest.py",
                    "tools/scripts/protected_merge_receipt.py"):
            with self.subTest(consumer=rel):
                self.assertNotIn("workflow_dispatch", (ROOT / rel).read_text(encoding="utf-8"))
        collect = (ROOT / "tools/scripts/reuse_replay_collect.py").read_text(encoding="utf-8")
        self.assertEqual(sorted(set(__import__("re").findall(r'self\.list_runs\("([a-z_]+)"', collect))),
                         ["merge_group", "pull_request"])

    def test_a_pr_dispatch_has_its_own_concurrency_group(self) -> None:
        # The group's discriminator is github.ref: refs/heads/<branch> for the
        # dispatch, refs/pull/<n>/merge for the PR run, the queue ref for a
        # merge group. pr_number never enters it, so the dispatch cannot
        # cancel either.
        group = self.workflow["concurrency"]["group"]
        self.assertTrue(group.startswith("build-${{") and group.rstrip().endswith("|| github.ref }}"), group)
        self.assertNotIn("pr_number", group)
        refs = {"dispatch": "refs/heads/topic", "pull_request": "refs/pull/7/merge",
                "merge_group": "refs/heads/gh-readonly-queue/main/pr-7-" + "c" * 40}
        self.assertEqual(len({f"build-{ref}" for ref in refs.values()}), 3)

    def test_no_secret_or_environment_beyond_a_pull_request_run(self) -> None:
        self.assertNotIn("secrets.", self.text)
        for name, job in self.workflow["jobs"].items():
            with self.subTest(job=name):
                self.assertNotIn("environment", job)
        # The jobs that raise their own token scope never run on a dispatch's
        # PR path: merge-group, push-to-main and local-proof only.
        raised = {name: self._condition(job) for name, job in self.workflow["jobs"].items()
                  if job.get("permissions")}
        self.assertEqual(set(raised), {"queue-breaker-notify", "a2t-headroom-issue"})
        self.assertIn("github.event_name == 'merge_group'", raised["queue-breaker-notify"])
        self.assertIn("github.event_name == 'push'", raised["a2t-headroom-issue"])


class WindowsAliasTest(unittest.TestCase):
    """The windows alias passes a pr_number dispatch exactly as it does a PR.

    resolve-provider omits the Windows leg for both. The alias used to early-
    pass only `pull_request`, so a pr_number dispatch (run_windows defaults to
    true) failed it with "Windows leg not found" (proof run 40225987931).
    """

    @classmethod
    def setUpClass(cls) -> None:
        cls.job = workflow()["jobs"]["windows"]
        cls.step = cls.job["steps"][0]

    def _run(self, event: str, pr_number: str, run_windows: str, windows_leg: str,
             gh_conclusion: str = "") -> subprocess.CompletedProcess:
        values = {"needs.classify.result": "success",
                  "needs.classify.outputs.native_build_required": "true",
                  "github.event_name": event, "inputs.run_windows": run_windows,
                  "github.repository": REPO, "github.run_id": "1",
                  "needs.windows-msvc-release-gate.result": "skipped"}
        script = self.step["run"]
        for expr, value in values.items():
            script = script.replace("${{ " + expr + " }}", value)
        self.assertNotIn("${{", script, "every expression the alias uses is substituted")
        with tempfile.TemporaryDirectory() as tmp:
            bin_dir = Path(tmp) / "bin"
            bin_dir.mkdir()
            (bin_dir / "gh").write_text(f"#!/bin/sh\nprintf '%s' '{gh_conclusion}'\n", encoding="utf-8")
            (bin_dir / "gh").chmod(0o755)
            proc, _ = bash(script, {"PATH": f"{bin_dir}{os.pathsep}{os.environ['PATH']}",
                                    "WINDOWS_LEG": windows_leg, "HOSTED_WINDOWS_ALLOWED": "false",
                                    "DISPATCH_PR_NUMBER": pr_number, "GH_TOKEN": "x"})
            return proc

    def test_a_pr_dispatch_passes_like_a_pull_request(self) -> None:
        omitted = "omitted: pull request or merge group"
        for event, pr_number in (("pull_request", ""), ("workflow_dispatch", "11053")):
            with self.subTest(event=event):
                proc = self._run(event, pr_number, run_windows="true", windows_leg=omitted)
                self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
                self.assertIn("not on the PR head — windows alias ✓", proc.stdout)
        self.assertEqual(self.step["env"]["DISPATCH_PR_NUMBER"], "${{ inputs.pr_number }}")

    def test_a_plain_dispatch_still_requires_its_windows_leg(self) -> None:
        # Control: no early pass; with no Windows job found it fails as before.
        proc = self._run("workflow_dispatch", "", run_windows="true", windows_leg="included")
        self.assertEqual(proc.returncode, 1, proc.stdout)
        self.assertIn("Windows leg not found", proc.stdout)

    def test_no_hosted_windows_gate_runs_on_a_pr_dispatch(self) -> None:
        for name in ("windows-msvc-release-gate", "windows-midi2-gate", "windows-ble-gate"):
            with self.subTest(job=name):
                self.assertIn("&& inputs.pr_number == ''",
                              " ".join(str(workflow()["jobs"][name]["if"]).split()))

    def test_the_intel_canary_lints_a_pr_dispatchs_change_against_its_merged_base(self) -> None:
        step = steps("build")["Intel canary — lint + x86_64 GPU-off compile"]
        self.assertEqual(step["env"]["INTEL_CANARY_PR_BASE"],
                         "${{ github.event.pull_request.base.sha || steps.tested.outputs.merged_base }}")
        self.assertNotIn('"${{ github.event_name }}" = "pull_request"', step["run"])

if __name__ == "__main__":
    unittest.main()

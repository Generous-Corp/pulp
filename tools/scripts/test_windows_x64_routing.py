#!/usr/bin/env python3
"""Pin which Windows x64 jobs take the self-hosted Windows x64 lane.

The lane is one repository-scoped VM (docs/guides/local-ci.md, "The Windows x64
slot") that runs one job at a time. Every routed job resolves its runner through
tools/scripts/windows_runner_selector.py --route, which defaults to the lane and
uses GitHub-hosted only with PULP_ALLOW_HOSTED_WINDOWS=1 (or a fork pull
request). No pull request reaches the lane: the jobs a pull request can start
run Windows x64 hosted on the opt-in and otherwise leave it out.

The step scripts are executed here, not modelled, with a fake `gh` standing in
for the backlog probe, so the tests stay offline.

Run:  python3 tools/scripts/test_windows_x64_routing.py
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

import yaml

REPO_ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS = REPO_ROOT / ".github" / "workflows"
LANE = ["self-hosted", "Windows", "X64", "pulp-build-windows-x64"]

# (workflow, job whose step runs the selector) for every routed Windows x64 job.
ROUTED = {
    ("cross-platform-check.yml", "windows-runner"),
    ("software-gpu-proof.yml", "windows-runner"),
    ("freshness-check.yml", "windows-runner"),
    ("android.yml", "resolve-runners"),
    ("coverage.yml", "resolve-runners"),
    ("control-shipping-native.yml", "legs"),
}

# Windows jobs that stay GitHub-hosted, with the reason. Adding one is a review
# decision; the inventory test below fails on any literal hosted Windows label
# that is not listed.
HOSTED_BY_DESIGN = {
    ("build.yml", "windows-msvc-release-gate"): "tracks windows-latest's moving toolchain; opt-in only",
    ("build.yml", "windows-midi2-gate"): "needs windows-latest's preset vcpkg; opt-in only",
    ("build.yml", "windows-ble-gate"): "tracks windows-latest's moving toolchain; opt-in only",
    ("windows-cli-compile.yml", "compile"): "mirrors release-cli.yml's windows-latest build",
}


def load(name: str) -> dict:
    return yaml.safe_load((WORKFLOWS / name).read_text(encoding="utf-8"))


def selector_steps(spec: dict) -> list[dict]:
    return [step for step in spec.get("steps", [])
            if "windows_runner_selector.py --route" in str(step.get("run", ""))]


def fake_gh(directory: Path, backlog: int) -> Path:
    """A `gh` that lists one other run holding `backlog` lane jobs."""
    gh = directory / "gh"
    gh.write_text(
        "#!/bin/sh\n"
        'case "$2" in\n'
        '  *"runs?status=queued"*) echo 501 ;;\n'
        '  *"runs?status="*) ;;\n'
        f'  *"/runs/501/jobs"*) echo {backlog} ;;\n'
        "esac\n",
        encoding="utf-8",
    )
    gh.chmod(0o755)
    return directory


def run_step(script: str, env: dict[str, str], backlog: int) -> dict[str, str]:
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        output = tmp_path / "github_output"
        output.write_text("", encoding="utf-8")
        bin_dir = fake_gh(tmp_path, backlog)
        full_env = {
            "PATH": f"{bin_dir}{os.pathsep}{os.environ.get('PATH', '/usr/bin:/bin')}",
            "GITHUB_OUTPUT": str(output),
            "GITHUB_REPOSITORY": "Generous-Corp/pulp",
            "GITHUB_RUN_ID": "1",
            **env,
        }
        result = subprocess.run(
            ["bash", "--noprofile", "--norc", "-c", script], cwd=REPO_ROOT, env=full_env,
            capture_output=True, text=True, encoding="utf-8", check=False, timeout=120,
        )
        if result.returncode != 0:
            raise AssertionError(f"step failed:\n{result.stdout}\n{result.stderr}")
        return dict(line.split("=", 1)
                    for line in output.read_text(encoding="utf-8").splitlines() if "=" in line)


class RoutedJobsTests(unittest.TestCase):
    def test_every_routed_job_reads_the_lane_and_the_opt_in(self) -> None:
        for workflow, job_name in sorted(ROUTED):
            with self.subTest(workflow=workflow, job=job_name):
                spec = load(workflow)["jobs"][job_name]
                steps = selector_steps(spec)
                self.assertEqual(len(steps), 1, "exactly one selector call")
                env = " ".join(str(value) for value in steps[0].get("env", {}).values())
                self.assertIn("vars.PULP_LOCAL_WINDOWS_X64_RUNS_ON_JSON", env)
                self.assertIn("vars.PULP_ALLOW_HOSTED_WINDOWS", env)

    def test_the_consumers_take_their_runner_from_the_resolver(self) -> None:
        consumers = {
            ("cross-platform-check.yml", "windows"): "needs.windows-runner.outputs.runs_on",
            ("software-gpu-proof.yml", "windows"): "needs.windows-runner.outputs.runs_on",
            ("freshness-check.yml", "build-validation"): "needs.windows-runner.outputs.runs_on",
            ("android.yml", "android-build"): "matrix.runs_on_json",
            ("coverage.yml", "coverage"): "matrix.runs_on_json",
            ("control-shipping-native.yml", "native"): "matrix.runs_on_json",
        }
        for (workflow, job_name), source in consumers.items():
            with self.subTest(workflow=workflow, job=job_name):
                runs_on = load(workflow)["jobs"][job_name]["runs-on"]
                self.assertIn(f"fromJSON({source})", runs_on)

    def test_pull_request_jobs_pass_the_head_repository(self) -> None:
        # The routed jobs a pull request can start. Both name the head
        # repository so the selector's fork check can see it.
        for workflow, job_name in (("coverage.yml", "resolve-runners"),
                                   ("control-shipping-native.yml", "legs")):
            with self.subTest(workflow=workflow):
                spec = load(workflow)["jobs"][job_name]
                self.assertIn("--head-repo", selector_steps(spec)[0]["run"])
        coverage = load("coverage.yml")["jobs"]["resolve-runners"]
        self.assertNotIn("--max-backlog", selector_steps(coverage)[0]["run"])
        self.assertEqual(coverage["permissions"].get("actions"), "read")

    def test_no_routed_job_is_a_required_context(self) -> None:
        ruleset = json.loads((REPO_ROOT / ".github/rulesets/main-protection.json").read_text(encoding="utf-8"))
        required = {check["context"] for rule in ruleset["rules"]
                    if rule["type"] == "required_status_checks"
                    for check in rule["parameters"]["required_status_checks"]}
        self.assertIn("macos", required, "control: the required list was read")
        self.assertFalse([context for context in required if "windows" in context.casefold()])

    def test_hosted_windows_labels_are_inventoried(self) -> None:
        found = set()
        for path in sorted(WORKFLOWS.glob("*.yml")):
            for job_name, spec in (yaml.safe_load(path.read_text(encoding="utf-8")).get("jobs") or {}).items():
                literal = " ".join([str(spec.get("runs-on", "")),
                                    json.dumps((spec.get("strategy") or {}).get("matrix", ""))])
                if re.search(r"(?<![\w-])windows-(latest|20\d\d)\b", literal):
                    found.add((path.name, job_name))
        self.assertEqual(found, set(HOSTED_BY_DESIGN))


class CoverageRouteTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.script = next(step["run"] for step in load("coverage.yml")["jobs"]["resolve-runners"]["steps"]
                          if step.get("id") == "resolve")

    def resolve(self, event: str, backlog: int = 0, **extra: str) -> tuple:
        env = {
            "GITHUB_EVENT_NAME": event,
            "THIS_REPO": "Generous-Corp/pulp",
            "PR_HEAD_REPO": "Generous-Corp/pulp" if event == "pull_request" else "",
            "EXPLICIT_LINUX_RUNNER_SELECTOR_JSON": "", "EXPLICIT_MACOS_RUNNER_SELECTOR_JSON": "",
            "EXPLICIT_WINDOWS_RUNNER_SELECTOR_JSON": "", "LINUX_RUNS_ON_JSON": "",
            "MACOS_RUNS_ON_JSON": "", "WINDOWS_RUNS_ON_JSON": "", "WINDOWS_X64_RUNS_ON_JSON": "",
            "ALLOW_HOSTED_WINDOWS": "", "ANDROID_KOTLIN_RUNS_ON_JSON": "",
            **extra,
        }
        out = run_step(self.script, env, backlog)
        return json.loads(out["windows_runs_on"]), out["windows_route"]

    def test_default_events_ignore_windows_lane_and_hosted_settings(self) -> None:
        for event in ("schedule", "workflow_dispatch", "pull_request"):
            with self.subTest(event=event):
                self.assertEqual(
                    self.resolve(event, ALLOW_HOSTED_WINDOWS="1", WINDOWS_X64_RUNS_ON_JSON=json.dumps(LANE)),
                    ("windows-latest", "skip"),
                )

    def test_schedule_and_pr_do_not_consume_a_dispatch_selector(self) -> None:
        for event in ("schedule", "pull_request"):
            for head in ("Generous-Corp/pulp", "fork/pulp", ""):
                with self.subTest(event=event, head=head):
                    self.assertEqual(
                        self.resolve(event, PR_HEAD_REPO=head, EXPLICIT_WINDOWS_RUNNER_SELECTOR_JSON=json.dumps(LANE)),
                        ("windows-latest", "skip"),
                    )

    def test_explicit_dispatch_uses_the_requested_route(self) -> None:
        for selector, route in ((LANE, "self-hosted"), ("windows-latest", "hosted")):
            with self.subTest(route=route):
                self.assertEqual(
                    self.resolve("workflow_dispatch", backlog=3,
                                 EXPLICIT_WINDOWS_RUNNER_SELECTOR_JSON=json.dumps(selector)),
                    (selector, route),
                )

    def test_explicit_dispatch_rejects_invalid_selector(self) -> None:
        with self.assertRaisesRegex(AssertionError, "windows-runner-selector: refused"):
            self.resolve("workflow_dispatch", EXPLICIT_WINDOWS_RUNNER_SELECTOR_JSON="{invalid")



class ControlShippingRouteTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.script = next(step["run"] for step in load("control-shipping-native.yml")["jobs"]["legs"]["steps"]
                          if step.get("id") == "legs")

    def legs(self, event: str, backlog: int = 0, **extra: str) -> tuple[dict, str]:
        env = {"GITHUB_EVENT_NAME": event, "DEFAULT_SELECTOR": "", "ALLOW_HOSTED": "",
               "HEAD_REPO": "Generous-Corp/pulp" if event == "pull_request" else "", **extra}
        out = run_step(self.script, env, backlog)
        include = json.loads(out["matrix"])["include"]
        return {leg["leg"]: json.loads(leg["runs_on_json"]) for leg in include}, out["deferred"]

    def test_windows_x64_takes_the_lane_and_the_other_legs_are_unchanged(self) -> None:
        legs, deferred = self.legs("schedule", backlog=4)
        self.assertEqual(legs["windows-x64"], LANE)
        self.assertEqual(deferred, "")
        self.assertEqual(legs["windows-arm64"], "windows-11-arm")
        self.assertEqual(legs["macos-universal"], "macos-15")
        self.assertEqual(legs["linux-x64"], "ubuntu-24.04")

    def test_an_automatic_pull_request_never_takes_the_lane(self) -> None:
        # Same-repo, fork, or no head repository, idle lane or busy: deferred.
        for head in ("Generous-Corp/pulp", "fork/pulp", ""):
            for backlog in (0, 1):
                with self.subTest(head=head, backlog=backlog):
                    legs, deferred = self.legs("pull_request", backlog=backlog, HEAD_REPO=head)
                    self.assertNotIn("windows-x64", legs)
                    self.assertEqual(deferred, "windows-x64")
                    self.assertEqual(len(legs), 4, "control: the other legs still run")
        # Even a lane variable cannot pull a pull request onto the lane.
        legs, deferred = self.legs("pull_request",
                                   DEFAULT_SELECTOR=json.dumps([*LANE, "pulp-host-macpro"]))
        self.assertEqual(deferred, "windows-x64")

    def test_a_pull_request_runs_windows_x64_hosted_only_with_the_opt_in(self) -> None:
        for head in ("Generous-Corp/pulp", "fork/pulp", ""):
            with self.subTest(head=head):
                legs, deferred = self.legs("pull_request", ALLOW_HOSTED="1", HEAD_REPO=head)
                self.assertEqual(legs["windows-x64"], "windows-2022")
                self.assertEqual(deferred, "")

    def test_hosted_is_the_opt_in_on_the_nightly(self) -> None:
        self.assertEqual(self.legs("schedule", ALLOW_HOSTED="1")[0]["windows-x64"], "windows-2022")
        self.assertEqual(self.legs("workflow_dispatch")[0]["windows-x64"], LANE)

    def test_a_hosted_lane_variable_keeps_the_legs_own_image(self) -> None:
        self.assertEqual(
            self.legs("schedule", DEFAULT_SELECTOR='"windows-latest"')[0]["windows-x64"],
            "windows-2022")

    def test_the_evidence_job_names_a_deferred_leg(self) -> None:
        evidence = load("control-shipping-native.yml")["jobs"]["evidence"]
        self.assertIn("legs", evidence["needs"])
        aggregate = next(step for step in evidence["steps"]
                         if step.get("name") == "Aggregate required native legs")
        self.assertIn("needs.legs.outputs.deferred", aggregate["env"]["DEFERRED_LEGS"])
        self.assertIn("--deferred-leg", aggregate["run"])


if __name__ == "__main__":
    unittest.main(verbosity=2)

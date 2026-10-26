#!/usr/bin/env python3
"""Lock the split between stable Windows runtime and latest-toolchain lanes.

The functional suite's hosted image is ``windows-2022`` while release and
standalone compile validation exercise ``windows-latest``. Coverage, the
nightly, and a dispatch's functional leg take the self-hosted Windows x64 lane
unless hosted is opted into (test_windows_x64_routing.py). Read every operative
workflow here so the policy cannot self-agree inside its documentation or
Shipyard mirror while a real lane silently drifts.

Run:  python3 tools/scripts/test_windows_runner_policy.py
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

import yaml


REPO_ROOT = Path(__file__).resolve().parent.parent.parent
WORKFLOWS = REPO_ROOT / ".github" / "workflows"


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def job(workflow: str, name: str) -> str:
    marker = f"\n  {name}:\n"
    if marker not in workflow:
        raise AssertionError(f"workflow job not found: {name}")
    remainder = workflow.split(marker, 1)[1]
    return re.split(r"\n  [A-Za-z0-9_-]+:\n", remainder, 1)[0]


class WindowsRunnerPolicyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.build = read(WORKFLOWS / "build.yml")
        cls.release = read(WORKFLOWS / "release-cli.yml")
        cls.coverage = read(WORKFLOWS / "coverage.yml")
        cls.nightly = read(WORKFLOWS / "cross-platform-check.yml")
        cls.release_resolver = read(
            REPO_ROOT / "tools" / "scripts" / "resolve_release_runners.py"
        )
        cls.profile = read(
            REPO_ROOT
            / ".shipyard"
            / "ci-profiles"
            / "normal-local-fast.toml"
        )

    def test_authoritative_functional_suite_uses_windows_2022(self) -> None:
        resolver = job(self.build, "resolve-provider")
        windows_call = resolver.split(
            '"--target-name", "Windows (x64)"', 1
        )[1].split("])", 1)[0]
        self.assertIn('"--github-hosted-label", "windows-2022"', windows_call)
        self.assertNotIn("windows-latest", windows_call)
        self.assertEqual(
            len(re.findall(r"(?m)^\s*windows_runs_on\s*=", resolver)), 1
        )
        self.assertIn(
            "matrix_json: ${{ steps.resolve.outputs.matrix_json }}", resolver
        )
        self.assertIn(
            '"runs_on_json": windows_runs_on', resolver
        )
        self.assertIn(
            'handle.write(f"matrix_json={json.dumps(matrix)}\\n")', resolver
        )

        consumer = job(self.build, "build")
        self.assertIn(
            "needs: [resolve-provider, classify, protected-receipt-reuse]",
            consumer,
        )
        self.assertIn(
            "needs.protected-receipt-reuse.outputs.matrix_json || needs.resolve-provider.outputs.matrix_json",
            consumer,
        )
        self.assertIn(
            "runs-on: ${{ fromJSON(matrix.runs_on_json) }}", consumer
        )

    def test_standalone_latest_toolchain_gates_stay_on_latest(self) -> None:
        for name in (
            "windows-msvc-release-gate",
            "windows-midi2-gate",
            "windows-ble-gate",
        ):
            with self.subTest(job=name):
                section = job(self.build, name)
                self.assertIn("runs-on: windows-latest", section)
                self.assertNotIn("windows-2022", section)

    def test_release_build_and_smoke_stay_on_latest(self) -> None:
        resolver = job(self.release, "resolve-macos-runner")
        self.assertIn(
            "python3 tools/scripts/resolve_release_runners.py --github-output",
            resolver,
        )
        self.assertRegex(
            self.release_resolver,
            r'(?m)^\s*"windows-x64":\s*"windows-latest",$',
        )
        self.assertIn(
            'print("map=" + json.dumps(resolved))', self.release_resolver
        )
        self.assertIn(
            "map: ${{ steps.resolve.outputs.map }}", resolver
        )
        clean_env = dict(os.environ)
        for name in (
            "DARWIN_ARM64",
            "DARWIN_X64",
            "LINUX_X64",
            "LINUX_ARM64",
            "WINDOWS_X64",
            "WINDOWS_ARM64",
            "LOCAL_MACOS",
            "NAMESPACE_JSON",
        ):
            clean_env.pop(name, None)
        emitted = subprocess.run(
            [
                sys.executable,
                str(
                    REPO_ROOT
                    / "tools"
                    / "scripts"
                    / "resolve_release_runners.py"
                ),
                "--github-output",
            ],
            capture_output=True,
            text=True,
            check=False,
            env=clean_env,
        )
        self.assertEqual(emitted.returncode, 0, emitted.stderr)
        map_line = next(
            line for line in emitted.stdout.splitlines() if line.startswith("map=")
        )
        emitted_map = json.loads(map_line.removeprefix("map="))
        self.assertEqual(emitted_map["windows-x64"], "windows-latest")
        # The matrix legs are data now (release_build_matrix.py, filtered by
        # active_platforms), so the windows-latest invariant is asserted on
        # the leg map the workflow derives its include lists from.
        sys.path.insert(0, str(REPO_ROOT / "tools" / "scripts"))
        try:
            import release_build_matrix
        finally:
            sys.path.pop(0)
        self.assertEqual(
            release_build_matrix.BUILD_LEGS["windows-x64"]["os"],
            "windows-latest",
        )
        for config in release_build_matrix.BUILD_LEGS.values():
            self.assertNotEqual(config["os"], "windows-2022")
        for name in ("build-cli", "smoke-cli"):
            with self.subTest(job=name):
                section = job(self.release, name)
                self.assertRegex(
                    section, r"(?m)^\s*needs:.*resolve-macos-runner"
                )
                self.assertRegex(
                    section,
                    r"include:\s*\$\{\{\s*fromJSON\(needs\.resolve-macos-runner"
                    r"\.outputs\.(build|smoke)_include\)\s*\}\}",
                )
                output = "map" if name == "build-cli" else "smoke_map"
                self.assertIn(
                    f"runs-on: ${{{{ fromJSON(needs.resolve-macos-runner.outputs.{output})"
                    "[matrix.platform] }}",
                    section,
                )
                self.assertNotIn("windows-2022", section)
        smoke_line = next(
            line for line in emitted.stdout.splitlines() if line.startswith("smoke_map=")
        )
        self.assertEqual(
            json.loads(smoke_line.removeprefix("smoke_map="))["windows-x64"],
            "windows-latest",
        )

    def test_coverage_windows_routes_through_the_lane_selector(self) -> None:
        # Behaviour per event is exercised in test_windows_x64_routing.py; this
        # pins the chain from the resolver to the matrix.
        resolver = job(self.coverage, "resolve-runners")
        self.assertIn("tools/scripts/windows_runner_selector.py --route", resolver)
        self.assertNotIn("windows-2022", resolver)
        # Exactly one assignment. A later reassignment before publication
        # would silently sever this chain.
        self.assertEqual(len(re.findall(r"(?m)^\s*windows\s*=", resolver)), 1)
        self.assertIn(
            "windows_runs_on: ${{ steps.resolve.outputs.windows_runs_on }}",
            resolver,
        )
        self.assertIn('echo "windows_runs_on=${windows}"', resolver)
        self.assertIn('echo "windows_route=${windows_leg}"', resolver)

        matrix_config = job(self.coverage, "matrix-config")
        self.assertIn("needs: [resolve-runners]", matrix_config)
        self.assertIn(
            "WINDOWS_RUNS_ON_JSON: "
            "${{ needs.resolve-runners.outputs.windows_runs_on }}",
            matrix_config,
        )
        self.assertIn(
            '--argjson runs_on "${WINDOWS_RUNS_ON_JSON}"', matrix_config
        )
        self.assertIn(
            "outputs:\n      matrix: ${{ steps.build.outputs.matrix }}",
            matrix_config,
        )
        self.assertIn(
            'echo "matrix=${matrix}" >> "${GITHUB_OUTPUT}"', matrix_config
        )

        consumer = job(self.coverage, "coverage")
        self.assertRegex(consumer, r"(?m)^\s*needs:.*matrix-config")
        self.assertIn(
            "matrix: ${{ fromJSON(needs.matrix-config.outputs.matrix) }}",
            consumer,
        )
        self.assertIn(
            "runs-on: ${{ fromJSON(matrix.runs_on_json) }}", consumer
        )

    def test_cross_platform_windows_takes_a_policed_selector_with_lane_default(self) -> None:
        # The Windows job's runner comes only from the resolver job, which runs
        # tools/scripts/windows_runner_selector.py --route over the dispatch
        # input. An empty input (the schedule passes none) takes the
        # self-hosted Windows x64 lane; hosted needs the repository opt-in.
        windows = job(self.nightly, "windows")
        self.assertIn("needs: windows-runner", windows)
        self.assertIn("runs-on: ${{ fromJSON(needs.windows-runner.outputs.runs_on) }}", windows)
        self.assertNotIn("windows-2022", windows)
        resolver = job(self.nightly, "windows-runner")
        self.assertIn("runs-on: ubuntu-latest", resolver)
        self.assertIn("tools/scripts/windows_runner_selector.py --route", resolver)
        self.assertIn('--selector "$SELECTOR"', resolver)
        self.assertIn("SELECTOR: ${{ inputs.windows_runner_selector_json || '' }}", resolver)
        self.assertIn("ALLOW_HOSTED: ${{ vars.PULP_ALLOW_HOSTED_WINDOWS || '' }}", resolver)
        default = subprocess.run(
            [sys.executable, str(REPO_ROOT / "tools/scripts/windows_runner_selector.py"),
             "--route", "--event-name", "schedule"],
            capture_output=True, text=True, encoding="utf-8", check=True,
        ).stdout.splitlines()[0]
        self.assertEqual(
            default, 'runs_on=["self-hosted","Windows","X64","pulp-build-windows-x64"]')
        triggers = yaml.safe_load(self.nightly)[True]
        self.assertEqual(
            triggers["workflow_dispatch"]["inputs"]["windows_runner_selector_json"]["default"], ""
        )

    def test_a_relocated_windows_run_never_touches_the_tracking_issues(self) -> None:
        tracking = job(self.nightly, "tracking-issues")
        self.assertIn("!inputs.windows_runner_selector_json", tracking)
        self.assertIn("!inputs.windows_only", tracking)

    def test_shipyard_mirror_preserves_runtime_and_latest_targets(self) -> None:
        pr_windows = self.profile.split(
            '[repo."Generous-Corp/pulp".pr.windows]', 1
        )[1].split("\n[", 1)[0]
        coverage_windows = self.profile.split(
            '[repo."Generous-Corp/pulp".coverage.windows]', 1
        )[1].split("\n[", 1)[0]
        scheduled = self.profile.split(
            '[repo."Generous-Corp/pulp".scheduled.nightly_intel]', 1
        )[1].split("\n[", 1)[0]
        runtime = self.profile.split(
            '[targets."github.windows-x64-runtime"]', 1
        )[1].split("\n[", 1)[0]
        latest = self.profile.split(
            '[targets."github.windows-x64"]', 1
        )[1].split("\n[", 1)[0]

        self.assertIn('targets = ["github.windows-x64-runtime"]', pr_windows)
        self.assertIn('targets = ["github.windows-x64"]', coverage_windows)
        self.assertIn('"github.windows-x64"', scheduled)
        self.assertIn('runs_on_json = "windows-2022"', runtime)
        self.assertIn('runs_on_json = "windows-latest"', latest)


class ProtectedLinuxFallbackTests(unittest.TestCase):
    """Rejected protected selectors must reach the literal hosted fallback."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.workflow = yaml.safe_load(read(WORKFLOWS / "pr-safe-linux.yml"))
        cls.admission = cls.workflow["jobs"]["admission"]
        cls.linux = cls.workflow["jobs"]["linux"]

    def test_admission_rejects_whitespace_altered_labels(self) -> None:
        script = "\n".join(
            step.get("run", "") for step in self.admission["steps"]
        )
        self.assertIn(
            "if any(item != item.strip() for item in value):", script
        )

    def test_rejected_selector_skips_self_hosted_assignment_validation(self) -> None:
        assignment = next(
            step
            for step in self.linux["steps"]
            if step.get("name") == "Validate same-repository PR assignment"
        )
        self.assertEqual(
            assignment["if"], "needs.admission.outputs.admitted == 'true'"
        )
        self.assertIn('|| \'"ubuntu-latest"\'', self.linux["runs-on"])

    def test_denied_legacy_route_checks_out_event_owned_sha(self) -> None:
        checkout = next(
            step
            for step in self.linux["steps"]
            if step.get("uses") == "actions/checkout@v7"
        )
        checkout_ref = " ".join(checkout["with"]["ref"].split())
        self.assertIn(
            "needs.admission.outputs.admitted != 'true'", checkout_ref
        )
        self.assertIn("github.sha || inputs.source_sha", checkout_ref)


class WindowsMergeQueueGatingTests(unittest.TestCase):
    """Windows is gated by the merge queue, never by the PR head.

    Every run carries four hosted Windows jobs (the matrix leg plus the three
    latest-toolchain compile gates). The repo draws them from a fixed pool of
    concurrent GitHub-hosted jobs, so a handful of simultaneously open PRs
    fills nearly every slot with advisory Windows work and starves the one
    REQUIRED hosted check the merge queue waits on
    (``Build + prove + (owner-gated) deploy``, ubuntu-latest). Queue entries
    then expire in AWAITING_CHECKS and nothing lands. Windows keeps full
    coverage in the serial ``merge_group`` validation, which builds PR ∪ main.
    """

    WINDOWS_GATE_JOBS = (
        "windows-msvc-release-gate",
        "windows-midi2-gate",
        "windows-ble-gate",
    )

    @classmethod
    def setUpClass(cls) -> None:
        cls.workflow = yaml.safe_load(read(WORKFLOWS / "build.yml"))
        cls.resolver_script = cls._resolver_script()

    @staticmethod
    def _resolver_script() -> str:
        """The inline Python that builds the build-matrix, lifted out of the YAML."""
        for step in yaml.safe_load(read(WORKFLOWS / "build.yml"))["jobs"][
            "resolve-provider"
        ]["steps"]:
            body = step.get("run", "")
            if "matrix_json=" in body:
                return textwrap.dedent(
                    body.split("python3 - <<'PY'", 1)[1].rsplit("PY", 1)[0]
                )
        raise AssertionError("resolve-provider matrix step not found")

    def _matrix_keys(
        self, event_name: str, *, run_windows: bool = True,
        **extra: str,
    ) -> list[str]:
        """Run the real resolver for one event and return its matrix leg keys."""
        return [leg["key"] for leg in self._matrix(event_name, run_windows=run_windows, **extra)]

    def _macos_lanes(self, event_name: str, **extra: str) -> list[str]:
        return [leg.get("lane", "") for leg in self._matrix(event_name, **extra)
                if leg["key"] == "macos"]

    def _matrix(
        self, event_name: str, *, run_windows: bool = True,
        **extra: str,
    ) -> list[dict]:
        """Run the real resolver for one event and return its matrix legs."""
        env = {
            k: v
            for k, v in os.environ.items()
            if not k.startswith(("GITHUB_", "PULP_", "EXPLICIT_", "NAMESPACE_"))
        }
        env.update(
            {
                "REQUESTED_PROVIDER": "github-hosted",
                "GITHUB_EVENT_NAME": event_name,
                "GITHUB_REPOSITORY": "Generous-Corp/pulp",
                "GITHUB_WORKSPACE": str(REPO_ROOT),
                "EXPLICIT_LINUX_RUNNER_SELECTOR_JSON": "",
                "EXPLICIT_WINDOWS_RUNNER_SELECTOR_JSON": "",
                "WORKFLOW_DISPATCH_RUN_WINDOWS": (
                    "true" if run_windows else "false"
                ),
                "WORKFLOW_DISPATCH_MACOS_SELECTOR": "",
                "LOCAL_MACOS_RUNS_ON_JSON": json.dumps(
                    ["self-hosted", "macOS", "ARM64", "pulp-build"]
                ),
                # Sentinel: keeps macOS local so the resolver never reaches the
                # overflow branch, which would shell out to `gh` for a live
                # runner probe. This test must stay offline.
                "OVERFLOW_MACOS_RUNS_ON_JSON": "local-only",
                "NAMESPACE_LINUX_RUNS_ON_JSON": "",
                "NAMESPACE_WINDOWS_RUNS_ON_JSON": "",
                "WORKFLOW_DISPATCH_ALLOW_HOSTED_WINDOWS": "",
                "ALLOW_HOSTED_WINDOWS_VAR": "",
                "LOCAL_WINDOWS_X64_RUNS_ON_JSON": "",
            }
        )
        env.update(extra)
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "github_output"
            output.write_text("", encoding="utf-8")
            env["GITHUB_OUTPUT"] = str(output)
            result = subprocess.run(
                [sys.executable, "-c", self.resolver_script],
                cwd=REPO_ROOT,
                env=env,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(
                result.returncode, 0, f"resolver failed:\n{result.stderr}"
            )
            for line in output.read_text(encoding="utf-8").splitlines():
                if line.startswith("matrix_json="):
                    matrix = json.loads(line.split("=", 1)[1])
                    return matrix["include"]
        raise AssertionError("resolver emitted no matrix_json")

    def test_pull_request_matrix_drops_windows_but_keeps_macos_and_linux(self) -> None:
        keys = self._matrix_keys("pull_request")
        self.assertNotIn("windows", keys)
        # Negative control: the saving must come from Windows alone. macOS runs
        # on the self-hosted Macs while automatic PR Linux uses the hosted pool;
        # dropping either would still cost PR-head signal.
        self.assertIn("macos", keys)
        self.assertIn("linux", keys)

    GATED = {"PULP_MACOS_PR_GATING": "true"}

    def test_unapproved_pull_request_runs_only_the_macos_contract_lane(self) -> None:
        """A new head with gating on keeps a macOS leg, but configure-only."""
        push = {**self.GATED, "PR_EVENT_ACTION": "synchronize"}
        self.assertEqual(self._macos_lanes("pull_request", **push), ["contract"])
        self.assertIn("linux", self._matrix_keys("pull_request", **push))

    def test_gating_defaults_off(self) -> None:
        """Unset or not exactly `true`: every pull request takes the full lane."""
        for gating in ("", "false", "1"):
            with self.subTest(gating=gating):
                self.assertEqual(self._macos_lanes(
                    "pull_request", PULP_MACOS_PR_GATING=gating,
                    PR_EVENT_ACTION="synchronize"), ["full"])
        self.assertEqual(self._macos_lanes("pull_request"), ["full"])

    def test_the_apps_label_event_runs_the_full_lane_and_only_macos(self) -> None:
        approval = {**self.GATED, "PR_EVENT_ACTION": "labeled", "PR_LABEL_NAME": "macos-ci",
                    "PR_SENDER": "shipyard-local[bot]", "PR_LABELS_JSON": '["macos-ci"]'}
        self.assertEqual(self._matrix_keys("pull_request", **approval), ["macos"])
        self.assertEqual(self._macos_lanes("pull_request", **approval), ["full"])
        # Anyone else's label leaves the head on the contract lane.
        self.assertEqual(self._macos_lanes(
            "pull_request", **{**approval, "PR_SENDER": "a-maintainer"}), ["contract"])

    def test_deferral_never_reaches_merge_group_or_dispatch(self) -> None:
        """The merge queue and the escape hatch always run the full macOS lane."""
        for event in ("merge_group", "workflow_dispatch"):
            with self.subTest(event=event):
                self.assertEqual(self._macos_lanes(
                    event, **self.GATED, PR_EVENT_ACTION="synchronize"), ["full"])
                self.assertIn("linux", self._matrix_keys(event, **self.GATED))

    def test_merge_group_matrix_keeps_macos_and_local_eligible_linux(self) -> None:
        """Advisory hosted legs must not sit in the path every merge takes.

        A merge-group leg runs per queued entry, so Windows there is the single
        largest consumer of hosted minutes while gating nothing — `windows` is
        advisory, and only `macos` plus the version/skill and Vellum checks are
        required. Linux remains advisory and uses the protected disposable Mac
        Pro pool when configured, with the hosted resolver fallback.
        """
        keys = self._matrix_keys("merge_group")
        self.assertNotIn("windows", keys)
        self.assertIn("macos", keys)
        self.assertIn("linux", keys)

    def _windows_runs_on(self, event_name: str, **extra: str):
        legs = [leg for leg in self._matrix(event_name, **extra) if leg["key"] == "windows"]
        return json.loads(legs[0]["runs_on_json"]) if legs else None

    def test_a_default_push_schedules_no_windows(self) -> None:
        """GitHub-hosted Windows is opt-in, and a push never takes the
        self-hosted lane (every push-reachable job stays hosted)."""
        keys = self._matrix_keys("push")
        self.assertNotIn("windows", keys)
        self.assertIn("linux", keys, "control: the resolver ran and built a matrix")
        self.assertIsNone(self._windows_runs_on(
            "push", LOCAL_WINDOWS_X64_RUNS_ON_JSON=json.dumps(self.LANE)))

    LANE = ["self-hosted", "Windows", "X64", "pulp-build-windows-x64"]

    def test_a_default_dispatch_runs_windows_on_the_self_hosted_lane(self) -> None:
        self.assertEqual(self._windows_runs_on("workflow_dispatch"), self.LANE)
        configured = [*self.LANE, "pulp-host-macpro"]
        self.assertEqual(self._windows_runs_on(
            "workflow_dispatch", LOCAL_WINDOWS_X64_RUNS_ON_JSON=json.dumps(configured)),
            configured)

    def test_a_dispatch_reaches_hosted_windows_only_by_opting_in(self) -> None:
        self.assertEqual(self._windows_runs_on(
            "workflow_dispatch", WORKFLOW_DISPATCH_ALLOW_HOSTED_WINDOWS="true"), "windows-2022")
        self.assertEqual(self._windows_runs_on(
            "workflow_dispatch", ALLOW_HOSTED_WINDOWS_VAR="1"), "windows-2022")
        # A hosted label in the lane variable switches the lane off.
        self.assertEqual(self._windows_runs_on(
            "workflow_dispatch", LOCAL_WINDOWS_X64_RUNS_ON_JSON='"windows-latest"'), "windows-2022")
        # An explicit selector still wins over the lane default.
        explicit = [*self.LANE, "pulp-host-macpro"]
        self.assertEqual(self._windows_runs_on(
            "workflow_dispatch",
            EXPLICIT_WINDOWS_RUNNER_SELECTOR_JSON=json.dumps(explicit)), explicit)

    def test_workflow_dispatch_reaches_windows_only_by_opting_in(self) -> None:
        """Still reachable on demand, which is how a Windows fix gets verified
        without waiting for the nightly: by the dispatch input, by a local
        (self-hosted) selector, or by the repository variable."""
        self.assertIn("windows", self._matrix_keys(
            "workflow_dispatch", WORKFLOW_DISPATCH_ALLOW_HOSTED_WINDOWS="true"))
        self.assertIn("windows", self._matrix_keys(
            "workflow_dispatch", EXPLICIT_WINDOWS_RUNNER_SELECTOR_JSON=json.dumps(
                ["self-hosted", "Windows", "X64", "pulp-windows"])))
        self.assertIn("windows", self._matrix_keys(
            "push", ALLOW_HOSTED_WINDOWS_VAR="1"))
        # A selector that is itself a hosted label is not a local runner.
        self.assertNotIn("windows", self._matrix_keys(
            "workflow_dispatch",
            EXPLICIT_WINDOWS_RUNNER_SELECTOR_JSON=json.dumps("windows-2022")))
        # The input means nothing outside a dispatch.
        self.assertNotIn("windows", self._matrix_keys(
            "push", WORKFLOW_DISPATCH_ALLOW_HOSTED_WINDOWS="true"))
        # run_windows=false still wins over the opt-in.
        self.assertNotIn("windows", self._matrix_keys(
            "workflow_dispatch", run_windows=False,
            WORKFLOW_DISPATCH_ALLOW_HOSTED_WINDOWS="true"))

    def test_workflow_dispatch_can_omit_windows_for_trusted_linux_only_run(
        self,
    ) -> None:
        """Mac Pro dispatches must not worsen hosted Windows saturation."""
        keys = self._matrix_keys("workflow_dispatch", run_windows=False)
        self.assertNotIn("windows", keys)
        self.assertIn("macos", keys)
        self.assertIn("linux", keys)

    def test_latest_toolchain_gates_need_the_hosted_opt_in(self) -> None:
        for name in self.WINDOWS_GATE_JOBS:
            with self.subTest(job=name):
                condition = " ".join(self.workflow["jobs"][name]["if"].split())
                self.assertIn(
                    "&& (inputs.allow_hosted_windows == true || "
                    "vars.PULP_ALLOW_HOSTED_WINDOWS == '1')",
                    condition,
                )
        inputs = self.workflow[True]["workflow_dispatch"]["inputs"]
        self.assertIs(inputs["allow_hosted_windows"]["default"], False)

    def test_windows_alias_passes_when_hosted_windows_was_not_opted_in(self) -> None:
        steps = self.workflow["jobs"]["windows"]["steps"]
        body = "\n".join(step.get("run", "") for step in steps)
        self.assertIn('"omitted: GitHub-hosted Windows is opt-in"*)', body)
        self.assertIn('[ "$msvc_ok" = "skipped" ] && [ "$HOSTED_WINDOWS_ALLOWED" != "true" ]',
                      body)
        self.assertIn("resolve-provider", self.workflow["jobs"]["windows"]["needs"])

    def test_latest_toolchain_gates_skip_pull_request(self) -> None:
        for name in self.WINDOWS_GATE_JOBS:
            with self.subTest(job=name):
                condition = " ".join(self.workflow["jobs"][name]["if"].split())
                self.assertIn("github.event_name != 'pull_request'", condition)
                self.assertIn("github.event_name != 'push'", condition)
                self.assertIn(
                    "github.event_name != 'workflow_dispatch' || inputs.run_windows",
                    condition,
                )

    def test_windows_alias_short_circuits_on_pull_request(self) -> None:
        """Without this the advisory alias fails closed once the leg is absent."""
        steps = self.workflow["jobs"]["windows"]["steps"]
        body = "\n".join(step.get("run", "") for step in steps)
        self.assertIn("github.event_name }}\" = \"pull_request\"", body)

    def test_windows_alias_short_circuits_on_linux_only_dispatch(self) -> None:
        steps = self.workflow["jobs"]["windows"]["steps"]
        body = "\n".join(step.get("run", "") for step in steps)
        self.assertIn("inputs.run_windows", body)
        self.assertIn("Windows omitted by operator request", body)

    def test_required_macos_alias_never_consumes_preamble_capacity(self) -> None:
        """The skip/failure bootstrap must leave classifiers runnable.

        Native PRs report directly from the macOS matrix child. The bootstrap
        uses a dedicated alias pool so fail-closed/skip reporting cannot consume
        preamble capacity.
        """
        runs_on = self.workflow["jobs"]["macos"]["runs-on"]
        self.assertIn("PULP_ALIAS_RUNS_ON_JSON", runs_on)
        self.assertIn("ubuntu-latest", runs_on)
        self.assertNotIn("PULP_PREAMBLE_RUNS_ON_JSON", runs_on)

        merge_runs_on = self.workflow["jobs"]["macos-merge-group"]["runs-on"]
        self.assertIn("PULP_PREAMBLE_RUNS_ON_JSON", merge_runs_on)
        self.assertNotIn("PULP_ALIAS_RUNS_ON_JSON", merge_runs_on)

    def test_required_macos_alias_paths_do_not_share_advisory_dependencies(self) -> None:
        """Advisory results neither delay nor determine required macOS."""
        condition = " ".join(self.workflow["jobs"]["macos"]["if"].split())
        self.assertIn("github.event_name == 'pull_request'", condition)
        self.assertIn("github.event_name == 'workflow_dispatch'", condition)
        self.assertIn("native_build_required != 'true'", condition)
        self.assertEqual(
            self.workflow["jobs"]["macos"]["needs"],
            ["resolve-provider", "classify"],
        )
        self.assertNotIn("build", self.workflow["jobs"]["macos"]["needs"])
        self.assertNotIn("gh api", "\n".join(
            step.get("run", "")
            for step in self.workflow["jobs"]["macos"]["steps"]
        ))

        pr_alias = self.workflow["jobs"]["macos"]
        self.assertIn("macos-pr-unused", pr_alias["name"])

        merge_alias = self.workflow["jobs"]["macos-merge-group"]
        self.assertIn("macos-merge-unused", merge_alias["name"])
        self.assertIn("'macos'", merge_alias["name"])
        self.assertNotIn("build", merge_alias["needs"])
        self.assertIn("resolve-provider", merge_alias["needs"])
        self.assertIn("classify", merge_alias["needs"])
        merge_condition = " ".join(merge_alias["if"].split())
        self.assertIn("github.event_name == 'merge_group'", merge_condition)
        self.assertIn("native_build_required != 'true'", merge_condition)
        self.assertIn("resolve-provider.result != 'success'", merge_condition)

        build_name = self.workflow["jobs"]["build"]["name"]
        self.assertIn("github.event_name == 'pull_request'", build_name)
        self.assertIn("github.event_name == 'workflow_dispatch'", build_name)
        self.assertIn("github.event_name == 'merge_group'", build_name)
        self.assertIn("matrix.key == 'macos'", build_name)
        self.assertIn("'macos'", build_name)

    def test_advisory_aliases_do_not_run_without_merge_group_legs(self) -> None:
        for name in ("linux", "windows"):
            with self.subTest(job=name):
                condition = " ".join(self.workflow["jobs"][name]["if"].split())
                self.assertIn("github.event_name != 'merge_group'", condition)


HOSTED_WINDOWS_OPT_IN = "vars.PULP_ALLOW_HOSTED_WINDOWS == '1'"
# Jobs whose runner is computed at run time, so their opt-in lives in the
# computation; each is exercised by a test below. Adding a job here is a review
# decision, not a way around the check.
COMPUTED_WINDOWS_ROUTING = {
    ("build.yml", "build"): "resolve-provider omits the Windows leg (tested above)",
    ("android.yml", "android-build"): "resolve-runners omits the windows row on push",
}
# Hosted Windows a push may start without opting in, with the reason.
HOSTED_WINDOWS_ALLOWED = {
    ("windows-cli-compile.yml", "compile"): (
        "the push verdict on main is the release backstop auto-release.yml's "
        "pre-tag check reads (windows_cli_release_precheck.py); scoped to about "
        "three relevant merges a day"
    ),
}


def hosted_windows_violations(name: str, text: str) -> list[str]:
    """Jobs in workflow `name` that can start a GitHub-hosted Windows runner on
    a push (or, for build.yml, a dispatch) without the opt-in. Only literal
    `windows-*` labels are read; computed routing must be listed above."""
    doc = yaml.safe_load(text) or {}
    triggers = doc.get("on", doc.get(True)) or {}
    triggers = list(triggers) if isinstance(triggers, (dict, list)) else [triggers]
    covered = "push" in triggers or (name == "build.yml" and "workflow_dispatch" in triggers)
    if not covered:
        return []
    found = []
    for job_name, spec in (doc.get("jobs") or {}).items():
        literal = " ".join([str(spec.get("runs-on", "")),
                            json.dumps((spec.get("strategy") or {}).get("matrix", ""))])
        if not re.search(r"(?<![\w-])windows-[\w.-]+", literal):
            continue
        if (name, job_name) in COMPUTED_WINDOWS_ROUTING or (name, job_name) in HOSTED_WINDOWS_ALLOWED:
            continue
        if HOSTED_WINDOWS_OPT_IN not in " ".join(str(spec.get("if", "")).split()):
            found.append(f"{name}:{job_name}")
    return found


class HostedWindowsOptInTests(unittest.TestCase):
    """GitHub-hosted Windows is opt-in on a push, and on a build.yml dispatch."""

    def test_no_workflow_starts_hosted_windows_without_the_opt_in(self) -> None:
        violations = []
        scanned = 0
        for path in sorted(WORKFLOWS.glob("*.yml")):
            scanned += 1
            violations += hosted_windows_violations(path.name, read(path))
        self.assertGreater(scanned, 50, "control: the workflow directory was read")
        self.assertEqual(violations, [])

    def test_the_check_flags_an_ungated_push_job(self) -> None:
        ungated = textwrap.dedent("""\
            on:
              push:
                branches: [main]
            jobs:
              smoke:
                runs-on: windows-2022
              arm:
                strategy:
                  matrix:
                    os: [ubuntu-latest, windows-11-arm]
                runs-on: ${{ matrix.os }}
              gated:
                if: github.event_name != 'push' || vars.PULP_ALLOW_HOSTED_WINDOWS == '1'
                runs-on: windows-2022
              linux:
                runs-on: ubuntu-latest
            """)
        self.assertEqual(hosted_windows_violations("smoke.yml", ungated),
                         ["smoke.yml:smoke", "smoke.yml:arm"])
        # The same jobs on a schedule or a pull request are out of scope.
        self.assertEqual(hosted_windows_violations(
            "smoke.yml", ungated.replace("push:", "schedule:")), [])

    def test_every_listed_job_still_exists(self) -> None:
        for workflow, job_name in [*COMPUTED_WINDOWS_ROUTING, *HOSTED_WINDOWS_ALLOWED]:
            with self.subTest(workflow=workflow, job=job_name):
                self.assertIn(job_name, yaml.safe_load(read(WORKFLOWS / workflow))["jobs"])

    def android_matrix(self, event: str, allow: str = "") -> list:
        doc = yaml.safe_load(read(WORKFLOWS / "android.yml"))
        step = next(s for s in doc["jobs"]["resolve-runners"]["steps"] if s.get("id") == "resolve")
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / "out"
            output.write_text("", encoding="utf-8")
            env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "GITHUB_OUTPUT": str(output),
                   "MACOS_RUNS_ON_JSON": "", "EVENT_NAME": event,
                   "ALLOW_HOSTED_WINDOWS_VAR": allow, "WINDOWS_X64_RUNS_ON_JSON": ""}
            result = subprocess.run(["bash", "-c", step["run"]], cwd=REPO_ROOT, env=env,
                                    capture_output=True, text=True, encoding="utf-8",
                                    check=False, timeout=60)
            self.assertEqual(result.returncode, 0, result.stderr)
            line = next(l for l in output.read_text(encoding="utf-8").splitlines()
                        if l.startswith("matrix="))
        return [row["os"] if row["os"] == "macos-latest"
                else (row["os"], json.loads(row["runs_on_json"]))
                for row in json.loads(line.split("=", 1)[1])["include"]]

    def test_android_runs_windows_on_the_lane_nightly_and_hosted_only_by_opting_in(self) -> None:
        lane = ("windows-x64", ["self-hosted", "Windows", "X64", "pulp-build-windows-x64"])
        hosted = ("windows-x64", "windows-latest")
        for event in ("push", "pull_request", "merge_group"):
            with self.subTest(event=event):
                self.assertEqual(self.android_matrix(event), ["macos-latest"])
                self.assertEqual(self.android_matrix(event, "1"), ["macos-latest", hosted])
        for event in ("schedule", "workflow_dispatch"):
            with self.subTest(event=event):
                self.assertEqual(self.android_matrix(event), ["macos-latest", lane])
                self.assertEqual(self.android_matrix(event, "1"), ["macos-latest", hosted])

    def test_no_required_context_comes_from_android(self) -> None:
        ruleset = json.loads(read(REPO_ROOT / ".github/rulesets/main-protection.json"))
        required = {check["context"] for rule in ruleset["rules"]
                    if rule["type"] == "required_status_checks"
                    for check in rule["parameters"]["required_status_checks"]}
        self.assertIn("macos", required, "control: the required list was read")
        jobs = yaml.safe_load(read(WORKFLOWS / "android.yml"))["jobs"]
        for job_name, spec in jobs.items():
            with self.subTest(job=job_name):
                name = str(spec.get("name", job_name))
                self.assertNotIn(name, required)
                self.assertFalse(any(context.startswith(job_name) for context in required))


class TartMacosWorkflowPrerequisiteTests(unittest.TestCase):
    """Pin assumptions that clean per-job Tart guests deliberately do not make."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.web_plugins = read(WORKFLOWS / "web-plugins.yml")
        cls.examples_validation = read(WORKFLOWS / "examples-validation.yml")

    def test_gpu_audio_macos_installs_and_uses_discovered_chrome(self) -> None:
        macos = job(self.web_plugins, "gpu-audio-macos")
        self.assertRegex(
            macos,
            r"uses: browser-actions/setup-chrome@[0-9a-f]{40} # v2\.2\.0",
        )
        self.assertIn("id: chrome-macos", macos)
        self.assertEqual(
            macos.count(
                "CHROME_PATH: ${{ steps.chrome-macos.outputs.chrome-path }}"
            ),
            2,
        )
        self.assertNotIn("/Applications/Google Chrome.app", macos)

    def test_auval_validation_is_serial_but_other_validators_stay_parallel(
        self,
    ) -> None:
        validate = job(self.examples_validation, "validate")
        self.assertRegex(
            validate,
            r"-L validation -E '\^auval-' -j4",
        )
        self.assertRegex(
            validate,
            r"-L validation -R '\^auval-' -j1",
        )
        self.assertNotRegex(
            validate,
            r"--no-tests=ignore -L validation -j4",
        )



if __name__ == "__main__":
    unittest.main(verbosity=2)

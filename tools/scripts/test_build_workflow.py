#!/usr/bin/env python3
"""Structural invariants for exact protected receipt reuse in build.yml."""

from pathlib import Path
import json
import os
import re
import subprocess
import tempfile
import unittest

import yaml


WORKFLOW = (Path(__file__).parents[2] / ".github/workflows/build.yml").read_text(
    encoding="utf-8"
)
WGPU_NATIVE_DIAGNOSTIC_WORKFLOW_PATH = (
    Path(__file__).parents[2] / ".github/workflows/wgpu-native-diagnostic.yml"
)
SANITIZERS_WORKFLOW_PATH = Path(__file__).parents[2] / ".github/workflows/sanitizers.yml"


def _workflow() -> dict[str, object]:
    return yaml.safe_load(WORKFLOW)


def _sanitizers_workflow() -> dict[str, object]:
    return yaml.safe_load(SANITIZERS_WORKFLOW_PATH.read_text(encoding="utf-8"))


def _wgpu_native_diagnostic_workflow() -> dict[str, object]:
    return yaml.safe_load(
        WGPU_NATIVE_DIAGNOSTIC_WORKFLOW_PATH.read_text(encoding="utf-8")
    )


class MiseCandidateWorkflowTest(unittest.TestCase):
    """The M1 pinned-mise lane is explicit, main-only, and ordered safely."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.workflow = _workflow()
        trigger = cls.workflow.get("on", cls.workflow.get(True, {}))
        cls.dispatch = trigger["workflow_dispatch"]
        cls.steps = cls.workflow["jobs"]["build"]["steps"]

    def test_dispatch_is_explicitly_opt_in(self) -> None:
        candidate = self.dispatch["inputs"]["mise_candidate"]
        self.assertEqual(candidate["type"], "boolean")
        self.assertFalse(candidate["default"])
        self.assertIn("main-only", candidate["description"])

    def test_resolver_requires_main_and_pilot_selector(self) -> None:
        step = next(
            s for s in self.workflow["jobs"]["resolve-provider"]["steps"]
            if s.get("id") == "resolve"
        )
        text = step["run"]
        for marker in (
            "WORKFLOW_DISPATCH_MISE_CANDIDATE",
            "WORKFLOW_DISPATCH_LOCAL_PROOF",
            "mise_candidate is main-only workflow_dispatch",
            "mise_candidate cannot combine with local_proof",
            "pulp-build-vm-mise",
            "pulp-build-pr-head",
            "pulp-build-merge-group",
            "required_candidate_labels",
        ):
            self.assertIn(marker, text)
        guard = text.split("required_candidate_labels = {", 1)[1].split("}", 1)[0]
        self.assertIn('"pulp-build-pr-head"', guard)
        self.assertNotIn('"pulp-build-merge-group"', guard)

    def test_activation_precedes_all_build_tool_use(self) -> None:
        names = [step.get("name") for step in self.steps]
        activate = names.index("Activate pinned mise candidate toolchain")
        enforce = names.index("Enforce pinned mise candidate toolchain")
        bootstrap = names.index("Bootstrap repository dependencies")
        report = names.index("Toolchain report against the manifest (warn only)")
        configure = names.index("Configure")
        self.assertLess(activate, bootstrap)
        self.assertLess(activate, enforce)
        self.assertLess(enforce, bootstrap)
        self.assertLess(activate, report)
        self.assertLess(activate, configure)
        self.assertNotIn("continue-on-error", self.steps[enforce])
        self.assertIn(
            "python3 tools/toolchain/check_env.py --require-tools --format github",
            self.steps[enforce]["run"],
        )
        text = json.dumps(self.steps[activate], sort_keys=True)
        for marker in (
            "mise-2026.10.17/bin/mise",
            'RUNNER_ARCH',
            "expected_mise_version",
            "expected_mise_sha",
            "shasum -a 256",
            "actual_mise_version",
            "MISE_ENV=toolchain",
            "MISE_TRUSTED_CONFIG_PATHS",
            "PATH=$HOME/.local/opt/mise-2026.10.17/bin:$HOME/.cargo/bin:$tool_path",
            "GITHUB_ENV",
        ):
            self.assertIn(marker, text)
        self.assertNotIn("runner.arch == 'ARM64'", self.steps[activate]["if"])
        self.assertNotIn("runner.arch == 'ARM64'", self.steps[enforce]["if"])

    def test_candidate_mise_runs_locked_with_auto_install_off(self) -> None:
        run = self.steps[
            [step.get("name") for step in self.steps].index(
                "Activate pinned mise candidate toolchain"
            )
        ]["run"]
        first_mise = run.index('"$mise" exec')
        for setting in ("MISE_LOCKED=1", "MISE_AUTO_INSTALL=0"):
            # Set before the first mise call, and exported to every later step.
            self.assertLess(run.index(f"export {setting}"), first_mise, setting)
            self.assertIn(f'echo "{setting}"', run.split(">> \"$GITHUB_ENV\"")[0], setting)

    def test_non_arm_candidate_fails_before_build(self) -> None:
        activation = self.steps[
            [step.get("name") for step in self.steps].index(
                "Activate pinned mise candidate toolchain"
            )
        ]
        run = activation["run"]
        self.assertIn('RUNNER_ARCH', run)
        self.assertIn('= "ARM64"', run)
        self.assertIn('exit 1', run)


class WgpuNativeDiagnosticWorkflowTest(unittest.TestCase):
    """The native callback diagnostic is advisory and fail-closed."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.workflow = _wgpu_native_diagnostic_workflow()
        cls.text = WGPU_NATIVE_DIAGNOSTIC_WORKFLOW_PATH.read_text(encoding="utf-8")

    def test_build_gate_does_not_run_the_advisory_diagnostic(self) -> None:
        build_steps = _build_steps()
        self.assertNotIn(
            "Run wgpu-native staged device-loss diagnostic (macOS)", build_steps
        )
        self.assertNotIn("pulp-gpu-wgpu-native-staged-diagnostic", WORKFLOW)

    def test_advisory_workflow_has_pull_request_paths_and_manual_dispatch(self) -> None:
        triggers = self.workflow.get("on", self.workflow.get(True, {}))
        self.assertIn("pull_request", triggers)
        self.assertIn("workflow_dispatch", triggers)
        paths = triggers["pull_request"]["paths"]
        self.assertIn(".github/workflows/wgpu-native-diagnostic.yml", paths)
        self.assertIn("test/cmake/render_gpu_surface_tests.cmake", paths)
        dispatch = triggers["workflow_dispatch"]["inputs"]["run_diagnostic"]
        self.assertEqual(dispatch["type"], "boolean")
        self.assertIs(dispatch["default"], True)

    def test_unavailable_runtime_is_a_typed_advisory_skip(self) -> None:
        self.assertIn('"status": "provider_unavailable"', self.text)
        self.assertIn('"reason": "provider_unavailable"', self.text)
        self.assertIn('"claim": "device_loss_callback_unavailable_after_destroy"', self.text)
        self.assertIn('"device_loss_observation": "probe_timeout_after_destroy"', self.text)
        self.assertIn("test -s \"$receipt\"", self.text)

    def test_receipt_checks_preserve_typed_callback_scope(self) -> None:
        for field in (
            '"status": "provider_unavailable"',
            '"device_loss_callback": "unavailable"',
            '"device_loss_proof": "unavailable"',
            '"device_loss_no_output_leases": "unavailable"',
            '"status": "passed"',
            '"device_loss_callback": "registered"',
            '"device_loss_scenario": "triggered"',
            '"device_loss_reason": "destroyed"',
            '"device_loss_proof": "callback_terminal_no_output_lease"',
            '"device_loss_no_output_leases": True',
            '"device_loss_reprime": "unavailable"',
            '"callback_fallback_proof": "unavailable"',
            '"product_proof": False',
        ):
            self.assertIn(field, self.text)
        self.assertIn("device_loss_terminal_records", self.text)
        self.assertIn("architecture-only", self.text)
        self.assertIn("does not prove a production", self.text)

    def test_advisory_runner_is_resolved_and_not_required_build_host(self) -> None:
        job = self.workflow["jobs"]["diagnostic"]
        self.assertEqual(job["needs"], "resolve-advisory-macos")
        self.assertIn("fromJSON(needs.resolve-advisory-macos.outputs.selector_json)", job["runs-on"])
        self.assertEqual(self.workflow["jobs"]["resolve-advisory-macos"]["runs-on"], "ubuntu-latest")
        self.assertIn("resolve_advisory_macos_runner.py", self.text)


class ProtectedReceiptWorkflowTest(unittest.TestCase):
    def test_verifier_comes_from_exact_protected_base(self) -> None:
        self.assertIn(
            'git show "$base:tools/scripts/protected_merge_receipt.py"', WORKFLOW
        )
        self.assertIn('--group-sha "$group"', WORKFLOW)

    def test_any_unavailable_receipt_retains_full_target(self) -> None:
        reuse_block = WORKFLOW.split("  protected-receipt-reuse:", 1)[1].split(
            "\n  build:", 1
        )[0]
        # Reuse (dropping the target from the matrix) happens only in the
        # branch where the protected-base copy both downloaded and verified.
        self.assertRegex(
            reuse_block,
            re.compile(
                r'if ! python3 "\$RUNNER_TEMP/protected_merge_receipt.py" download .*'
                r'elif python3 "\$RUNNER_TEMP/protected_merge_receipt.py" verify [^\n]*(\n[^\n]*)*?'
                r'\n\s*matrix="\$\(MATRIX_JSON=.*macos_reused=true.*\n\s*else\n',
                re.DOTALL,
            ),
        )
        # The checked-out copy only renders decision notes; it never decides.
        workspace_calls = re.findall(
            r"python3 tools/scripts/protected_merge_receipt.py (\S+)", reuse_block)
        self.assertEqual(set(workspace_calls), {"note", "publish-notes"})
        self.assertIn("--summary", reuse_block)
        self.assertIn(
            "receipt reuse unavailable for ${target}; full validation retained",
            reuse_block,
        )
        self.assertIn('matrix="$ORIGINAL_MATRIX"', reuse_block)
        build = WORKFLOW.split("\n  build:", 1)[1].split(
            "\n  windows-msvc-release-gate:", 1
        )[0]
        self.assertIn("always()", build)
        self.assertIn("protected-receipt-reuse.result != 'success'", build)
        self.assertIn(
            "protected-receipt-reuse.outputs.matrix_json || needs.resolve-provider.outputs.matrix_json",
            build,
        )

    def test_required_macos_alias_needs_subject_bound_reuse(self) -> None:
        alias = WORKFLOW.split("\n  macos-merge-group:", 1)[1].split(
            "\n  linux:", 1
        )[0]
        self.assertIn("protected-receipt-reuse.outputs.macos_reused == 'true'", alias)
        # The verdict lives in the script the job runs.
        self.assertIn("run: bash tools/ci/macos_merge_group_bootstrap.sh", alias)
        verdict = (Path(__file__).parents[2] / "tools/ci/macos_merge_group_bootstrap.sh").read_text(
            encoding="utf-8"
        )
        self.assertIn("protected receipt decision unavailable", verdict)

    def test_receipts_are_only_published_after_successful_pr_validation(self) -> None:
        issue = WORKFLOW.split("- name: Issue exact protected-validation receipt", 1)[1].split(
            "\n      - name:", 1
        )[0]
        condition = issue.split("run: |", 1)[0]
        self.assertIn("github.event_name == 'pull_request'", condition)
        self.assertIn("success()", condition)
        # success() alone also holds when the Test step was skipped; the
        # receipt must require that the tests actually ran and passed.
        self.assertIn("steps.ctest.outcome == 'success'", condition)
        for flag in ("--ctest-exit-file", "--ctest-selection", "--ctest-selected-json", "--ctest-junit"):
            self.assertIn(flag, issue)

    def test_ctest_step_records_receipt_evidence(self) -> None:
        test_step = WORKFLOW.split("- name: Test (non-Windows)", 1)[1].split(
            "\n      - name:", 1
        )[0]
        self.assertIn("id: ctest", test_step)
        self.assertIn('"$evidence_dir/selection.json"', test_step)
        self.assertIn("ctest --show-only=json-v1 --test-dir", test_step)
        self.assertIn('"$evidence_dir/exit-code"', test_step)
        self.assertIn("--output-junit", test_step)
        self.assertIn("id: protected_receipt", WORKFLOW)
        self.assertIn("steps.protected_receipt.outcome == 'success'", WORKFLOW)
        self.assertIn("steps.protected_receipt.outputs.path", WORKFLOW)
        self.assertNotIn("steps.protected-receipt", WORKFLOW)
        self.assertIn("retention-days: 2", WORKFLOW)

    def test_receipt_uses_the_checkout_that_was_built_not_event_sha(self) -> None:
        issuer = WORKFLOW.split(
            "      - name: Issue exact protected-validation receipt", 1
        )[1].split("      - name: Publish exact protected-validation receipt", 1)[0]
        self.assertIn(
            "checkout_sha=\"$(git rev-parse --verify 'HEAD^{commit}')\"", issuer
        )
        self.assertIn('--checkout-sha "$checkout_sha"', issuer)
        self.assertNotIn('--checkout-sha "$GITHUB_SHA"', issuer)
        self.assertIn('--base-sha "$PR_BASE_SHA" --head-sha "$PR_HEAD_SHA"', issuer)


class LinuxRuntimeNodeProvisioningTest(unittest.TestCase):
    """The Linux matrix must provision Node before materialized-runtime npm ci."""

    _SUPPORTED_SETUP_NODE_USES = {
        "actions/setup-node@v4",
        "actions/setup-node@v7",
    }

    @classmethod
    def setUpClass(cls) -> None:
        cls.steps = _build_steps()
        cls.setup = cls.steps["Set up Node.js for materialized runtime tests (Linux)"]
        cls.install = cls.steps["Install materialized runtime Node test dependencies (Linux)"]

    def test_setup_node_is_linux_only_and_pinned(self) -> None:
        self.assertEqual(self.setup["if"], "runner.os == 'Linux'")
        self.assertIn(self.setup["uses"], self._SUPPORTED_SETUP_NODE_USES)
        self.assertEqual(self.setup["with"]["node-version"], "22.15.0")
        self.assertEqual(self.setup["with"]["cache"], "npm")
        self.assertEqual(
            self.setup["with"]["cache-dependency-path"],
            "tools/import-design/jsx-runtime/package-lock.json",
        )

    def test_runtime_install_is_linux_only_and_follows_provisioning(self) -> None:
        self.assertEqual(self.install["if"], "runner.os == 'Linux'")
        self.assertIn("npm ci --prefix tools/import-design/jsx-runtime", self.install["run"])
        names = [step.get("name") for step in _workflow()["jobs"]["build"]["steps"]]
        self.assertLess(names.index(self.setup["name"]), names.index(self.install["name"]))

    def test_runtime_install_is_present_for_macos(self) -> None:
        steps = _build_steps()
        install = steps["Install materialized runtime Node test dependencies (macOS)"]
        self.assertEqual(install["if"], "runner.os == 'macOS'")
        self.assertIn("npm ci --prefix tools/import-design/jsx-runtime", install["run"])

    def test_other_matrix_legs_do_not_claim_linux_runtime_install(self) -> None:
        self.assertNotIn("runner.os != 'Linux'", str(self.install["if"]))
        self.assertNotIn("npm ci --prefix tools/import-design/jsx-runtime", self.setup.get("run", ""))


class SanitizerRuntimeNodeProvisioningTest(unittest.TestCase):
    """ASan and UBSan provision JSX runtime dependencies before CTest."""

    _SUPPORTED_SETUP_NODE_USES = {
        "actions/setup-node@v4",
        "actions/setup-node@v7",
    }

    @classmethod
    def setUpClass(cls) -> None:
        cls.workflow = _sanitizers_workflow()

    def test_asan_and_ubsan_install_locked_runtime_dependencies_before_ctest(self) -> None:
        for job_name in ("asan", "ubsan"):
            with self.subTest(job=job_name):
                steps = self.workflow["jobs"][job_name]["steps"]
                names = [step.get("name") for step in steps]
                setup_name = "Set up Node.js for materialized runtime tests"
                install_name = "Install materialized runtime Node test dependencies"
                self.assertIn(setup_name, names)
                self.assertIn(install_name, names)
                setup = next(step for step in steps if step.get("name") == setup_name)
                install = next(step for step in steps if step.get("name") == install_name)
                self.assertIn(setup.get("uses"), self._SUPPORTED_SETUP_NODE_USES)
                self.assertEqual(setup["with"].get("node-version"), "22.15.0")
                self.assertEqual(setup["with"].get("cache"), "npm")
                self.assertEqual(
                    setup["with"].get("cache-dependency-path"),
                    "tools/import-design/jsx-runtime/package-lock.json",
                )
                self.assertIn(
                    "npm ci --prefix tools/import-design/jsx-runtime --no-audit --no-fund",
                    install.get("run", ""),
                )
                first_ctest = next(
                    index for index, step in enumerate(steps)
                    if "ctest" in str(step.get("run", ""))
                )
                self.assertLess(names.index(setup_name), first_ctest)
                self.assertLess(names.index(install_name), first_ctest)


def _build_steps() -> dict[str, dict[str, object]]:
    steps = _workflow()["jobs"]["build"]["steps"]
    return {step["name"]: step for step in steps if "name" in step}


def _evaluate_if(expression: object, context: dict[str, object]) -> bool:
    """Evaluate a GitHub Actions step condition over a small literal context."""
    return bool(_evaluate(expression, context))


def _evaluate(expression: object, context: dict[str, object]) -> object:
    """Evaluate a GitHub Actions expression over a small literal context.

    Returns the expression's value, so `a && 'x' || 'y'` yields the string a
    job `name:` would get.

    Supports exactly what these step conditions use: `&&`, `||`, `!`, `==`,
    `!=`, parentheses, single-quoted strings, dotted context names, and the
    status functions. An unknown name is '' (GitHub reads a missing step
    output as an empty string), so a condition that forgets a clause cannot
    pass by raising.
    """
    if isinstance(expression, bool):  # YAML reads a bare `true` as a bool
        return expression
    expression = expression.replace("${{", "").replace("}}", "")
    tokens = re.findall(
        r"\s+|&&|\|\||!=|==|!|\(|\)|'[^']*'|[A-Za-z_][A-Za-z0-9_.\-]*(?:\(\))?",
        expression)
    assert "".join(tokens) == expression, f"unparsed condition: {expression!r}"
    python = []
    for token in tokens:
        if token.isspace():
            python.append(" ")
        elif token in ("&&", "||", "!"):
            python.append({"&&": " and ", "||": " or ", "!": " not "}[token])
        elif token in ("==", "!=", "(", ")") or token.startswith("'"):
            python.append(token)
        elif token in ("true", "false"):
            python.append(repr(token == "true"))
        else:
            python.append(repr(context.get(token, "")))
    return eval("".join(python), {"__builtins__": {}})  # noqa: S307


class ReadyPullRequestReceiptTest(unittest.TestCase):
    """The receipt is issued from a ready head's full run, and from nothing else."""

    @classmethod
    def setUpClass(cls) -> None:
        steps = _build_steps()
        cls.decide = steps["Decide pull-request test suite"]
        cls.fast = steps["Test fast deterministic tier (pull request head)"]
        cls.full = steps["Test (non-Windows)"]
        cls.issue = steps["Issue exact protected-validation receipt"]
        cls.publish = steps["Publish exact protected-validation receipt"]

    def _context(self, event, suite="", ctest_outcome="", success=True, key="macos",
                 os_name="macOS"):
        return {
            "runner.os": os_name, "github.event_name": event, "matrix.key": key,
            "steps.pr_suite.outputs.suite": suite,
            "steps.ctest.outcome": ctest_outcome,
            "success()": success, "failure()": not success,
        }

    def test_ready_pull_request_runs_the_full_suite_as_evidence(self) -> None:
        ready = self._context("pull_request", suite="full")
        self.assertTrue(_evaluate_if(self.decide["if"], ready))
        self.assertTrue(_evaluate_if(self.fast["if"], ready))
        self.assertTrue(_evaluate_if(self.full["if"], ready))
        self.assertEqual(
            _evaluate_if(self.full["continue-on-error"], ready), False,
            "an armed head's full run must gate queue admission")

    def test_ordinary_pull_request_push_keeps_only_the_fast_tier(self) -> None:
        for suite in ("fast", ""):
            ordinary = self._context("pull_request", suite=suite)
            self.assertTrue(_evaluate_if(self.fast["if"], ordinary))
            self.assertFalse(_evaluate_if(self.full["if"], ordinary), suite)
            self.assertTrue(
                _evaluate_if(self.full["continue-on-error"], ordinary),
                "ordinary pull-request runs keep the skipped full step non-gating")

    def test_armed_full_suite_failure_is_not_downgraded_to_green(self) -> None:
        armed = self._context("pull_request", suite="full", ctest_outcome="failure")
        self.assertFalse(
            _evaluate_if(self.full["continue-on-error"], armed),
            "a failed armed full suite must make the required check red")

    def test_audited_ejected_heads_would_be_blocked(self) -> None:
        # Replay the six armed heads named by the fleet-completion audit. Their
        # full-suite evidence was red while the required check stayed green.
        for pr in (8933, 9036, 9283, 9577, 10084, 10115):
            with self.subTest(pr=pr):
                armed_failure = self._context(
                    "pull_request", suite="full", ctest_outcome="failure")
                self.assertFalse(
                    _evaluate_if(self.full["continue-on-error"], armed_failure),
                    f"audit PR #{pr} must be refused before queue admission")

    def test_merge_group_and_push_still_run_the_full_suite_as_the_gate(self) -> None:
        for event in ("merge_group", "push"):
            ctx = self._context(event)
            self.assertTrue(_evaluate_if(self.full["if"], ctx))
            self.assertFalse(_evaluate_if(self.fast["if"], ctx))
            self.assertFalse(_evaluate_if(self.full["continue-on-error"], ctx), event)

    def test_issuer_runs_only_after_a_ready_heads_full_suite_passed(self) -> None:
        passed = self._context("pull_request", suite="full", ctest_outcome="success")
        self.assertTrue(_evaluate_if(self.issue["if"], passed))
        self.assertTrue(_evaluate_if(self.issue["if"], {**passed, "matrix.key": "linux"}))
        refused = {
            "fast tier (Test skipped)": self._context(
                "pull_request", suite="fast", ctest_outcome="skipped"),
            "full suite failed (continue-on-error)": self._context(
                "pull_request", suite="full", ctest_outcome="failure"),
            "earlier step failed": self._context(
                "pull_request", suite="full", ctest_outcome="success", success=False),
            "merge group": self._context("merge_group", ctest_outcome="success"),
            "push": self._context("push", ctest_outcome="success"),
            "windows leg": self._context(
                "pull_request", suite="full", ctest_outcome="success", key="windows"),
        }
        for label, ctx in refused.items():
            self.assertFalse(_evaluate_if(self.issue["if"], ctx), label)

    def test_published_name_is_the_one_the_merge_group_looks_up(self) -> None:
        # The consumer's lookup is the exact-match binding: same target, the
        # PR head the group merged, and the base it merged onto. A receipt
        # published under any other name is simply never found.
        published = self.publish["with"]["name"]
        self.assertEqual(
            published,
            "protected-validation-${{ matrix.key }}-"
            "${{ github.event.pull_request.head.sha }}-"
            "${{ github.event.pull_request.base.sha }}",
        )
        self.assertIn(
            'artifact_name="protected-validation-${target}-${head}-${base}"', WORKFLOW)
        self.assertIn('read -r group base head extra <<< "$parents"', WORKFLOW)


class NoPrGateSettleTest(unittest.TestCase):
    """The PR gate settle window is gone and nothing waits on it.

    A superseding push that lands inside about five minutes cancels the run
    before its macOS leg claims a gate VM anyway, and pushes that cancel a
    claimed VM arrive many minutes apart, so a wait long enough to catch them
    costs every native pull-request run more latency than the VM time it
    saves. The measurement is in docs/guides/local-ci.md.
    """

    def test_no_settle_job_and_build_needs_only_the_preamble(self) -> None:
        jobs = _workflow()["jobs"]
        self.assertNotIn("pr-gate-settle", jobs)
        self.assertEqual(jobs["build"]["needs"], ["resolve-provider", "classify", "protected-receipt-reuse"])
        self.assertNotIn("PULP_PR_GATE_SETTLE_SECONDS", WORKFLOW)
        # A skipped `needs` entry (local_proof skips the reuse job) must not
        # skip the native matrix; the status function is what prevents it.
        self.assertIn("!cancelled()", " ".join(jobs["build"]["if"].split()))


class LocalProofWorkflowTest(unittest.TestCase):
    def setUp(self) -> None:
        self.workflow = _workflow()
        self.jobs = self.workflow["jobs"]

    def test_local_proof_is_default_off_and_dispatch_only(self) -> None:
        triggers = self.workflow.get("on", self.workflow.get(True, {}))
        local_proof = triggers["workflow_dispatch"]["inputs"]["local_proof"]
        self.assertIs(local_proof["default"], False)
        self.assertEqual(local_proof["type"], "boolean")

        job = self.jobs["local-proof"]
        condition = job["if"]
        self.assertIn("github.event_name == 'workflow_dispatch'", condition)
        self.assertIn("github.ref == 'refs/heads/main'", condition)
        self.assertIn("inputs.local_proof", condition)
        self.assertEqual(job["permissions"], {})

    def test_local_proof_requests_exact_protected_m1_assignment(self) -> None:
        runs_on = self.jobs["local-proof"]["runs-on"]
        self.assertEqual(runs_on["group"], "pulp-trusted-build")
        self.assertEqual(
            runs_on["labels"],
            [
                "self-hosted",
                "macOS",
                "ARM64",
                "pulp-build",
                "pulp-build-vm",
                "pulp-build-merge-group",
            ],
        )

    def test_local_proof_executes_no_repository_source_and_is_bounded(self) -> None:
        job = self.jobs["local-proof"]
        self.assertEqual(job["timeout-minutes"], 12)
        self.assertEqual(len(job["steps"]), 1)
        step = job["steps"][0]
        self.assertNotIn("uses", step)
        self.assertIn("sleep 600", step["run"])
        text = json.dumps(job, sort_keys=True)
        for forbidden in (
            "actions/checkout",
            "github.token",
            "GITHUB_TOKEN",
            "GH_TOKEN",
            "git ",
            "cmake",
            "python",
        ):
            self.assertNotIn(forbidden, text)

    def test_local_proof_has_a_unique_non_cancelling_concurrency_domain(self) -> None:
        concurrency = self.workflow["concurrency"]
        self.assertIn("inputs.local_proof", concurrency["group"])
        self.assertIn("github.run_id", concurrency["group"])
        self.assertIn("github.ref", concurrency["group"])
        self.assertIn("!inputs.local_proof", concurrency["cancel-in-progress"])

    def test_no_job_gates_itself_on_always(self) -> None:
        # `always()` runs a job even when the run has been CANCELLED, so a superseded
        # run keeps building and keeps holding its concurrency group. Every newer head
        # then waits at `pending`, which is indistinguishable from runner starvation,
        # and only a force-cancel (which bypasses `always()`) releases it.
        # `!cancelled()` buys the same thing the `always()` here was for: it still
        # evaluates when an upstream need failed or was skipped.
        offenders = {
            name: " ".join(str(job.get("if", "")).split())
            for name, job in self.jobs.items()
            if "always()" in str(job.get("if", ""))
        }
        self.assertEqual(
            offenders,
            {},
            "job-level `always()` keeps a cancelled run alive and holds the "
            f"concurrency group; use `!cancelled()`. offenders: {offenders}",
        )

    def test_the_always_scan_reaches_the_jobs(self) -> None:
        # Guards the scan above: an empty job map would pass it vacuously.
        self.assertGreater(len(self.jobs), 5)
        self.assertIn("build", self.jobs)
        self.assertEqual(
            {n for n, j in {"a": {"if": "always() && x"}, "b": {"if": "!cancelled()"}}.items()
             if "always()" in str(j.get("if", ""))},
            {"a"},
        )

    def test_cancellation_sensitive_jobs_still_run_on_upstream_failure(self) -> None:
        # `!cancelled()` must not regress the reason the gate was permissive: the
        # alias jobs report an outcome when an upstream need did NOT succeed.
        for name in ("macos", "linux", "windows"):
            with self.subTest(job=name):
                condition = " ".join(str(self.jobs[name].get("if", "")).split())
                self.assertIn("!cancelled()", condition)
                self.assertNotIn("always()", condition)

    def test_local_proof_structurally_suppresses_every_ordinary_job(self) -> None:
        ordinary = set(self.jobs) - {"local-proof"}
        self.assertTrue(ordinary)
        for name in sorted(ordinary):
            with self.subTest(job=name):
                condition = " ".join(str(self.jobs[name].get("if", "")).split())
                # Dispatch inputs are absent on pull_request/merge_group events.
                # The workflow therefore uses an event-aware equivalent guard;
                # retain acceptance of the original direct negation as well.
                self.assertTrue(
                    "!inputs.local_proof" in condition
                    or (
                        "inputs.local_proof != true" in condition
                        and "inputs.local_proof != 'true'" in condition
                    ),
                    condition,
                )


class CtestParallelismTest(unittest.TestCase):
    """The non-Windows ctest -j follows a declared tartci lease, capped at 8.

    Runs the derivation lines exactly as build.yml declares them, with a stub
    getconf standing in for the runner's core count. A runner that declares no
    lease keeps the prior -j8, so the change rolls out host by host.
    """

    @classmethod
    def setUpClass(cls) -> None:
        match = re.search(
            r'^( *ctest_jobs=8$.*?)^ *echo "ctest parallelism',
            WORKFLOW,
            re.DOTALL | re.MULTILINE,
        )
        assert match, "build.yml no longer derives ctest_jobs"
        cls.snippet = match.group(1) + 'echo "$ctest_jobs"\n'

    def _jobs(self, cores: str | None, guest: str | None = None) -> int:
        import os
        import subprocess
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            stub = Path(tmp) / "getconf"
            body = f"echo {cores}" if cores is not None else "exit 1"
            stub.write_text(f"#!/bin/bash\n{body}\n", encoding="utf-8")
            stub.chmod(0o755)
            env = {**os.environ, "PATH": f"{tmp}{os.pathsep}{os.environ['PATH']}"}
            env.pop("TARTCI_GUEST_CORES", None)
            if guest is not None:
                env["TARTCI_GUEST_CORES"] = guest
            proc = subprocess.run(
                ["bash", "-c", "set -euo pipefail\n" + self.snippet],
                env=env, capture_output=True, text=True, check=True,
            )
        return int(proc.stdout.strip())

    def test_undeclared_runner_keeps_the_prior_parallelism(self) -> None:
        self.assertEqual(self._jobs("3"), 8)
        self.assertEqual(self._jobs("28"), 8)

    def test_small_guest_is_not_oversubscribed(self) -> None:
        self.assertEqual(self._jobs("3", guest="3"), 3)
        self.assertEqual(self._jobs("6", guest="6"), 6)

    def test_large_guest_is_capped_at_eight(self) -> None:
        self.assertEqual(self._jobs("12", guest="12"), 8)

    def test_declared_lease_can_only_narrow(self) -> None:
        self.assertEqual(self._jobs("12", guest="3"), 3)
        self.assertEqual(self._jobs("6", guest="16"), 6)
        self.assertEqual(self._jobs(None, guest="3"), 3)

    def test_garbage_is_ignored(self) -> None:
        for bad in ("", "0", "abc", "-2"):
            with self.subTest(value=bad):
                self.assertEqual(self._jobs("6", guest=bad), 8)

    def test_non_windows_ctest_uses_the_derived_value(self) -> None:
        self.assertIn('-j"$ctest_jobs" --timeout 120', WORKFLOW)



class ArtifactUploadResilienceTest(unittest.TestCase):
    """An artifact-service reset after green tests must not red the gate."""

    SDK = "Upload exact GPU-audio SDK (macOS ARM64)"
    WAIT = "Wait before retrying the GPU-audio SDK upload"
    SDK_RETRY = "Upload exact GPU-audio SDK (macOS ARM64, retry)"
    CTEST_LOGS = "Upload ctest logs and JUnit report"

    @classmethod
    def setUpClass(cls) -> None:
        cls.steps = _build_steps()
        cls.order = [step.get("name") for step in _workflow()["jobs"]["build"]["steps"]]

    def test_ctest_log_upload_is_diagnostic_only(self) -> None:
        step = self.steps[self.CTEST_LOGS]
        self.assertIs(step.get("continue-on-error"), True)
        self.assertIn("always()", str(step["if"]))

    def test_sdk_upload_failure_is_retried_after_a_pause(self) -> None:
        first = self.steps[self.SDK]
        step_id = first.get("id")
        self.assertTrue(step_id, "the first SDK upload needs an id the retry can read")
        # Without continue-on-error a failed first attempt fails the job and the
        # retry, which runs only on success(), would never start.
        self.assertIs(first.get("continue-on-error"), True)
        for name in (self.WAIT, self.SDK_RETRY):
            with self.subTest(step=name):
                condition = self.steps[name]["if"]
                self.assertEqual(condition, f"steps.{step_id}.outcome == 'failure'")
                self.assertTrue(_evaluate_if(condition, {f"steps.{step_id}.outcome": "failure"}))
                for outcome in ("success", "skipped", ""):
                    self.assertFalse(
                        _evaluate_if(condition, {f"steps.{step_id}.outcome": outcome}))
        self.assertEqual(
            self.order.index(self.SDK) + 2, self.order.index(self.SDK_RETRY))
        self.assertEqual(self.order.index(self.SDK) + 1, self.order.index(self.WAIT))
        self.assertRegex(self.steps[self.WAIT]["run"], r"sleep [1-9][0-9]+")

    def test_sdk_retry_keeps_the_published_artifact_contract(self) -> None:
        first = self.steps[self.SDK]["with"]
        retry_step = self.steps[self.SDK_RETRY]
        retry = retry_step["with"]
        self.assertEqual(retry_step["uses"], self.steps[self.SDK]["uses"])
        for key in ("name", "path", "if-no-files-found", "retention-days", "compression-level"):
            with self.subTest(key=key):
                self.assertEqual(retry.get(key), first.get(key))
        self.assertIn("pulp-gpu-audio-sdk-${{ github.sha }}-", retry["name"])
        # A reset can land after the artifact record exists.
        self.assertIs(retry.get("overwrite"), True)
        # Consumers rely on the artifact: a job that never published it fails.
        self.assertNotIn("continue-on-error", retry_step)

    def test_unread_uploads_never_red_the_gate(self) -> None:
        # Nothing gates on these: the plugin bundles are a human download and
        # a lost iOS marker only costs a later rebuild.
        for name in ("Upload plugin bundles (macOS)", "Record iOS gate digest"):
            with self.subTest(step=name):
                self.assertIs(self.steps[name].get("continue-on-error"), True)

    def test_evidence_publishes_still_fail_the_job(self) -> None:
        # Receipts and verification evidence are read by merge decisions; a
        # job that did not publish them must not look green.
        for name in ("Publish exact protected-validation receipt",
                     "Publish A2T structural verification"):
            with self.subTest(step=name):
                self.assertNotIn("continue-on-error", self.steps[name])


class MacosApprovalGateTest(unittest.TestCase):
    """An unapproved PR head's macOS leg runs only its contract lane.

    Most pull-request heads never merge, and their full macOS runs were the
    largest share of the eight-VM fleet. Without `macos-ci` the leg configures,
    runs the pr-fast and pr-guard tiers (the PR-caused failures that otherwise
    eject merge-group batches) and stops; the build and the tests it feeds wait
    for the label the arm sweep applies to an approved head. Any other label
    starts a run that does nothing.
    """

    APP = "shipyard-local[bot]"
    NOOP = {"github.event_name": "pull_request", "github.event.action": "labeled",
            "github.event.label.name": "queue-unblock", "github.event.sender.login": APP,
            "vars.PULP_MACOS_PR_GATING": "true"}
    APPROVAL = {**NOOP, "github.event.label.name": "macos-ci"}
    CONTRACT = {"github.event_name": "pull_request", "runner.os": "macOS",
                "matrix.key": "macos", "matrix.lane": "contract",
                "runner.environment": "self-hosted",
                "needs.classify.outputs.agent_capability_installed_sdk_required": "true",
                "success()": True, "failure()": False}
    # Steps that need the build, or are the expensive tests it feeds.
    BUILD_FED = (
        "Build",
        "Decide pull-request test suite",
        "Test what this pull request reaches (pull request head)",
        "Test (non-Windows)",
        "Observe GPU adapter contract (self-hosted macOS)",
        "Test installed SDK capability contract (affected changes)",
        "Install and publish exact GPU-audio SDK (macOS ARM64)",
        "Upload exact GPU-audio SDK (macOS ARM64)",
        "Record per-test results for reuse replay (macOS)",
    )

    @classmethod
    def setUpClass(cls) -> None:
        cls.workflow = _workflow()
        cls.jobs = cls.workflow["jobs"]
        cls.triggers = cls.workflow.get("on", cls.workflow.get(True, {}))
        cls.steps = _build_steps()

    def test_labeled_joins_the_default_pull_request_types(self) -> None:
        self.assertEqual(self.triggers["pull_request"]["types"],
                         ["opened", "synchronize", "reopened", "labeled"])
        # merge_group and push keep every event; nothing about them is labelled.
        self.assertIsNone(self.triggers["merge_group"])
        self.assertEqual(self.triggers["push"], {"branches": ["main"]})

    def test_only_the_apps_approval_shares_the_pull_requests_concurrency_group(self) -> None:
        group = self.workflow["concurrency"]["group"]
        noop = "build-label-noop-42"
        for context, expected in (
                (self.APPROVAL, "build-refs/pull/1/merge"),
                (self.NOOP, noop),
                ({**self.APPROVAL, "github.event.sender.login": "a-maintainer"}, noop),
                ({**self.APPROVAL, "vars.PULP_MACOS_PR_GATING": ""}, noop),
                ({"github.event_name": "pull_request", "github.event.action": "synchronize"},
                 "build-refs/pull/1/merge")):
            with self.subTest(context=context):
                value = (group.replace("format('label-noop-{0}', github.run_id)", "'label-noop-42'")
                         .replace("format('local-proof-{0}', github.run_id)", "'local-proof-42'"))
                value = "build-" + str(_evaluate(value.split("build-", 1)[1],
                                                 {**context, "github.ref": "refs/pull/1/merge"}))
                self.assertEqual(value, expected)

    def test_another_label_skips_every_job_that_survives_a_skipped_need(self) -> None:
        # The roots skip outright. The aliases evaluate after a skipped need
        # (`!cancelled()`), so each needs the clause itself or it fails closed
        # on the head. Everything else needs a root and is skipped with it.
        skipped = {**self.NOOP, "needs.resolve-provider.result": "skipped",
                   "needs.classify.result": "skipped"}
        approval = {**skipped, "github.event.label.name": "macos-ci"}
        for name in ("resolve-provider", "classify", "macos", "linux", "windows"):
            with self.subTest(job=name):
                self.assertFalse(_evaluate_if(self.jobs[name]["if"], skipped))
                for untrusted in ({"github.event.sender.login": "a-maintainer"},
                                  {"vars.PULP_MACOS_PR_GATING": ""}):
                    self.assertFalse(_evaluate_if(self.jobs[name]["if"],
                                                  {**approval, **untrusted}))
        # The App's approval runs the preamble and the macOS alias; Linux and
        # Windows stay no-ops, because a label event's run is macOS-only.
        for name in ("resolve-provider", "classify", "macos"):
            with self.subTest(job=name):
                self.assertTrue(_evaluate_if(self.jobs[name]["if"], approval))
        for name in ("linux", "windows"):
            with self.subTest(job=name):
                self.assertFalse(_evaluate_if(self.jobs[name]["if"], approval))
        for name, job in self.jobs.items():
            if name in ("local-proof", "resolve-provider", "classify"):
                continue
            with self.subTest(job=name):
                self.assertTrue(job.get("needs"), f"{name} must hang off a root")

    def test_a_skipped_alias_never_carries_a_reporting_name(self) -> None:
        skipped = {**self.NOOP, "needs.resolve-provider.result": "skipped",
                   "needs.classify.result": "skipped"}
        for name, unused in (("macos", "macos-pr-unused"),
                             ("linux", "linux-label-noop"),
                             ("windows", "windows-label-noop")):
            with self.subTest(job=name):
                self.assertEqual(_evaluate(self.jobs[name]["name"], skipped), unused)
        self.assertEqual(_evaluate(self.jobs["linux"]["name"], {"github.event_name": "push"}),
                         "linux")

    def test_the_resolver_hands_the_lane_rule_its_inputs(self) -> None:
        env = self.jobs["resolve-provider"]["steps"][1]["env"]
        self.assertEqual(env["PULP_MACOS_PR_GATING"], "${{ vars.PULP_MACOS_PR_GATING || '' }}")
        self.assertEqual(env["PR_SENDER"], "${{ github.event.sender.login }}")
        self.assertEqual(env["PR_EVENT_ACTION"], "${{ github.event.action }}")
        self.assertIn('"macos_pr_lane.py"', self.jobs["resolve-provider"]["steps"][1]["run"])

    def test_the_contract_lane_skips_the_build_and_what_it_feeds(self) -> None:
        for name in self.BUILD_FED:
            with self.subTest(step=name):
                condition = self.steps[name].get("if")
                self.assertFalse(_evaluate_if(condition, self.CONTRACT))
        # Control: the full lane still builds on the same event.
        self.assertTrue(_evaluate_if(self.steps["Build"]["if"],
                                     {**self.CONTRACT, "matrix.lane": "full"}))

    def test_the_contract_lane_still_configures_and_runs_both_cheap_tiers(self) -> None:
        for name in ("Configure", "Test fast deterministic tier (pull request head)",
                     "Test merge-queue ejection guards (pull request head)"):
            with self.subTest(step=name):
                self.assertTrue(_evaluate_if(self.steps[name].get("if", True), self.CONTRACT))
        # The guard tier is macOS-only, so Linux keeps its current shape.
        self.assertFalse(_evaluate_if(
            self.steps["Test merge-queue ejection guards (pull request head)"]["if"],
            {**self.CONTRACT, "runner.os": "Linux", "matrix.key": "linux"}))

    def test_the_contract_lanes_green_is_marked_deferred(self) -> None:
        body = self.steps["Test fast deterministic tier (pull request head)"]["run"]
        self.assertIn('label_exclude="${label_exclude}|^needs-build$"', body)
        self.assertIn('"tier":"deferred"', body)
        self.assertIn('"tier":"fast"', body, "control: the full lane keeps its own tier")
        self.assertEqual(
            self.steps["Test fast deterministic tier (pull request head)"]["env"]["PULP_MACOS_LANE"],
            "${{ matrix.lane }}")

    def test_the_bootstrap_alias_never_claims_a_deferral(self) -> None:
        # The native leg owns `macos` on every native PR head; a second
        # `macos` from the bootstrap would collide with it.
        self.assertNotIn("deferred", json.dumps(self.jobs["macos"]))

    def test_the_label_reset_runs_from_main_and_checks_out_nothing(self) -> None:
        reset = yaml.safe_load((Path(__file__).parents[2]
                                / ".github/workflows/macos-ci-label-reset.yml").read_text(encoding="utf-8"))
        triggers = reset.get("on", reset.get(True, {}))
        self.assertEqual(list(triggers), ["pull_request_target"])
        self.assertEqual(triggers["pull_request_target"]["types"], ["synchronize"])
        self.assertEqual(reset["permissions"], {"pull-requests": "write"})
        text = json.dumps(reset)
        self.assertNotIn("actions/checkout", text)
        self.assertIn("labels/macos-ci", text)
        # Default off, like the lane itself.
        self.assertEqual(reset["jobs"]["reset"]["if"], "vars.PULP_MACOS_PR_GATING == 'true'")

    def test_a_push_disarms_an_armed_pull_request(self) -> None:
        # Otherwise the new head's contract-lane green would auto-merge it.
        reset = yaml.safe_load((Path(__file__).parents[2]
                                / ".github/workflows/macos-ci-label-reset.yml").read_text(encoding="utf-8"))
        steps = {step["name"]: step for step in reset["jobs"]["reset"]["steps"]}
        disarm = steps["Disable auto-merge"]
        self.assertEqual(disarm["if"], "github.event.pull_request.auto_merge != null")
        self.assertIn("disablePullRequestAutoMerge", disarm["run"])
        self.assertEqual(disarm["env"]["PR_NODE_ID"], "${{ github.event.pull_request.node_id }}")
        self.assertIn("!cancelled()", steps["Remove the label"]["if"])



# Answers the disarm step's `gh` calls from a JSON fixture and logs each call,
# so the step's own bash runs unchanged. Responses are what `--jq` would print.
FAKE_GH = """#!/usr/bin/env python3
import json, os, sys
args = sys.argv[1:]
fixture = json.load(open(os.environ["FAKE_GH_FIXTURE"], encoding="utf-8"))
with open(os.environ["FAKE_GH_LOG"], "a", encoding="utf-8") as log:
    log.write(json.dumps(args) + "\\n")
text = " ".join(args)
for key, answer in fixture.items():
    if key in text:
        sys.stdout.write(answer)
        break
"""


class MacosDeferredDisarmTest(unittest.TestCase):
    """A pull request armed before its checks ran is disarmed on a deferred green.

    The arm-side guard refuses to arm a `deferred` head, but a pull request
    armed on open has nothing for it to read yet, and native auto-merge would
    enqueue it on the contract lane's green.
    """

    PATH = Path(__file__).parents[2] / ".github/workflows/macos-ci-deferred-disarm.yml"
    HEAD = "a" * 40

    @classmethod
    def setUpClass(cls) -> None:
        cls.workflow = yaml.safe_load(cls.PATH.read_text(encoding="utf-8"))
        cls.job = cls.workflow["jobs"]["disarm"]
        cls.step = cls.job["steps"][0]

    def test_it_runs_from_main_on_completed_build_runs_and_checks_out_nothing(self) -> None:
        triggers = self.workflow.get("on", self.workflow.get(True, {}))
        self.assertEqual(triggers, {"workflow_run": {"workflows": ["Build and Test"],
                                                     "types": ["completed"]}})
        self.assertEqual(self.workflow["permissions"],
                         {"actions": "read", "checks": "read", "pull-requests": "write"})
        self.assertNotIn("actions/checkout", json.dumps(self.workflow))

    def test_it_is_default_off_and_pull_request_only(self) -> None:
        condition = self.job["if"]
        self.assertTrue(_evaluate_if(condition, {"vars.PULP_MACOS_PR_GATING": "true",
                                                 "github.event.workflow_run.event": "pull_request"}))
        for context in ({"vars.PULP_MACOS_PR_GATING": "", "github.event.workflow_run.event": "pull_request"},
                        {"vars.PULP_MACOS_PR_GATING": "true", "github.event.workflow_run.event": "merge_group"}):
            with self.subTest(context=context):
                self.assertFalse(_evaluate_if(condition, context))

    def _run(self, fixture: dict[str, str], run_prs: str = "7") -> list[list[str]]:
        with tempfile.TemporaryDirectory() as tmp:
            bin_dir = Path(tmp) / "bin"
            bin_dir.mkdir()
            gh = bin_dir / "gh"
            gh.write_text(FAKE_GH, encoding="utf-8")
            gh.chmod(0o755)
            (Path(tmp) / "fixture.json").write_text(json.dumps(fixture), encoding="utf-8")
            log = Path(tmp) / "log"
            log.write_text("", encoding="utf-8")
            env = {**os.environ, "PATH": f"{bin_dir}{os.pathsep}{os.environ['PATH']}",
                   "FAKE_GH_FIXTURE": str(Path(tmp) / "fixture.json"), "FAKE_GH_LOG": str(log),
                   "GITHUB_REPOSITORY": "Generous-Corp/pulp", "RUN_ID": "99",
                   "HEAD_SHA": self.HEAD, "RUN_PRS": run_prs, "GH_TOKEN": "x"}
            proc = subprocess.run(["bash", "-c", self.step["run"]], env=env, capture_output=True,
                                  text=True, encoding="utf-8", check=False)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            return [json.loads(line) for line in log.read_text(encoding="utf-8").splitlines()]

    def _fixture(self, tier: str, armed: str = "true", head: str = HEAD) -> dict[str, str]:
        return {
            "actions/runs/99/jobs": "123\n",
            "check-runs/123/annotations": '{"schema":"shipyard-test-tier/v1","tier":"%s"}\n' % tier,
            "commits/": "7\n",
            "mutation(": "",
            "query(": f"PR_NODE {head} {armed} false\n",
        }

    @staticmethod
    def _disarmed(calls: list[list[str]]) -> bool:
        return any("disablePullRequestAutoMerge" in " ".join(call) for call in calls)

    def test_an_armed_pull_request_on_a_deferred_green_is_disarmed(self) -> None:
        calls = self._run(self._fixture("deferred"))
        self.assertTrue(self._disarmed(calls))
        self.assertIn("id=PR_NODE", " ".join(" ".join(call) for call in calls))

    def test_an_app_opened_pull_request_is_found_through_its_commit(self) -> None:
        calls = self._run(self._fixture("deferred"), run_prs="")
        self.assertTrue(any("commits/" + self.HEAD + "/pulls" in " ".join(call) for call in calls))
        self.assertTrue(self._disarmed(calls))

    def test_nothing_is_disarmed_unless_deferred_armed_and_still_at_that_head(self) -> None:
        for label, fixture in (("full lane", self._fixture("fast")),
                               ("skip-safe", self._fixture("not-required")),
                               ("not armed", self._fixture("deferred", armed="false")),
                               ("moved on", self._fixture("deferred", head="b" * 40))):
            with self.subTest(case=label):
                self.assertFalse(self._disarmed(self._run(fixture)))

if __name__ == "__main__":
    unittest.main()

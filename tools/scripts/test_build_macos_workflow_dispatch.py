#!/usr/bin/env python3
"""Execute the PR resolver embedded in the macOS retarget workflow."""

from __future__ import annotations

import copy
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = REPO_ROOT / ".github" / "workflows" / "build-macos.yml"
RECONCILER = REPO_ROOT / ".github" / "workflows" / "build-macos-reconcile.yml"
MACOS_COMMAND = REPO_ROOT / "tools" / "cli" / "cmd_macos.cpp"
BASE_SHA = "a" * 40
HEAD_SHA = "b" * 40


def _pr(*, number: int = 7723, state: str = "open",
        base_repo: str = "Generous-Corp/pulp", base_ref: str = "main",
        head_repo: str = "Generous-Corp/pulp",
        head_ref: str = "repair/security-7723",
        base_sha: str = BASE_SHA, head_sha: str = HEAD_SHA) -> dict[str, object]:
    return {
        "number": number,
        "state": state,
        "base": {"sha": base_sha, "ref": base_ref,
                 "repo": {"full_name": base_repo}},
        "head": {"sha": head_sha, "ref": head_ref,
                 "repo": {"full_name": head_repo}},
    }


def _resolver_script() -> str:
    try:
        import yaml
    except ImportError:  # pragma: no cover - environment-dependent
        raise unittest.SkipTest("PyYAML not installed")
    document = yaml.safe_load(WORKFLOW.read_text(encoding="utf-8"))
    steps = document["jobs"]["resolve-runner"]["steps"]
    step = next((item for item in steps if item.get("id") == "pr"), None)
    if step is None:
        raise AssertionError("build-macos.yml lost the exact PR resolver")
    return step["run"]


def _runner_script() -> str:
    document = _workflow()
    steps = document["jobs"]["resolve-runner"]["steps"]
    step = next((item for item in steps if item.get("id") == "resolve"), None)
    if step is None:
        raise AssertionError("build-macos.yml lost the trusted runner resolver")
    return step["run"]


def _reporter_script() -> str:
    document = _workflow()
    steps = document["jobs"]["complete-macos-check"]["steps"]
    if len(steps) != 1:
        raise AssertionError("check completer must remain a single trusted step")
    return steps[0]["run"]


def _pending_script() -> str:
    document = _workflow()
    steps = document["jobs"]["publish-macos-pending"]["steps"]
    step = next((item for item in steps if item.get("id") == "publish"), None)
    if step is None:
        raise AssertionError("build-macos.yml lost the pending check publisher")
    return step["run"]


def _workflow() -> dict[str, object]:
    try:
        import yaml
    except ImportError:  # pragma: no cover - environment-dependent
        raise unittest.SkipTest("PyYAML not installed")
    return yaml.safe_load(WORKFLOW.read_text(encoding="utf-8"))


def _job_text(job_name: str) -> str:
    document = _workflow()
    return json.dumps(document["jobs"][job_name], sort_keys=True)


def _assert_trust_boundary(workflow: dict[str, object]) -> None:
    jobs = workflow["jobs"]
    build = jobs["build-test"]
    pending = jobs["publish-macos-pending"]
    reporter = jobs["complete-macos-check"]
    build_text = json.dumps(build, sort_keys=True)
    pending_text = json.dumps(pending, sort_keys=True)
    reporter_text = json.dumps(reporter, sort_keys=True)

    if workflow.get("permissions") != {}:
        raise AssertionError("workflow permissions must default-deny")
    if build.get("permissions") != {"contents": "read"}:
        raise AssertionError("untrusted build may receive contents-read only")
    for forbidden in (
        "actions/cache", "nscloud-cache-action", "statuses: write",
        "GH_TOKEN", "github.token", "~/Library/Caches",
    ):
        if forbidden in build_text:
            raise AssertionError(f"untrusted build exposes {forbidden}")
    for isolated in (
        "PULP_EPHEMERAL_ROOT", "PULP_BUILD_DIR",
        "PULP_SHARED_FETCHCONTENT_SOURCE_DIR", "PULP_SKIA_CACHE_ROOT",
        "PULP_UNTRUSTED_SOURCE", "PULP_UNTRUSTED_RUNNER",
        "PULP_EXPECTED_BASE", "refs/remotes/origin/main",
        "GITHUB_RUN_ID", "GITHUB_RUN_ATTEMPT", "PULP_USE_CCACHE=OFF",
    ):
        if isolated not in build_text:
            raise AssertionError(f"untrusted build lost isolation marker {isolated}")
    expected_controller_permissions = {
        "checks": "write", "pull-requests": "read"
    }
    for label, controller, text in (
        ("pending publisher", pending, pending_text),
        ("check completer", reporter, reporter_text),
    ):
        if controller.get("permissions") != expected_controller_permissions:
            raise AssertionError(f"{label} permissions drifted")
        if "actions/checkout" in text:
            raise AssertionError(f"privileged {label} must never check out source")
    if build.get("needs") != ["resolve-runner", "publish-macos-pending"]:
        raise AssertionError("untrusted build must wait for exact-head pending check")
    if "repos/$GITHUB_REPOSITORY/check-runs" not in pending_text:
        raise AssertionError("pending publisher must create a Checks API run")
    if "status=in_progress" not in pending_text:
        raise AssertionError("pending check must exist before untrusted execution")
    if "repos/$GITHUB_REPOSITORY/check-runs/$CHECK_RUN_ID" not in reporter_text:
        raise AssertionError("check completer must update the pending check run")
    if "needs.resolve-runner.outputs.head_sha" not in reporter_text:
        raise AssertionError("reporter must use trusted resolver head")
    for identity in (
        "needs.resolve-runner.outputs.base_sha",
        "needs.resolve-runner.outputs.head_ref",
        "live_base_repository", "live_base_ref",
        "live_head_repository", "live_head_ref",
    ):
        if identity not in reporter_text or identity not in pending_text:
            raise AssertionError(f"controller lost PR identity check {identity}")
    resolver_text = json.dumps(jobs["resolve-runner"], sort_keys=True)
    for marker in ("local retarget is disabled", "two-account Tart", "namespace-"):
        if marker not in resolver_text:
            raise AssertionError(f"runner resolver lost isolation marker {marker}")
    init = next(
        step for step in build["steps"]
        if step.get("name") == "Initialize isolated retarget paths"
    )["run"]
    for marker in (
        "exec sudo -u nobody /usr/bin/env -i",
        'HOME="$PULP_UNTRUSTED_HOME"',
        'TMPDIR="$PULP_UNTRUSTED_TMPDIR"',
    ):
        if marker not in init:
            raise AssertionError(f"untrusted account wrapper lost {marker}")
    for secret in (
        "ACTIONS_RUNTIME_TOKEN=", "ACTIONS_CACHE_URL=",
        "ACTIONS_RESULTS_URL=", "GITHUB_TOKEN=", "GH_TOKEN=",
        "GITHUB_ENV=", "GITHUB_PATH=", "HTTP_PROXY=", "HTTPS_PROXY=",
    ):
        if secret in init:
            raise AssertionError(f"untrusted wrapper passes protected variable {secret}")
    export = next(
        step for step in build["steps"]
        if step.get("name") == "Export exact head into the untrusted account"
    )
    export_text = json.dumps(export, sort_keys=True)
    for marker in (
        "needs.resolve-runner.outputs.base_sha",
        "PULP_EXPECTED_BASE",
    ):
        if marker not in export_text:
            raise AssertionError(f"isolated clone lost protected base marker {marker}")
    export_script = " ".join(export.get("run", "").split()).replace("\\ ", "")
    for marker in (
        "refs/heads/pulp-retarget-export-base",
        "refs/heads/pulp-retarget-export-head",
        "git clone --no-local --no-checkout",
        "sudo chown -R nobody",
        "update-ref refs/remotes/origin/main",
        "rev-parse refs/remotes/origin/main",
    ):
        if marker not in export_script:
            raise AssertionError(f"isolated clone lost protected base marker {marker}")
    pr_command_steps = {
        "Bootstrap repository dependencies", "Configure", "Build", "Test",
    }
    for step in build["steps"]:
        script = step.get("run", "")
        if step.get("name") in pr_command_steps and '"$PULP_UNTRUSTED_RUNNER"' not in script:
            raise AssertionError(
                f"PR-controlled command bypasses untrusted wrapper in {step.get('name')}"
            )


def _assert_reconciler(document: dict[str, object]) -> None:
    if document.get("permissions") != {}:
        raise AssertionError("reconciler permissions must default-deny")
    trigger = document.get("on", document.get(True, {}))
    if "workflow_run" not in trigger:
        raise AssertionError("reconciler lost workflow_run completion trigger")
    reconcile = document["jobs"]["reconcile"]
    if reconcile.get("permissions") != {"actions": "read", "checks": "write"}:
        raise AssertionError("reconciler permissions drifted")
    text = json.dumps(document)
    if "actions/checkout" in text:
        raise AssertionError("reconciler must never check out source")
    for marker in (
        "macos-retarget-check-", "check-owner.json", "external_id",
        "check-runs?check_name=macos", "status=completed",
    ):
        if marker not in text:
            raise AssertionError(f"reconciler lost ownership marker {marker}")


def _run(*, explicit: str = "7723", target_ref: str = "repair/security-7723",
         workflow_ref: str = "main",
         detail: dict[str, object] | None = None,
         matches: list[dict[str, object]] | None = None) -> tuple[int, dict[str, str], str]:
    detail = detail if detail is not None else _pr()
    matches = matches if matches is not None else [_pr()]
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        fake_bin = root / "bin"
        fake_bin.mkdir()
        fixtures = root / "fixtures"
        fixtures.mkdir()
        (fixtures / "detail.json").write_text(json.dumps(detail), encoding="utf-8")
        (fixtures / "matches.json").write_text(json.dumps(matches), encoding="utf-8")
        fake_gh = fake_bin / "gh"
        fake_gh.write_text(
            "#!/bin/sh\n"
            "case \"$*\" in\n"
            "  */pulls/[0-9]*) cat \"$FIXTURES/detail.json\" ;;\n"
            "  *) cat \"$FIXTURES/matches.json\" ;;\n"
            "esac\n",
            encoding="utf-8",
        )
        fake_gh.chmod(0o755)
        output = root / "output"
        output.write_text("", encoding="utf-8")
        env = {key: value for key, value in os.environ.items()
               if not key.startswith("GITHUB_")}
        env.update({
            "PATH": f"{fake_bin}:{os.environ['PATH']}",
            "FIXTURES": str(fixtures),
            "GITHUB_OUTPUT": str(output),
            "GITHUB_REPOSITORY": "Generous-Corp/pulp",
            "GITHUB_REPOSITORY_OWNER": "Generous-Corp",
            "INPUT_PR_NUMBER": explicit,
            "TARGET_REF": target_ref,
            "WORKFLOW_REF": workflow_ref,
            "GH_TOKEN": "test-token",
        })
        process = subprocess.run(
            ["bash", "-c", _resolver_script()], cwd=root, env=env,
            capture_output=True, text=True,
        )
        parsed = dict(
            line.split("=", 1) for line in output.read_text().splitlines()
            if "=" in line
        )
        return process.returncode, parsed, process.stderr


def _run_runner(*, choice: str = "github-hosted",
                local: str = '["self-hosted","macOS","ARM64","pulp-build","pulp-build-vm","pulp-gate-fast"]',
                namespace: str = '"namespace-profile-generouscorp-macos"',
                ) -> tuple[int, dict[str, str], str]:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        output = root / "output"
        output.write_text("", encoding="utf-8")
        env = os.environ.copy()
        env.update({
            "GITHUB_OUTPUT": str(output),
            "RUNNER_CHOICE": choice,
            "LOCAL_MACOS_RUNS_ON_JSON": local,
            "NAMESPACE_MACOS_RUNS_ON_JSON": namespace,
        })
        process = subprocess.run(
            ["bash", "-c", _runner_script()], cwd=root, env=env,
            capture_output=True, text=True,
        )
        parsed = dict(
            line.split("=", 1) for line in output.read_text().splitlines()
            if "=" in line
        )
        return process.returncode, parsed, process.stderr


def _run_reporter(*, detail: dict[str, object] | None = None,
                  expected_base: str = BASE_SHA,
                  expected_head: str = HEAD_SHA,
                  expected_head_ref: str = "repair/security-7723",
                  ) -> tuple[int, bool, str]:
    detail = detail if detail is not None else _pr()
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        fake_bin = root / "bin"
        fake_bin.mkdir()
        fixture = root / "detail.json"
        fixture.write_text(json.dumps(detail), encoding="utf-8")
        posted = root / "posted"
        fake_gh = fake_bin / "gh"
        fake_gh.write_text(
            "#!/bin/sh\n"
            "case \" $* \" in\n"
            "  *\" --method POST \"*) printf '%s\\n' \"$*\" > \"$POSTED\" ;;\n"
            "  *\" --method PATCH \"*) printf '%s\\n' \"$*\" > \"$POSTED\" ;;\n"
            "  *) cat \"$FIXTURE\" ;;\n"
            "esac\n",
            encoding="utf-8",
        )
        fake_gh.chmod(0o755)
        env = os.environ.copy()
        env.update({
            "PATH": f"{fake_bin}:{os.environ['PATH']}",
            "FIXTURE": str(fixture),
            "POSTED": str(posted),
            "GITHUB_REPOSITORY": "Generous-Corp/pulp",
            "EXPECTED_PR": "7723",
            "EXPECTED_BASE": expected_base,
            "EXPECTED_HEAD": expected_head,
            "EXPECTED_HEAD_REF": expected_head_ref,
            "CHECK_RUN_ID": "12345",
            "CHECK_EXTERNAL_ID": "retarget-1-1",
            "BUILD_RESULT": "success",
            "DETAILS_URL": "https://example.invalid/run/1",
            "GH_TOKEN": "test-token",
        })
        process = subprocess.run(
            ["bash", "-c", _reporter_script()], cwd=root, env=env,
            capture_output=True, text=True,
        )
        return process.returncode, posted.exists(), process.stderr


def _run_pending(*, detail: dict[str, object] | None = None,
                 ) -> tuple[int, bool, str]:
    detail = detail if detail is not None else _pr()
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        fake_bin = root / "bin"
        fake_bin.mkdir()
        fixture = root / "detail.json"
        fixture.write_text(json.dumps(detail), encoding="utf-8")
        posted = root / "posted"
        fake_gh = fake_bin / "gh"
        fake_gh.write_text(
            "#!/bin/sh\n"
            "case \" $* \" in\n"
            "  *\" --method POST \"*) printf '%s\\n' \"$*\" > \"$POSTED\"; printf '12345\\n' ;;\n"
            "  *) cat \"$FIXTURE\" ;;\n"
            "esac\n",
            encoding="utf-8",
        )
        fake_gh.chmod(0o755)
        output = root / "output"
        output.write_text("", encoding="utf-8")
        env = os.environ.copy()
        env.update({
            "PATH": f"{fake_bin}:{os.environ['PATH']}",
            "FIXTURE": str(fixture), "POSTED": str(posted),
            "GITHUB_OUTPUT": str(output),
            "GITHUB_REPOSITORY": "Generous-Corp/pulp",
            "EXPECTED_PR": "7723", "EXPECTED_BASE": BASE_SHA,
            "EXPECTED_HEAD": HEAD_SHA,
            "EXPECTED_HEAD_REF": "repair/security-7723",
            "CHECK_EXTERNAL_ID": "retarget-1-1",
            "DETAILS_URL": "https://example.invalid/run/1",
            "GH_TOKEN": "test-token",
        })
        process = subprocess.run(
            ["bash", "-c", _pending_script()], cwd=root, env=env,
            capture_output=True, text=True,
        )
        return process.returncode, posted.exists(), process.stderr


class BuildMacosWorkflowDispatchTests(unittest.TestCase):
    def test_explicit_pr_pins_exact_event_base_and_head(self) -> None:
        rc, output, _ = _run()
        self.assertEqual(rc, 0)
        self.assertEqual(output, {
            "number": "7723", "base_sha": BASE_SHA, "head_sha": HEAD_SHA,
            "head_ref": "repair/security-7723",
        })

    def test_concurrency_uses_the_required_canonical_pr_ref(self) -> None:
        self.assertEqual(
            _workflow()["concurrency"]["group"],
            "build-macos-${{ inputs.target_ref }}",
        )
        self.assertEqual(
            _workflow()[True]["workflow_dispatch"]["inputs"]["target_ref"]["required"],
            True,
        )

    def test_local_route_fails_closed_even_for_current_jit_selector(self) -> None:
        selectors = (
            '["self-hosted","macOS","ARM64","pulp-build","pulp-build-vm","pulp-gate-fast"]',
            '["self-hosted","sanitizer"]', '"macos-15"', "", "not-json",
        )
        for selector in selectors:
            with self.subTest(selector=selector):
                rc, output, error = _run_runner(choice="local", local=selector)
                self.assertEqual(rc, 1)
                self.assertEqual(output, {})
                self.assertIn("two-account Tart runner class", error)

    def test_namespace_route_cannot_be_redirected_to_self_hosted(self) -> None:
        rc, output, _ = _run_runner(choice="namespace")
        self.assertEqual(rc, 0)
        self.assertEqual(output["route"], "namespace")
        for selector in (
            '["self-hosted","macOS"]', '"namespace-persistent"',
            '"macos-15"', "", "not-json"
        ):
            with self.subTest(selector=selector):
                rc, output, error = _run_runner(
                    choice="namespace", namespace=selector,
                )
                self.assertEqual(rc, 1)
                self.assertEqual(output, {})
                self.assertTrue(
                    "exact approved Namespace profile" in error
                    or "is not set" in error
                )

    def test_pending_check_is_published_before_untrusted_build(self) -> None:
        rc, posted, _ = _run_pending()
        self.assertEqual(rc, 0)
        self.assertTrue(posted)
        workflow = _workflow()
        self.assertEqual(
            workflow["jobs"]["build-test"]["needs"],
            ["resolve-runner", "publish-macos-pending"],
        )
        pending = _job_text("publish-macos-pending")
        self.assertIn("status=in_progress", pending)
        self.assertIn("external_id", pending)
        self.assertIn("terminalize_orphan_on_exit", pending)
        self.assertIn("checks", pending)

    def test_protected_reconciler_owns_cancelled_pending_checks(self) -> None:
        import yaml
        document = yaml.safe_load(RECONCILER.read_text(encoding="utf-8"))
        _assert_reconciler(document)
        pending = _job_text("publish-macos-pending")
        upload = pending.index("actions/upload-artifact@v4")
        create = pending.index("--raw-field name=macos")
        self.assertLess(upload, create)

    def test_hostile_reconciler_mutations_are_rejected(self) -> None:
        import yaml
        source = yaml.safe_load(RECONCILER.read_text(encoding="utf-8"))
        cases: list[tuple[str, callable]] = [
            (
                "reconciler checks out source",
                lambda doc: doc["jobs"]["reconcile"]["steps"].insert(
                    0, {"uses": "actions/checkout@v5"}
                ),
            ),
            (
                "reconciler loses checks permission",
                lambda doc: doc["jobs"]["reconcile"].update(
                    {"permissions": {"actions": "read"}}
                ),
            ),
            (
                "reconciler accepts an input-selected check",
                lambda doc: doc["jobs"]["reconcile"]["steps"][0].update(
                    {"run": "gh api repos/$GITHUB_REPOSITORY/check-runs/123"}
                ),
            ),
        ]
        for label, mutate in cases:
            with self.subTest(label=label):
                document = copy.deepcopy(source)
                mutate(document)
                with self.assertRaises(AssertionError):
                    _assert_reconciler(document)

    def test_reporter_posts_only_for_unchanged_complete_pr_identity(self) -> None:
        rc, posted, _ = _run_reporter()
        self.assertEqual(rc, 0)
        self.assertTrue(posted)
        hostile = (
            _pr(state="closed"),
            _pr(base_sha="c" * 40),
            _pr(base_repo="attacker/pulp"),
            _pr(base_ref="develop"),
            _pr(head_sha="d" * 40),
            _pr(head_repo="attacker/pulp"),
            _pr(head_ref="replacement"),
        )
        for detail in hostile:
            with self.subTest(detail=detail):
                rc, posted, error = _run_reporter(detail=detail)
                self.assertEqual(rc, 1)
                self.assertTrue(posted)
                self.assertIn("identity changed", error)

    def test_target_ref_resolves_one_open_pr_when_number_is_omitted(self) -> None:
        rc, output, _ = _run(explicit="")
        self.assertEqual(rc, 0)
        self.assertEqual(output["number"], "7723")

    def test_target_ref_is_required_for_every_dispatch_form(self) -> None:
        rc, output, error = _run(target_ref="")
        self.assertEqual(rc, 1)
        self.assertEqual(output, {})
        self.assertIn("target_ref is required to resolve an exact pull request", error)

    def test_target_ref_must_resolve_exactly_one_pr(self) -> None:
        for matches in ([], [_pr(number=1), _pr(number=2)]):
            with self.subTest(count=len(matches)):
                rc, output, error = _run(explicit="", matches=matches)
                self.assertEqual(rc, 1)
                self.assertEqual(output, {})
                self.assertIn("expected exactly one", error)

    def test_closed_pr_is_rejected(self) -> None:
        rc, output, error = _run(detail=_pr(state="closed"))
        self.assertEqual(rc, 1)
        self.assertEqual(output, {})
        self.assertIn("closed", error)

    def test_untrusted_base_is_rejected(self) -> None:
        for detail in (_pr(base_repo="attacker/pulp"), _pr(base_ref="develop")):
            with self.subTest(base=detail["base"]):
                rc, output, error = _run(detail=detail)
                self.assertEqual(rc, 1)
                self.assertEqual(output, {})
                self.assertIn("expected Generous-Corp/pulp:main", error)

    def test_wrong_head_repository_or_ref_is_rejected(self) -> None:
        for detail in (_pr(head_repo="attacker/pulp"), _pr(head_ref="other")):
            with self.subTest(head=detail["head"]):
                rc, output, error = _run(detail=detail)
                self.assertEqual(rc, 1)
                self.assertEqual(output, {})
                self.assertIn("expected Generous-Corp/pulp:repair/security-7723", error)

    def test_non_numeric_pr_input_is_rejected_before_api_lookup(self) -> None:
        rc, output, error = _run(explicit="7723/merge")
        self.assertEqual(rc, 1)
        self.assertEqual(output, {})
        self.assertIn("decimal digits", error)

    def test_non_full_sha_is_rejected(self) -> None:
        rc, output, error = _run(detail=_pr(base_sha="abc"))
        self.assertEqual(rc, 1)
        self.assertEqual(output, {})
        self.assertIn("40-character", error)

    def test_non_main_workflow_definition_is_rejected(self) -> None:
        rc, output, error = _run(workflow_ref="repair/security-7723")
        self.assertEqual(rc, 1)
        self.assertEqual(output, {})
        self.assertIn("protected main", error)

    def test_cli_dispatches_the_trusted_workflow_with_explicit_pr_identity(self) -> None:
        source = MACOS_COMMAND.read_text(encoding="utf-8")
        self.assertIn('"--ref", "main"', source)
        self.assertIn('"--field", "pr_number=" + pr_number', source)
        self.assertNotIn('"--ref", head_ref', source)
        refusal = source.index('if (runner == "local")')
        cancellation = source.index("cancel_in_flight_macos(pr_number)", refusal)
        self.assertLess(refusal, cancellation)
        self.assertIn("two-account Tart runner class is proven", source)

    def test_untrusted_build_has_no_persistent_cache_or_privileged_token(self) -> None:
        workflow = _workflow()
        _assert_trust_boundary(workflow)
        build = workflow["jobs"]["build-test"]
        text = _job_text("build-test")
        for forbidden in (
            "actions/cache", "nscloud-cache-action", "statuses: write",
            "GH_TOKEN", "github.token", "~/Library/Caches",
        ):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, text)
        for isolated in (
            "PULP_EPHEMERAL_ROOT", "PULP_BUILD_DIR",
            "PULP_SHARED_FETCHCONTENT_SOURCE_DIR", "PULP_SKIA_CACHE_ROOT",
            "PULP_UNTRUSTED_SOURCE", "PULP_UNTRUSTED_RUNNER",
            "GITHUB_RUN_ID", "GITHUB_RUN_ATTEMPT", "PULP_USE_CCACHE=OFF",
        ):
            with self.subTest(isolated=isolated):
                self.assertIn(isolated, text)
        checkout = next(
            step for step in build["steps"]
            if step.get("uses") == "actions/checkout@v5"
        )
        self.assertEqual(checkout["with"]["clean"], True)
        self.assertEqual(checkout["with"]["persist-credentials"], False)

    def test_privileged_reporter_never_checks_out_or_executes_pr_code(self) -> None:
        workflow = _workflow()
        reporter = workflow["jobs"]["complete-macos-check"]
        self.assertEqual(
            reporter["permissions"],
            {"checks": "write", "pull-requests": "read"},
        )
        self.assertEqual(
            reporter["needs"],
            ["resolve-runner", "publish-macos-pending", "build-test"],
        )
        text = _job_text("complete-macos-check")
        script = reporter["steps"][0]["run"]
        self.assertNotIn("actions/checkout", text)
        self.assertIn("EXPECTED_BASE", text)
        self.assertIn("EXPECTED_HEAD_REF", text)
        self.assertIn("needs.resolve-runner.outputs.head_sha", text)
        self.assertIn("needs.build-test.result", text)
        self.assertIn("check-runs/$CHECK_RUN_ID", text)
        self.assertIn("--raw-field status=completed", text)
        self.assertIn("live_head", text)
        self.assertIn("live_base_repository", text)
        self.assertIn("live_head_repository", text)
        self.assertIn("publish_failure_on_exit", script)
        self.assertIn("trap publish_failure_on_exit EXIT", script)
        self.assertIn("resolve_check_run_id", script)
        self.assertIn("patch_terminal_check", script)
        self.assertIn("for delay in 1 2 4", script)
        self.assertIn('[ "$conclusion" = success ] || exit 1', script)
        cleanup = next(
            step for step in workflow["jobs"]["build-test"]["steps"]
            if step.get("name") == "Remove untrusted writable state"
        )["run"]
        self.assertIn("sudo /bin/rm -rf", cleanup)

    def test_only_trusted_jobs_receive_elevated_permissions(self) -> None:
        jobs = _workflow()["jobs"]
        self.assertEqual(
            jobs["resolve-runner"]["permissions"],
            {"contents": "read", "pull-requests": "read"},
        )
        self.assertEqual(jobs["build-test"]["permissions"], {"contents": "read"})
        self.assertEqual(
            jobs["publish-macos-pending"]["permissions"],
            {"checks": "write", "pull-requests": "read"},
        )
        self.assertEqual(
            jobs["complete-macos-check"]["permissions"],
            {"checks": "write", "pull-requests": "read"},
        )

    def test_hostile_trust_boundary_mutations_are_rejected(self) -> None:
        cases: list[tuple[str, callable]] = [
            (
                "cache action in untrusted build",
                lambda doc: doc["jobs"]["build-test"]["steps"].append(
                    {"uses": "actions/cache/restore@v4"}
                ),
            ),
            (
                "checks write in untrusted build",
                lambda doc: doc["jobs"]["build-test"].update(
                    {"permissions": {"contents": "read", "statuses": "write"}}
                ),
            ),
            (
                "token exposed to untrusted build",
                lambda doc: doc["jobs"]["build-test"].setdefault("env", {}).update(
                    {"GH_TOKEN": "${{ github.token }}"}
                ),
            ),
            (
                "privileged reporter checks out source",
                lambda doc: doc["jobs"]["complete-macos-check"]["steps"].insert(
                    0, {"uses": "actions/checkout@v5"}
                ),
            ),
            (
                "privileged reporter stops checking the immutable base",
                lambda doc: doc["jobs"]["complete-macos-check"]["steps"][0].update(
                    {"env": {
                        key: value for key, value in
                        doc["jobs"]["complete-macos-check"]["steps"][0]["env"].items()
                        if key != "EXPECTED_BASE"
                    }}
                ),
            ),
            (
                "untrusted build starts before pending check",
                lambda doc: doc["jobs"]["build-test"].update(
                    {"needs": ["resolve-runner"]}
                ),
            ),
            (
                "local route stops failing closed",
                lambda doc: doc["jobs"]["resolve-runner"]["steps"][0].update(
                    {"run": "echo runs_on_json='[\"self-hosted\"]' >> $GITHUB_OUTPUT; echo 'local retarget is disabled two-account Tart'"}
                ),
            ),
            (
                "PR code inherits protected runner environment",
                lambda doc: next(
                    step for step in doc["jobs"]["build-test"]["steps"]
                    if step.get("name") == "Initialize isolated retarget paths"
                ).update({"run": "exec sudo -u nobody env"}),
            ),
            (
                "PR setup bypasses untrusted account wrapper",
                lambda doc: next(
                    step for step in doc["jobs"]["build-test"]["steps"]
                    if step.get("name") == "Bootstrap repository dependencies"
                ).update({"run": "./setup.sh --ci --deps-only"}),
            ),
            (
                "isolated clone loses the immutable main base",
                lambda doc: next(
                    step for step in doc["jobs"]["build-test"]["steps"]
                    if step.get("name") == "Export exact head into the untrusted account"
                ).update({"run": "git clone --shared --no-checkout trusted untrusted"}),
            ),
        ]
        for label, mutate in cases:
            with self.subTest(label=label):
                document = copy.deepcopy(_workflow())
                mutate(document)
                with self.assertRaises(AssertionError):
                    _assert_trust_boundary(document)

    def test_pinned_chrome_is_checksum_verified_inside_ephemeral_root(self) -> None:
        text = _job_text("build-test")
        self.assertIn("151.0.7922.47", text)
        self.assertIn(
            "9529990b6afd9867a862c7a5bff2a4a8eef84614d910acac22e4c5fa5c24daee",
            text,
        )
        self.assertIn("$PULP_EPHEMERAL_ROOT/chrome-for-testing", text)
        self.assertIn("Chrome archive SHA-256 mismatch", text)


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Focused runtime and workflow-structure tests for Linux runner routing."""

from __future__ import annotations

import importlib.util
import json
import re
import subprocess
import sys
from pathlib import Path


SCRIPT = Path(__file__).parent / "resolve_linux_route.py"
REPO_ROOT = Path(__file__).resolve().parents[2]
BUILD_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "build.yml"
PR_SAFE_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "pr-safe-linux.yml"
VELLUM_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "vellum-freeze-check.yml"
VERSION_SKILL_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "version-skill-check.yml"
MAC_PRO_SELECTOR = (
    '["self-hosted","Linux","X64","pulp-build-linux-x64",'
    '"pulp-host-macpro","pulp-auto-linux-x64"]'
)

_SPEC = importlib.util.spec_from_file_location("linux_route_under_test", SCRIPT)
route = importlib.util.module_from_spec(_SPEC)
assert _SPEC.loader is not None
_SPEC.loader.exec_module(route)


def test_dispatch_uses_configured_self_hosted_selector() -> None:
    metadata = route.resolve_route(
        event_name="workflow_dispatch",
        dispatch_selector_json="",
        configured_selector_json=MAC_PRO_SELECTOR,
        authorized_selector_json=MAC_PRO_SELECTOR,
        resolved_selector_json=MAC_PRO_SELECTOR,
    )
    assert metadata["linux_route_reason"] == "explicit-dispatch"
    assert metadata["linux_provider"] == "local"
    assert json.loads(metadata["configured_selector_json"])[-1] == "pulp-auto-linux-x64"
    assert metadata["event_authorized_selector_json"] == metadata["configured_selector_json"]


def test_pull_request_exposes_security_hosted_route() -> None:
    metadata = route.resolve_route(
        event_name="pull_request",
        dispatch_selector_json="",
        configured_selector_json=MAC_PRO_SELECTOR,
        authorized_selector_json="",
        resolved_selector_json='"ubuntu-latest"',
    )
    assert metadata["linux_route_reason"] == "security-hosted"
    assert metadata["linux_provider"] == "github-hosted"
    assert metadata["configured_selector_json"]
    assert metadata["event_authorized_selector_json"] == ""


def test_same_repository_pull_request_can_use_trusted_local_route() -> None:
    metadata = route.resolve_route(
        event_name="pull_request",
        dispatch_selector_json="",
        configured_selector_json=MAC_PRO_SELECTOR,
        authorized_selector_json=MAC_PRO_SELECTOR,
        resolved_selector_json=MAC_PRO_SELECTOR,
    )
    assert metadata["linux_route_reason"] == "trusted-local"
    assert metadata["linux_provider"] == "local"


def test_merge_group_can_use_trusted_local_route() -> None:
    metadata = route.resolve_route(
        event_name="merge_group",
        dispatch_selector_json="",
        configured_selector_json=MAC_PRO_SELECTOR,
        authorized_selector_json=MAC_PRO_SELECTOR,
        resolved_selector_json=MAC_PRO_SELECTOR,
    )
    assert metadata["linux_route_reason"] == "trusted-local"
    assert metadata["linux_provider"] == "local"


def test_unconfigured_event_exposes_hosted_route() -> None:
    metadata = route.resolve_route(
        event_name="workflow_dispatch",
        dispatch_selector_json="",
        configured_selector_json="",
        authorized_selector_json="",
        resolved_selector_json='"ubuntu-latest"',
    )
    assert metadata["linux_route_reason"] == "unconfigured-hosted"
    assert metadata["linux_provider"] == "github-hosted"


def test_dispatch_override_can_explicitly_select_hosted() -> None:
    metadata = route.resolve_route(
        event_name="workflow_dispatch",
        dispatch_selector_json='"ubuntu-24.04"',
        configured_selector_json=MAC_PRO_SELECTOR,
        authorized_selector_json='"ubuntu-24.04"',
        resolved_selector_json='"ubuntu-24.04"',
    )
    assert metadata["linux_route_reason"] == "explicit-dispatch"
    assert metadata["linux_provider"] == "github-hosted"


def test_bare_label_dispatch_override_remains_supported() -> None:
    metadata = route.resolve_route(
        event_name="workflow_dispatch",
        dispatch_selector_json="ubuntu-24.04",
        configured_selector_json="",
        authorized_selector_json="ubuntu-24.04",
        resolved_selector_json='"ubuntu-24.04"',
    )
    assert metadata["linux_provider"] == "github-hosted"
    assert metadata["event_authorized_selector_json"] == '"ubuntu-24.04"'


def test_configured_custom_only_label_is_authoritatively_local() -> None:
    metadata = route.resolve_route(
        event_name="workflow_dispatch",
        dispatch_selector_json="",
        configured_selector_json="pulp-macpro-custom-only",
        authorized_selector_json="pulp-macpro-custom-only",
        resolved_selector_json='"pulp-macpro-custom-only"',
    )
    assert metadata["linux_provider"] == "local"


def test_configured_hosted_looking_custom_label_is_still_local() -> None:
    metadata = route.resolve_route(
        event_name="workflow_dispatch",
        dispatch_selector_json="",
        configured_selector_json="ubuntu-pulp-macpro",
        authorized_selector_json="ubuntu-pulp-macpro",
        resolved_selector_json='"ubuntu-pulp-macpro"',
    )
    assert metadata["linux_provider"] == "local"


def test_ambiguous_operator_label_is_reported_as_operator() -> None:
    metadata = route.resolve_route(
        event_name="workflow_dispatch",
        dispatch_selector_json="operator-selected-label",
        configured_selector_json="",
        authorized_selector_json="operator-selected-label",
        resolved_selector_json='"operator-selected-label"',
    )
    assert metadata["linux_provider"] == "operator"


def test_version_shaped_custom_label_is_reported_as_operator() -> None:
    metadata = route.resolve_route(
        event_name="workflow_dispatch",
        dispatch_selector_json="ubuntu-99.99",
        configured_selector_json="",
        authorized_selector_json="ubuntu-99.99",
        resolved_selector_json='"ubuntu-99.99"',
    )
    assert metadata["linux_provider"] == "operator"


def test_configured_dispatch_fails_if_it_resolves_hosted() -> None:
    proc = subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "--event-name", "workflow_dispatch",
            "--configured-selector-json", MAC_PRO_SELECTOR,
            "--authorized-selector-json", MAC_PRO_SELECTOR,
            "--resolved-selector-json", '"ubuntu-latest"',
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    assert proc.returncode == 1
    assert "unexpectedly resolved" in proc.stderr


def test_configured_dispatch_rejects_hosted_configuration() -> None:
    proc = subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "--event-name", "workflow_dispatch",
            "--configured-selector-json", '"ubuntu-latest"',
            "--authorized-selector-json", '"ubuntu-latest"',
            "--resolved-selector-json", '"ubuntu-latest"',
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    assert proc.returncode == 1
    assert "unexpectedly resolved" in proc.stderr


def test_non_dispatch_cannot_authorize_configured_selector() -> None:
    try:
        route.resolve_route(
            event_name="pull_request_target",
            dispatch_selector_json="",
            configured_selector_json=MAC_PRO_SELECTOR,
            authorized_selector_json=MAC_PRO_SELECTOR,
            resolved_selector_json=MAC_PRO_SELECTOR,
        )
    except ValueError as exc:
        assert "unexpectedly authorized" in str(exc)
    else:
        raise AssertionError("pull_request accepted the configured private selector")


def test_non_dispatch_cannot_authorize_without_configured_selector() -> None:
    try:
        route.resolve_route(
            event_name="pull_request_target",
            dispatch_selector_json="",
            configured_selector_json="",
            authorized_selector_json=MAC_PRO_SELECTOR,
            resolved_selector_json=MAC_PRO_SELECTOR,
        )
    except ValueError as exc:
        assert "unexpectedly authorized" in str(exc)
    else:
        raise AssertionError("pull_request accepted an unauthorized selector")


def test_namespace_provider_is_derived_from_resolved_selector() -> None:
    metadata = route.resolve_route(
        event_name="workflow_dispatch",
        dispatch_selector_json='["namespace-profile-generouscorp"]',
        configured_selector_json="",
        authorized_selector_json='["namespace-profile-generouscorp"]',
        resolved_selector_json='["namespace-profile-generouscorp"]',
    )
    assert metadata["linux_provider"] == "namespace"


def test_workflow_keeps_configured_and_authorized_selectors_distinct() -> None:
    text = BUILD_WORKFLOW.read_text(encoding="utf-8")
    configured = re.search(
        r"(?m)^\s+CONFIGURED_LINUX_RUNNER_SELECTOR_JSON:\s*(.+)$", text
    )
    automatic = re.search(
        r"(?m)^\s+AUTOMATIC_LINUX_RUNNER_SELECTOR_JSON:\s*(.+)$", text
    )
    authorized = re.search(
        r"(?m)^\s+EXPLICIT_LINUX_RUNNER_SELECTOR_JSON:\s*(.+)$", text
    )
    assert configured is not None
    assert automatic is not None
    assert authorized is not None
    assert "vars.PULP_LOCAL_LINUX_RUNS_ON_JSON" in configured.group(1)
    assert "vars.PULP_AUTO_LINUX_RUNS_ON_JSON" in automatic.group(1)
    assert "github.event_name == 'workflow_dispatch'" in authorized.group(1)
    assert "vars.PULP_LOCAL_LINUX_RUNS_ON_JSON" in authorized.group(1)
    assert "vars.PULP_AUTO_LINUX_RUNS_ON_JSON" not in authorized.group(1)
    assert '"pulp-auto-linux-x64"' in text
    assert "automatic_local_linux_selector" in text
    assert "parseable_automatic_linux_selector" in text
    assert "isinstance(parsed_local_linux_selector, (str, list))" in text
    assert "configured Linux selector lacks the exact" in text
    trusted_dispatcher = PR_SAFE_WORKFLOW.read_text(encoding="utf-8")
    assert (
        "TRUSTED_SELECTOR_JSON: "
        "${{ vars.PULP_AUTO_LINUX_RUNS_ON_JSON || '' }}"
        in trusted_dispatcher
    )
    assert (
        "TRUSTED_SELECTOR_JSON: ${{ vars.PULP_LOCAL_LINUX_RUNS_ON_JSON"
        not in trusted_dispatcher
    )


def test_protected_automatic_workflows_use_restricted_linux_selector() -> None:
    for workflow in (VELLUM_WORKFLOW, VERSION_SKILL_WORKFLOW):
        text = workflow.read_text(encoding="utf-8")
        assert "runs-on: >-" in text, workflow
        expression = text.split("runs-on: >-", 1)[1].split("\n    steps:", 1)[0]
        assert "vars.PULP_AUTO_LINUX_RUNS_ON_JSON" in expression, workflow
        assert "github.event_name == 'workflow_dispatch'" in expression, workflow
        assert "vars.PULP_LOCAL_LINUX_RUNS_ON_JSON" in expression, workflow
        assert expression.index("github.event_name == 'workflow_dispatch'") < expression.index(
            "vars.PULP_LOCAL_LINUX_RUNS_ON_JSON"
        ), workflow


def test_workflow_exposes_reason_and_uses_resolved_provider() -> None:
    text = BUILD_WORKFLOW.read_text(encoding="utf-8")
    assert "linux_route_reason: ${{ steps.resolve.outputs.linux_route_reason }}" in text
    assert '"provider": linux_provider' in text
    assert '"route_reason": linux_route_reason' in text
    for reason in route.ROUTE_REASONS:
        assert reason in SCRIPT.read_text(encoding="utf-8")


def test_build_uses_a_bounded_fleet_wide_parallelism_cap() -> None:
    text = BUILD_WORKFLOW.read_text(encoding="utf-8")
    assert (
        'cmake --build "$PULP_BUILD_DIR" --config Release --parallel 4' in text
    )
    assert 'cmake --build "$PULP_BUILD_DIR" --config Release\n' not in text


def test_every_runs_on_json_selector_is_parsed_not_interpolated() -> None:
    """A `runs-on` that interpolates a *_RUNS_ON_JSON variable without
    `fromJSON` yields ONE label whose text is the literal JSON array. No runner
    can carry that label, and GitHub queues such a job forever instead of
    failing it, so the symptom is an invisible hang on a required gate rather
    than an error. Observed live on 2026-08-16: setting
    PULP_LOCAL_LINUX_RUNS_ON_JSON left a dispatched `Enforce version & skill
    sync` job queued with
    labels=['["self-hosted","Linux","X64","pulp-build-linux-x64","pulp-host-macpro"]'].
    """
    workflow_dir = BUILD_WORKFLOW.parent
    offenders = []
    for workflow in sorted(workflow_dir.glob("*.yml")):
        text = workflow.read_text(encoding="utf-8")
        for block in re.findall(r"^\s*runs-on:.*?(?=^\s*\S+:|\Z)", text, re.M | re.S):
            if "_RUNS_ON_JSON" not in block:
                continue
            if "fromJSON" in block:
                continue
            offenders.append(f"{workflow.name}: {' '.join(block.split())[:110]}")
    assert not offenders, (
        "runs-on interpolates a *_RUNS_ON_JSON variable without fromJSON; "
        "setting that variable would queue the job forever:\n  "
        + "\n  ".join(offenders)
    )


def test_json_selector_fallbacks_stay_json_quoted() -> None:
    """Wrapping the expression in `fromJSON` is only safe when EVERY branch is
    JSON. A bare `'ubuntu-latest'` fallback inside `fromJSON(...)` fails to
    parse and takes the required gate down with it, so the hosted fallbacks
    must be written as the quoted JSON string `'"ubuntu-latest"'`.
    """
    for name in ("version-skill-check.yml", "vellum-freeze-check.yml"):
        text = (BUILD_WORKFLOW.parent / name).read_text(encoding="utf-8")
        block = re.search(r"runs-on: >-\n(.*?\n)\s*(?:name:|steps:)", text, re.S)
        assert block, f"{name}: could not locate the runs-on block"
        body = block.group(1)
        assert "fromJSON(" in body, f"{name}: selector is not parsed"
        assert "|| 'ubuntu-latest'" not in body, (
            f"{name}: bare 'ubuntu-latest' fallback inside fromJSON would fail "
            "to parse; use the JSON-quoted form"
        )
        assert body.count("'\"ubuntu-latest\"'") >= 3, (
            f"{name}: every branch must carry a JSON-quoted hosted fallback"
        )


def _all_tests() -> list:
    return [
        value
        for name, value in globals().items()
        if name.startswith("test_") and callable(value)
    ]


def main() -> int:
    failures = 0
    for test in _all_tests():
        try:
            test()
            print(f"ok  {test.__name__}")
        except Exception as exc:  # noqa: BLE001
            failures += 1
            print(f"FAIL {test.__name__}: {exc}")
    if failures:
        print(f"\n{failures} failing test(s)")
        return 1
    print(f"\nall {len(_all_tests())} tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

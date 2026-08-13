#!/usr/bin/env python3
"""Focused runtime and workflow-structure tests for Linux runner routing."""

from __future__ import annotations

import importlib.util
import datetime as dt
import json
import re
import subprocess
import sys
from pathlib import Path


SCRIPT = Path(__file__).parent / "resolve_linux_route.py"
REPO_ROOT = Path(__file__).resolve().parents[2]
BUILD_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "build.yml"
VELLUM_TRUSTED_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "vellum-trusted-gate.yml"
VELLUM_FREEZE_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "vellum-freeze-check.yml"
VERSION_SKILL_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "version-skill-check.yml"
MAC_PRO_SELECTOR = (
    '["self-hosted","Linux","X64","pulp-build-linux-x64",'
    '"pulp-host-macpro"]'
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
    assert json.loads(metadata["configured_selector_json"])[-1] == "pulp-host-macpro"
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


def test_pull_request_stays_hosted_even_when_operator_lease_is_enabled() -> None:
    metadata = route.resolve_route(
        event_name="pull_request",
        dispatch_selector_json="",
        configured_selector_json=MAC_PRO_SELECTOR,
        authorized_selector_json="",
        resolved_selector_json='"ubuntu-latest"',
        local_enabled=True,
    )
    assert metadata["linux_route_reason"] == "security-hosted"
    assert metadata["linux_provider"] == "github-hosted"


def test_merge_group_prefers_macpro_when_operator_lease_is_enabled() -> None:
    metadata = route.resolve_route(
        event_name="merge_group",
        dispatch_selector_json="",
        configured_selector_json=MAC_PRO_SELECTOR,
        authorized_selector_json=MAC_PRO_SELECTOR,
        resolved_selector_json=MAC_PRO_SELECTOR,
        local_enabled=True,
    )
    assert metadata["linux_route_reason"] == "local-enabled"


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


def test_operator_lease_is_short_lived_and_fails_closed() -> None:
    now = dt.datetime(2026, 8, 13, 18, 0, tzinfo=dt.timezone.utc)
    assert route.operator_lease_active("2026-08-13T18:10:00Z", now)
    assert not route.operator_lease_active("2026-08-13T17:59:59Z", now)
    assert not route.operator_lease_active("2026-08-13T18:16:00Z", now)
    assert not route.operator_lease_active("not-a-time", now)
    assert not route.operator_lease_active("2026-08-13T18:10:00", now)
    assert not route.operator_lease_active("0001-01-01T00:00:00+14:00", now)


def test_reviewed_macpro_selector_is_order_independent_but_exact() -> None:
    labels = json.loads(MAC_PRO_SELECTOR)
    assert route.is_macpro_linux_selector(labels)
    assert route.is_macpro_linux_selector(list(reversed(labels)))
    assert not route.is_macpro_linux_selector(labels + [labels[-1]])
    assert not route.is_macpro_linux_selector(labels[:-1])


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


def test_non_dispatch_cannot_authorize_configured_selector_without_operator_lease() -> None:
    try:
        route.resolve_route(
            event_name="merge_group",
            dispatch_selector_json="",
            configured_selector_json=MAC_PRO_SELECTOR,
            authorized_selector_json=MAC_PRO_SELECTOR,
            resolved_selector_json='"ubuntu-latest"',
            local_enabled=False,
        )
    except ValueError as exc:
        assert "operator health lease" in str(exc)
    else:
        raise AssertionError("merge_group accepted an expired local lease")


def test_non_dispatch_cannot_authorize_without_configured_selector() -> None:
    try:
        route.resolve_route(
            event_name="merge_group",
            dispatch_selector_json="",
            configured_selector_json="",
            authorized_selector_json=MAC_PRO_SELECTOR,
            resolved_selector_json=MAC_PRO_SELECTOR,
        )
    except ValueError as exc:
        assert "configured selector" in str(exc)
    else:
        raise AssertionError("merge_group accepted an unauthorized selector")


def test_namespace_provider_is_derived_from_resolved_selector() -> None:
    metadata = route.resolve_route(
        event_name="workflow_dispatch",
        dispatch_selector_json='["namespace-profile-generouscorp"]',
        configured_selector_json="",
        authorized_selector_json='["namespace-profile-generouscorp"]',
        resolved_selector_json='["namespace-profile-generouscorp"]',
    )
    assert metadata["linux_provider"] == "namespace"


def test_workflow_keeps_configured_selector_separate_from_event_authorization() -> None:
    text = BUILD_WORKFLOW.read_text(encoding="utf-8")
    configured = re.search(
        r"(?m)^\s+CONFIGURED_LINUX_RUNNER_SELECTOR_JSON:\s*(.+)$", text
    )
    authorized = re.search(
        r"(?m)^\s+EXPLICIT_LINUX_RUNNER_SELECTOR_JSON:\s*(.+)$", text
    )
    assert configured is not None
    assert authorized is not None
    assert "vars.PULP_LOCAL_LINUX_RUNS_ON_JSON" in configured.group(1)
    assert "inputs.linux_runner_selector_json" in authorized.group(1)
    assert "def local_linux_capacity" not in text
    assert 'REPOSITORY = os.environ.get("GITHUB_REPOSITORY", "")' in text
    assert 'EVENT_NAME == "workflow_dispatch" and configured_linux_selector' in text
    assert 'automatic_linux = EVENT_NAME == "merge_group"' in text
    assert "automatic_linux and configured_linux_valid" in text
    assert "PULP_LOCAL_LINUX_LEASE_UNTIL" in text


def test_workflow_exposes_reason_and_uses_resolved_provider() -> None:
    text = BUILD_WORKFLOW.read_text(encoding="utf-8")
    assert "linux_route_reason: ${{ steps.resolve.outputs.linux_route_reason }}" in text
    assert '"provider": linux_provider' in text
    assert '"route_reason": linux_route_reason' in text
    for reason in route.ROUTE_REASONS:
        assert reason in SCRIPT.read_text(encoding="utf-8")


def test_privileged_lanes_stay_hosted_and_trusted_label_is_not_declared() -> None:
    vellum = VELLUM_TRUSTED_WORKFLOW.read_text(encoding="utf-8")
    assert "pulp-vellum-trusted-mg" not in vellum
    assert vellum.count("runs-on: ubuntu-latest") >= 2
    assert "name: Vellum trusted freeze" in vellum
    for name in ("release-cli.yml", "sign-and-release.yml"):
        text = (REPO_ROOT / ".github" / "workflows" / name).read_text()
        assert "PULP_LOCAL_LINUX_RUNS_ON_JSON" not in text


def test_small_unprivileged_gates_stay_hosted_without_an_expiry_resolver() -> None:
    for workflow in (VELLUM_FREEZE_WORKFLOW, VERSION_SKILL_WORKFLOW):
        text = workflow.read_text(encoding="utf-8")
        runs_on = re.search(r"(?m)^\s+runs-on:\s*(.+)$", text)
        assert runs_on is not None
        assert runs_on.group(1) == "ubuntu-latest"
        assert "PULP_LOCAL_LINUX_RUNS_ON_JSON" not in text
        assert "PULP_LOCAL_LINUX_LEASE_UNTIL" not in text


def test_unprivileged_gate_triggers_and_public_names_are_preserved() -> None:
    expected = (
        (VELLUM_FREEZE_WORKFLOW, "name: Vellum freeze"),
        (VERSION_SKILL_WORKFLOW, "name: Enforce version & skill sync"),
    )
    for workflow, public_name in expected:
        text = workflow.read_text(encoding="utf-8")
        trigger_block = text.split("jobs:", 1)[0]
        assert "pull_request:" in trigger_block
        assert "merge_group:" in trigger_block
        assert "workflow_dispatch:" in trigger_block
        assert public_name in text
    assert "permissions:\n  contents: read" in VELLUM_FREEZE_WORKFLOW.read_text()


def test_build_uses_a_bounded_fleet_wide_parallelism_cap() -> None:
    text = BUILD_WORKFLOW.read_text(encoding="utf-8")
    assert (
        'cmake --build "$PULP_BUILD_DIR" --config Release --parallel 4' in text
    )
    assert 'cmake --build "$PULP_BUILD_DIR" --config Release\n' not in text


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

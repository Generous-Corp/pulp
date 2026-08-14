#!/usr/bin/env python3
"""Focused policy tests for the PR-safe disposable Linux route."""

from __future__ import annotations

import importlib.util
import json
from datetime import datetime, timedelta, timezone
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = Path(__file__).with_name("resolve_pr_safe_linux_route.py")
WORKFLOW = ROOT / ".github" / "workflows" / "pr-safe-linux.yml"
BUILD = ROOT / ".github" / "workflows" / "build.yml"

spec = importlib.util.spec_from_file_location("pr_safe_route", SCRIPT)
route = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(route)


def _resolve(**overrides):
    now = datetime(2026, 8, 13, 12, tzinfo=timezone.utc)
    values = {
        "event_name": "pull_request",
        "repository": "Generous-Corp/pulp",
        "pr_head_repository": "Generous-Corp/pulp",
        "selector_json": json.dumps(route.PR_SAFE_SELECTOR),
        "lease_until": (now + timedelta(minutes=5)).isoformat(),
        "enabled": "true",
        "now": now,
    }
    values.update(overrides)
    return route.resolve(**values)


def test_same_repository_pr_with_live_lease_uses_local() -> None:
    assert _resolve()["use_reusable"] is True


def test_fork_never_uses_local() -> None:
    result = _resolve(pr_head_repository="someone/fork")
    assert result == {"use_reusable": False, "reason": "fork-or-missing-head-repository", "selector_json": ""}


def test_expired_or_malformed_lease_falls_back() -> None:
    now = datetime(2026, 8, 13, 12, tzinfo=timezone.utc)
    assert _resolve(lease_until=(now - timedelta(seconds=1)).isoformat())["use_reusable"] is False
    assert _resolve(lease_until="not-a-time")["use_reusable"] is False


def test_implausibly_distant_lease_falls_back() -> None:
    now = datetime(2026, 8, 13, 12, tzinfo=timezone.utc)
    assert _resolve(lease_until=(now + timedelta(minutes=16)).isoformat())["use_reusable"] is False


def test_wrong_event_selector_or_disabled_route_falls_back() -> None:
    assert _resolve(event_name="pull_request_target")["use_reusable"] is False
    assert _resolve(event_name="merge_group")["use_reusable"] is False
    assert _resolve(selector_json='["self-hosted","Linux"]')["use_reusable"] is False
    assert _resolve(enabled="false")["use_reusable"] is False


def test_reusable_workflow_is_read_only_and_validates_pr_identity() -> None:
    text = WORKFLOW.read_text(encoding="utf-8")
    assert "workflow_call:" in text
    assert "contents: read" in text
    assert "pull-requests: write" not in text
    assert "statuses: write" not in text
    assert "secrets: inherit" not in text
    assert "github.event.pull_request.head.repo.full_name" in text
    assert "github.event.pull_request.head.sha" in text
    assert "pulp-pr-safe-linux-x64" in text
    assert "pulp-auto-linux-x64" not in text
    assert "runs-on: ubuntu-latest" in text
    assert "PULP_PR_SAFE_LINUX_REUSABLE_ENABLED" in text
    assert "PULP_PR_SAFE_LINUX_LEASE_UNTIL" in text
    assert "needs.admission.outputs.admitted == 'true'" in text
    assert "inputs.runner_selector_json || '\"ubuntu-latest\"'" in text
    assert "PR-safe admission denied; using hosted Linux" in text
    assert "AudioInspectorPanel renders a non-empty headless snapshot" in text
    assert "Subtree cache composites a GPU image on the replay frame" in text


def test_build_calls_only_the_main_owned_reusable_workflow() -> None:
    text = BUILD.read_text(encoding="utf-8")
    assert "uses: Generous-Corp/pulp/.github/workflows/pr-safe-linux.yml@main" in text
    assert "PULP_PR_SAFE_LINUX_LEASE_UNTIL" in text
    assert "PULP_PR_SAFE_LINUX_RUNS_ON_JSON" in text
    assert "PULP_PR_SAFE_LINUX_REUSABLE_ENABLED" in text


def main() -> int:
    tests = [value for name, value in globals().items() if name.startswith("test_")]
    for test in tests:
        test()
        print(f"ok  {test.__name__}")
    print(f"\nall {len(tests)} tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Focused tests for the fail-closed safe-runner selector.

This selector decides whether a job runs on our own hardware or on hosted
capacity, so every ambiguous case must resolve to the hosted fallback. The
tests below pin that direction explicitly: a selector is used only when an
online, idle runner currently carries every requested label AND the caller
passed the eligibility opt-in. Anything else — an unset selector, a busy or
offline runner, a missing label, an API failure, malformed JSON — must fall
back rather than queue against capacity that cannot serve it.
"""

from __future__ import annotations

import importlib.util
import io
import json
import subprocess
import sys
from contextlib import redirect_stdout
from pathlib import Path


SCRIPT = Path(__file__).parent / "resolve_safe_runner.py"
SELECTOR = ["self-hosted", "macOS", "ARM64", "pulp-build", "pulp-build-vm"]
FALLBACK = "macos-15"

_SPEC = importlib.util.spec_from_file_location("safe_runner_under_test", SCRIPT)
safe = importlib.util.module_from_spec(_SPEC)
assert _SPEC.loader is not None
_SPEC.loader.exec_module(safe)


def _runner(labels: list[str], *, status: str = "online", busy: bool = False) -> dict:
    return {
        "status": status,
        "busy": busy,
        "labels": [{"name": name} for name in labels],
    }


def _stub_api(payload: dict, *, raises: Exception | None = None):
    """Replace the `gh api` call with a canned inventory."""

    def _run(*args, **kwargs):
        if raises is not None:
            raise raises
        return subprocess.CompletedProcess(
            args=args[0] if args else [], returncode=0, stdout=json.dumps(payload), stderr=""
        )

    return _run


def _with_stub(payload: dict, *, raises: Exception | None = None):
    original = safe.subprocess.run
    safe.subprocess.run = _stub_api(payload, raises=raises)
    return original


def _restore(original) -> None:
    safe.subprocess.run = original


def _resolve(argv: list[str]) -> dict[str, str]:
    """Run main() with argv and parse its key=value output."""
    original_argv = sys.argv
    sys.argv = ["resolve_safe_runner.py", *argv]
    buffer = io.StringIO()
    try:
        with redirect_stdout(buffer):
            code = safe.main()
    finally:
        sys.argv = original_argv
    assert code == 0, f"selector must never fail the job, got exit {code}"
    parsed = dict(
        line.split("=", 1) for line in buffer.getvalue().splitlines() if "=" in line
    )
    return parsed


# --- local_healthy: the capacity probe ---------------------------------------


def test_online_idle_runner_with_every_label_is_healthy() -> None:
    original = _with_stub({"runners": [_runner(SELECTOR)]})
    try:
        assert safe.local_healthy("owner/repo", SELECTOR) is True
    finally:
        _restore(original)


def test_busy_runner_is_not_healthy() -> None:
    original = _with_stub({"runners": [_runner(SELECTOR, busy=True)]})
    try:
        assert safe.local_healthy("owner/repo", SELECTOR) is False
    finally:
        _restore(original)


def test_offline_runner_is_not_healthy() -> None:
    original = _with_stub({"runners": [_runner(SELECTOR, status="offline")]})
    try:
        assert safe.local_healthy("owner/repo", SELECTOR) is False
    finally:
        _restore(original)


def test_runner_missing_one_label_is_not_healthy() -> None:
    partial = [label for label in SELECTOR if label != "pulp-build-vm"]
    original = _with_stub({"runners": [_runner(partial)]})
    try:
        assert safe.local_healthy("owner/repo", SELECTOR) is False
    finally:
        _restore(original)


def test_empty_runner_inventory_is_not_healthy() -> None:
    original = _with_stub({"runners": []})
    try:
        assert safe.local_healthy("owner/repo", SELECTOR) is False
    finally:
        _restore(original)


def test_hosted_selector_never_probes_as_local() -> None:
    """A hosted label set must short-circuit before any API call."""

    def _explode(*args, **kwargs):
        raise AssertionError("must not query runners for a hosted selector")

    original = safe.subprocess.run
    safe.subprocess.run = _explode
    try:
        assert safe.local_healthy("owner/repo", "macos-15") is False
    finally:
        _restore(original)


def test_extra_runner_labels_still_satisfy_the_subset() -> None:
    original = _with_stub({"runners": [_runner([*SELECTOR, "pulp-gate-fast"])]})
    try:
        assert safe.local_healthy("owner/repo", SELECTOR) is True
    finally:
        _restore(original)


# --- main(): the routing decision --------------------------------------------


def test_healthy_local_runner_is_selected_when_eligible() -> None:
    original = _with_stub({"runners": [_runner(SELECTOR)]})
    try:
        result = _resolve(
            [
                "--repo", "owner/repo",
                "--configured-json", json.dumps(SELECTOR),
                "--fallback-json", json.dumps(FALLBACK),
                "--allow-local",
            ]
        )
    finally:
        _restore(original)
    assert result["route_reason"] == "local-healthy"
    assert json.loads(result["selector_json"]) == SELECTOR


def test_ineligible_event_stays_hosted_even_with_idle_capacity() -> None:
    """Without --allow-local the runner inventory is irrelevant."""
    original = _with_stub({"runners": [_runner(SELECTOR)]})
    try:
        result = _resolve(
            [
                "--repo", "owner/repo",
                "--configured-json", json.dumps(SELECTOR),
                "--fallback-json", json.dumps(FALLBACK),
            ]
        )
    finally:
        _restore(original)
    assert result["route_reason"] == "hosted-fallback"
    assert json.loads(result["selector_json"]) == FALLBACK


def test_unset_selector_stays_hosted() -> None:
    result = _resolve(
        [
            "--repo", "owner/repo",
            "--configured-json", "",
            "--fallback-json", json.dumps(FALLBACK),
            "--allow-local",
        ]
    )
    assert result["route_reason"] == "hosted-fallback"
    assert json.loads(result["selector_json"]) == FALLBACK


def test_busy_pool_falls_back_instead_of_queueing() -> None:
    """The black-hole case: labels exist but nothing can serve them now."""
    original = _with_stub({"runners": [_runner(SELECTOR, busy=True)]})
    try:
        result = _resolve(
            [
                "--repo", "owner/repo",
                "--configured-json", json.dumps(SELECTOR),
                "--fallback-json", json.dumps(FALLBACK),
                "--allow-local",
            ]
        )
    finally:
        _restore(original)
    assert result["route_reason"] == "hosted-fallback"
    assert json.loads(result["selector_json"]) == FALLBACK


def test_api_failure_falls_back_and_records_the_cause() -> None:
    original = _with_stub({}, raises=subprocess.SubprocessError("gh exploded"))
    try:
        result = _resolve(
            [
                "--repo", "owner/repo",
                "--configured-json", json.dumps(SELECTOR),
                "--fallback-json", json.dumps(FALLBACK),
                "--allow-local",
            ]
        )
    finally:
        _restore(original)
    assert result["route_reason"].startswith("hosted-fallback:")
    assert json.loads(result["selector_json"]) == FALLBACK


def test_malformed_configured_selector_falls_back() -> None:
    result = _resolve(
        [
            "--repo", "owner/repo",
            "--configured-json", "{not json",
            "--fallback-json", json.dumps(FALLBACK),
            "--allow-local",
        ]
    )
    assert result["route_reason"].startswith("hosted-fallback:")
    assert json.loads(result["selector_json"]) == FALLBACK


def test_selector_output_is_compact_json_for_runs_on() -> None:
    """`runs-on` consumes this verbatim, so stray whitespace must not appear."""
    original = _with_stub({"runners": [_runner(SELECTOR)]})
    try:
        result = _resolve(
            [
                "--repo", "owner/repo",
                "--configured-json", json.dumps(SELECTOR),
                "--fallback-json", json.dumps(FALLBACK),
                "--allow-local",
            ]
        )
    finally:
        _restore(original)
    assert " " not in result["selector_json"]


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

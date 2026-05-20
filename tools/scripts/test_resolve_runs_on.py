#!/usr/bin/env python3
"""Tests for tools/scripts/resolve_runs_on.py.

Covers three invariants:

1. build.yml parity — for every combination of (provider, explicit selector,
   namespace env var) that build.yml previously supported, the new resolver
   returns the same JSON output.
2. Local provider — adding `local` wires through the new
   PULP_LOCAL_<PLATFORM>_RUNS_ON_JSON path without regressing other providers.
3. Default mode — sanitizer jobs with no repo var fall through to the
   hard-coded default (e.g. "macos-14") unchanged, matching today's
   sanitizers.yml behavior.

Run with:
    python3 tools/scripts/test_resolve_runs_on.py
"""

from __future__ import annotations

import json
import contextlib
import importlib.util
import io
import os
import re
import subprocess
import sys
from pathlib import Path

SCRIPT = Path(__file__).parent / "resolve_runs_on.py"
REPO_ROOT = Path(__file__).resolve().parents[2]
BUILD_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "build.yml"

_SPEC = importlib.util.spec_from_file_location("resolve_runs_on_under_test", SCRIPT)
resolver = importlib.util.module_from_spec(_SPEC)
assert _SPEC.loader is not None
_SPEC.loader.exec_module(resolver)


def _build_workflow_runner_provider_default() -> str:
    text = BUILD_WORKFLOW.read_text(encoding="utf-8")
    match = re.search(
        r"(?ms)^      runner_provider:\n(?P<body>(?:        .+\n)+)",
        text,
    )
    _assert(match is not None, "build.yml missing runner_provider input")
    body = match.group("body")
    default = re.search(r"(?m)^        default:\s*([A-Za-z0-9_-]+)\s*$", body)
    _assert(default is not None, "runner_provider input missing default")
    return default.group(1)


def _build_workflow_requested_provider_fallback() -> str:
    text = BUILD_WORKFLOW.read_text(encoding="utf-8")
    match = re.search(
        r"REQUESTED_PROVIDER:\s*\$\{\{\s*inputs\.runner_provider\s*\|\|\s*"
        r"vars\.PULP_DEFAULT_RUNNER_PROVIDER\s*\|\|\s*'([^']+)'\s*\}\}",
        text,
    )
    _assert(match is not None, "build.yml missing REQUESTED_PROVIDER fallback")
    return match.group(1)


def _run(args: list[str], env_extra: dict[str, str] | None = None,
         expect_error: bool = False) -> tuple[int, str, str]:
    env = os.environ.copy()
    # Clean any existing runner-resolver envs so parent env doesn't leak.
    for key in list(env):
        if key.startswith(("EXPLICIT_", "NAMESPACE_", "LOCAL_",
                           "REQUESTED_PROVIDER")):
            env.pop(key, None)
    if env_extra:
        env.update(env_extra)
    proc = subprocess.run(
        [sys.executable, str(SCRIPT), *args],
        env=env,
        capture_output=True,
        text=True,
    )
    if not expect_error:
        assert proc.returncode == 0, (
            f"unexpected failure: code={proc.returncode} "
            f"stdout={proc.stdout!r} stderr={proc.stderr!r}"
        )
    return proc.returncode, proc.stdout, proc.stderr


def _assert(condition: bool, msg: str) -> None:
    if not condition:
        raise AssertionError(msg)


# ── build.yml parity cases ──────────────────────────────────────────────


def test_provider_github_hosted_default() -> None:
    """github-hosted + no override -> fixed label."""
    _, out, _ = _run([
        "--target-name", "Linux (x64)",
        "--mode", "provider",
        "--github-hosted-label", "ubuntu-latest",
        "--explicit-env", "EXPLICIT_LINUX_RUNNER_SELECTOR_JSON",
        "--namespace-env", "NAMESPACE_LINUX_RUNS_ON_JSON",
        "--namespace-setting-name",
        "PULP_NAMESPACE_BUILD_LINUX_RUNS_ON_JSON",
    ], env_extra={"REQUESTED_PROVIDER": "github-hosted"})
    _assert(json.loads(out) == "ubuntu-latest",
            f"expected 'ubuntu-latest', got {out!r}")


def test_build_workflow_dispatch_default_does_not_require_namespace() -> None:
    """Shipyard workflow_dispatch must work when Namespace vars are unset."""
    provider = _build_workflow_runner_provider_default()
    _assert(provider == "github-hosted",
            f"workflow_dispatch default must avoid Namespace, got {provider!r}")
    _, out, _ = _run([
        "--target-name", "Linux (x64)",
        "--mode", "provider",
        "--github-hosted-label", "ubuntu-latest",
        "--explicit-env", "EXPLICIT_LINUX_RUNNER_SELECTOR_JSON",
        "--namespace-env", "NAMESPACE_LINUX_RUNS_ON_JSON",
        "--namespace-setting-name",
        "PULP_NAMESPACE_BUILD_LINUX_RUNS_ON_JSON",
    ], env_extra={"REQUESTED_PROVIDER": provider})
    _assert(json.loads(out) == "ubuntu-latest",
            f"dispatch default unexpectedly required Namespace: {out!r}")


def test_build_workflow_pull_request_fallback_does_not_require_namespace() -> None:
    """pull_request has no workflow_dispatch input, so the env fallback matters."""
    provider = _build_workflow_requested_provider_fallback()
    _assert(provider == "github-hosted",
            f"pull_request fallback must avoid Namespace, got {provider!r}")
    _, out, _ = _run([
        "--target-name", "Linux (x64)",
        "--mode", "provider",
        "--github-hosted-label", "ubuntu-latest",
        "--explicit-env", "EXPLICIT_LINUX_RUNNER_SELECTOR_JSON",
        "--namespace-env", "NAMESPACE_LINUX_RUNS_ON_JSON",
        "--namespace-setting-name",
        "PULP_NAMESPACE_BUILD_LINUX_RUNS_ON_JSON",
    ], env_extra={"REQUESTED_PROVIDER": provider})
    _assert(json.loads(out) == "ubuntu-latest",
            f"pull_request fallback unexpectedly required Namespace: {out!r}")


def test_provider_namespace_with_env() -> None:
    _, out, _ = _run([
        "--target-name", "Windows (x64)",
        "--mode", "provider",
        "--github-hosted-label", "windows-latest",
        "--explicit-env", "EXPLICIT_WINDOWS_RUNNER_SELECTOR_JSON",
        "--namespace-env", "NAMESPACE_WINDOWS_RUNS_ON_JSON",
        "--namespace-setting-name",
        "PULP_NAMESPACE_BUILD_WINDOWS_RUNS_ON_JSON",
    ], env_extra={
        "REQUESTED_PROVIDER": "namespace",
        "NAMESPACE_WINDOWS_RUNS_ON_JSON":
            '["namespace-profile-generouscorp-windows"]',
    })
    _assert(json.loads(out) == ["namespace-profile-generouscorp-windows"],
            f"unexpected namespace routing: {out!r}")


def test_provider_namespace_missing_env_errors() -> None:
    code, _, err = _run([
        "--target-name", "Linux (x64)",
        "--mode", "provider",
        "--github-hosted-label", "ubuntu-latest",
        "--explicit-env", "EXPLICIT_LINUX_RUNNER_SELECTOR_JSON",
        "--namespace-env", "NAMESPACE_LINUX_RUNS_ON_JSON",
        "--namespace-setting-name",
        "PULP_NAMESPACE_BUILD_LINUX_RUNS_ON_JSON",
    ], env_extra={"REQUESTED_PROVIDER": "namespace"}, expect_error=True)
    _assert(code == 1, "expected error exit")
    _assert("PULP_NAMESPACE_BUILD_LINUX_RUNS_ON_JSON" in err,
            f"stderr missing setting name: {err!r}")


def test_provider_namespace_missing_env_uses_env_name_without_setting_name() -> None:
    code, _, err = _run([
        "--target-name", "Linux (x64)",
        "--mode", "provider",
        "--github-hosted-label", "ubuntu-latest",
        "--namespace-env", "NAMESPACE_LINUX_RUNS_ON_JSON",
    ], env_extra={"REQUESTED_PROVIDER": "namespace"}, expect_error=True)
    _assert(code == 1, "expected error exit")
    _assert("NAMESPACE_LINUX_RUNS_ON_JSON is not set" in err,
            f"stderr missing env fallback name: {err!r}")


def test_provider_explicit_beats_everything() -> None:
    _, out, _ = _run([
        "--target-name", "Linux (x64)",
        "--mode", "provider",
        "--github-hosted-label", "ubuntu-latest",
        "--explicit-env", "EXPLICIT_LINUX_RUNNER_SELECTOR_JSON",
        "--namespace-env", "NAMESPACE_LINUX_RUNS_ON_JSON",
        "--namespace-setting-name",
        "PULP_NAMESPACE_BUILD_LINUX_RUNS_ON_JSON",
    ], env_extra={
        "REQUESTED_PROVIDER": "namespace",
        "NAMESPACE_LINUX_RUNS_ON_JSON": '["namespace-fallback"]',
        "EXPLICIT_LINUX_RUNNER_SELECTOR_JSON": '"custom-runner"',
    })
    _assert(json.loads(out) == "custom-runner",
            f"explicit selector did not win: {out!r}")


def test_provider_uses_custom_requested_provider_env() -> None:
    _, out, _ = _run([
        "--target-name", "Linux (x64)",
        "--mode", "provider",
        "--requested-provider-env", "CUSTOM_PROVIDER",
        "--github-hosted-label", "ubuntu-latest",
        "--namespace-env", "NAMESPACE_LINUX_RUNS_ON_JSON",
    ], env_extra={
        "CUSTOM_PROVIDER": "namespace",
        "NAMESPACE_LINUX_RUNS_ON_JSON": '"namespace-linux"',
    })
    _assert(json.loads(out) == "namespace-linux",
            f"custom provider env was ignored: {out!r}")


def test_optional_namespace_no_env_returns_empty() -> None:
    """macOS path: if provider is not namespace and no selector, emit ''."""
    _, out, _ = _run([
        "--target-name", "macOS (ARM64)",
        "--mode", "optional-namespace",
        "--explicit-env", "EXPLICIT_MACOS_RUNNER_SELECTOR_JSON",
        "--namespace-env", "NAMESPACE_MACOS_RUNS_ON_JSON",
    ], env_extra={"REQUESTED_PROVIDER": "github-hosted"})
    _assert(out == "", f"expected empty stdout, got {out!r}")


def test_optional_namespace_provider_missing_env_returns_empty() -> None:
    _, out, _ = _run([
        "--target-name", "macOS (ARM64)",
        "--mode", "optional-namespace",
        "--namespace-env", "NAMESPACE_MACOS_RUNS_ON_JSON",
    ], env_extra={"REQUESTED_PROVIDER": "namespace"})
    _assert(out == "", f"expected empty stdout for unset namespace env, got {out!r}")


def test_optional_namespace_rejects_unsupported_provider() -> None:
    code, _, err = _run([
        "--target-name", "macOS (ARM64)",
        "--mode", "optional-namespace",
    ], env_extra={"REQUESTED_PROVIDER": "cloud"}, expect_error=True)
    _assert(code == 1, "expected error exit")
    _assert("Unsupported runner_provider" in err,
            f"stderr missing unsupported-provider error: {err!r}")


def test_optional_namespace_with_explicit_routes_under_namespace_provider() -> None:
    _, out, _ = _run([
        "--target-name", "macOS (ARM64)",
        "--mode", "optional-namespace",
        "--explicit-env", "EXPLICIT_MACOS_RUNNER_SELECTOR_JSON",
        "--namespace-env", "NAMESPACE_MACOS_RUNS_ON_JSON",
    ], env_extra={
        "REQUESTED_PROVIDER": "namespace",
        "EXPLICIT_MACOS_RUNNER_SELECTOR_JSON":
            '"namespace-profile-generouscorp-macos"',
    })
    _assert(json.loads(out) == "namespace-profile-generouscorp-macos",
            f"unexpected macOS namespace selector: {out!r}")


def test_optional_namespace_explicit_wins_under_github_hosted_provider() -> None:
    _, out, _ = _run([
        "--target-name", "macOS (ARM64)",
        "--mode", "optional-namespace",
        "--explicit-env", "EXPLICIT_MACOS_RUNNER_SELECTOR_JSON",
        "--namespace-env", "NAMESPACE_MACOS_RUNS_ON_JSON",
    ], env_extra={
        "REQUESTED_PROVIDER": "github-hosted",
        "EXPLICIT_MACOS_RUNNER_SELECTOR_JSON":
            '["self-hosted","sanitizer"]',
    })
    _assert(json.loads(out) == ["self-hosted", "sanitizer"],
            f"explicit macOS selector ignored: {out!r}")


def test_optional_namespace_falls_back_to_namespace_env() -> None:
    _, out, _ = _run([
        "--target-name", "macOS (ARM64)",
        "--mode", "optional-namespace",
        "--explicit-env", "EXPLICIT_MACOS_RUNNER_SELECTOR_JSON",
        "--namespace-env", "NAMESPACE_MACOS_RUNS_ON_JSON",
    ], env_extra={
        "REQUESTED_PROVIDER": "namespace",
        "EXPLICIT_MACOS_RUNNER_SELECTOR_JSON": "   ",
        "NAMESPACE_MACOS_RUNS_ON_JSON":
            '"namespace-profile-generouscorp-macos"',
    })
    _assert(json.loads(out) == "namespace-profile-generouscorp-macos",
            f"namespace fallback selector ignored: {out!r}")


# ── new: local provider ─────────────────────────────────────────────────


def test_provider_local_uses_local_env() -> None:
    _, out, _ = _run([
        "--target-name", "Linux (x64)",
        "--mode", "provider",
        "--github-hosted-label", "ubuntu-latest",
        "--explicit-env", "EXPLICIT_LINUX_RUNNER_SELECTOR_JSON",
        "--namespace-env", "NAMESPACE_LINUX_RUNS_ON_JSON",
        "--namespace-setting-name",
        "PULP_NAMESPACE_BUILD_LINUX_RUNS_ON_JSON",
        "--local-env", "LOCAL_LINUX_RUNS_ON_JSON",
        "--local-setting-name", "PULP_LOCAL_LINUX_RUNS_ON_JSON",
    ], env_extra={
        "REQUESTED_PROVIDER": "local",
        "LOCAL_LINUX_RUNS_ON_JSON":
            '["self-hosted","linux","arm64","sanitizer"]',
    })
    _assert(json.loads(out) == ["self-hosted", "linux", "arm64", "sanitizer"],
            f"unexpected local selector: {out!r}")


def test_provider_local_missing_env_errors() -> None:
    code, _, err = _run([
        "--target-name", "macOS (ARM64)",
        "--mode", "provider",
        "--github-hosted-label", "macos-15",
        "--explicit-env", "EXPLICIT_MACOS_RUNNER_SELECTOR_JSON",
        "--namespace-env", "NAMESPACE_MACOS_RUNS_ON_JSON",
        "--local-env", "LOCAL_MACOS_RUNS_ON_JSON",
        "--local-setting-name", "PULP_LOCAL_MAC_RUNS_ON_JSON",
    ], env_extra={"REQUESTED_PROVIDER": "local"}, expect_error=True)
    _assert(code == 1, "expected error exit")
    _assert("PULP_LOCAL_MAC_RUNS_ON_JSON" in err,
            f"stderr missing setting name: {err!r}")


def test_provider_local_missing_env_uses_env_name_without_setting_name() -> None:
    code, _, err = _run([
        "--target-name", "macOS (ARM64)",
        "--mode", "provider",
        "--github-hosted-label", "macos-15",
        "--local-env", "LOCAL_MACOS_RUNS_ON_JSON",
    ], env_extra={"REQUESTED_PROVIDER": "local"}, expect_error=True)
    _assert(code == 1, "expected error exit")
    _assert("LOCAL_MACOS_RUNS_ON_JSON is not set" in err,
            f"stderr missing env fallback name: {err!r}")


# ── default mode (sanitizers) ───────────────────────────────────────────


def test_default_mode_falls_back_to_label() -> None:
    """Sanitizer: no explicit, no override -> hard-coded default."""
    _, out, _ = _run([
        "--target-name", "TSan (macOS ARM64)",
        "--mode", "default",
        "--explicit-env", "EXPLICIT_TSAN_RUNNER_SELECTOR_JSON",
        "--override-env", "TSAN_RUNS_ON_JSON",
        "--default-label", "macos-14",
    ])
    _assert(json.loads(out) == "macos-14",
            f"expected default 'macos-14', got {out!r}")


def test_default_mode_override_wins() -> None:
    _, out, _ = _run([
        "--target-name", "TSan (macOS ARM64)",
        "--mode", "default",
        "--explicit-env", "EXPLICIT_TSAN_RUNNER_SELECTOR_JSON",
        "--override-env", "TSAN_RUNS_ON_JSON",
        "--default-label", "macos-14",
    ], env_extra={
        "TSAN_RUNS_ON_JSON":
            '["self-hosted","macos","arm64","sanitizer"]',
    })
    _assert(json.loads(out) == ["self-hosted", "macos", "arm64", "sanitizer"],
            f"override ignored: {out!r}")


def test_default_mode_explicit_beats_override() -> None:
    _, out, _ = _run([
        "--target-name", "TSan (macOS ARM64)",
        "--mode", "default",
        "--explicit-env", "EXPLICIT_TSAN_RUNNER_SELECTOR_JSON",
        "--override-env", "TSAN_RUNS_ON_JSON",
        "--default-label", "macos-14",
    ], env_extra={
        "TSAN_RUNS_ON_JSON": '["repo-var"]',
        "EXPLICIT_TSAN_RUNNER_SELECTOR_JSON": '["workflow-dispatch"]',
    })
    _assert(json.loads(out) == ["workflow-dispatch"],
            f"explicit did not beat override: {out!r}")


def test_default_mode_ubuntu_default() -> None:
    """Sanitizer with ubuntu-24.04 default (rtsan)."""
    _, out, _ = _run([
        "--target-name", "RTSan (Linux x86_64)",
        "--mode", "default",
        "--explicit-env", "EXPLICIT_RTSAN_RUNNER_SELECTOR_JSON",
        "--override-env", "RTSAN_RUNS_ON_JSON",
        "--default-label", "ubuntu-24.04",
    ])
    _assert(json.loads(out) == "ubuntu-24.04",
            f"expected default 'ubuntu-24.04', got {out!r}")


def test_default_mode_ignores_unsupported_requested_provider() -> None:
    _, out, _ = _run([
        "--target-name", "TSan (macOS ARM64)",
        "--mode", "default",
        "--default-label", "macos-14",
    ], env_extra={"REQUESTED_PROVIDER": "bogus"})
    _assert(json.loads(out) == "macos-14",
            f"default mode unexpectedly consulted provider: {out!r}")


# ── argument validation ───────────────────────────────────────────────────


def test_provider_mode_requires_github_hosted_label() -> None:
    code, _, err = _run([
        "--target-name", "Linux (x64)",
        "--mode", "provider",
    ], expect_error=True)
    _assert(code == 1, "expected error exit")
    _assert("--github-hosted-label is required" in err,
            f"stderr missing required-label error: {err!r}")


def test_default_mode_requires_default_label() -> None:
    code, _, err = _run([
        "--target-name", "TSan (macOS ARM64)",
        "--mode", "default",
    ], expect_error=True)
    _assert(code == 1, "expected error exit")
    _assert("--default-label is required" in err,
            f"stderr missing required-default error: {err!r}")


def test_provider_mode_rejects_unsupported_provider() -> None:
    code, _, err = _run([
        "--target-name", "Linux (x64)",
        "--mode", "provider",
        "--github-hosted-label", "ubuntu-latest",
    ], env_extra={"REQUESTED_PROVIDER": "bogus"}, expect_error=True)
    _assert(code == 1, "expected error exit")
    _assert("Unsupported runner_provider" in err,
            f"stderr missing unsupported-provider error: {err!r}")


def test_provider_resolver_function_rejects_unsupported_provider_directly() -> None:
    stderr = io.StringIO()
    with contextlib.redirect_stderr(stderr):
        try:
            resolver.resolve_provider_mode(
                target_name="Linux (x64)",
                requested="unsupported",
                github_hosted_label="ubuntu-latest",
                explicit_env=None,
                namespace_env=None,
                namespace_setting_name=None,
                local_env=None,
                local_setting_name=None,
            )
        except SystemExit as exc:
            _assert(exc.code == 1, f"unexpected exit code: {exc.code!r}")
        else:
            raise AssertionError("expected SystemExit")
    _assert("Unsupported runner_provider: unsupported" in stderr.getvalue(),
            f"stderr missing direct unsupported-provider error: {stderr.getvalue()!r}")


# ── JSON validation ─────────────────────────────────────────────────────


def test_invalid_json_errors() -> None:
    code, _, err = _run([
        "--target-name", "TSan (macOS ARM64)",
        "--mode", "default",
        "--override-env", "TSAN_RUNS_ON_JSON",
        "--default-label", "macos-14",
    ], env_extra={"TSAN_RUNS_ON_JSON": "{not-json"}, expect_error=True)
    _assert(code == 1, "expected error exit")
    _assert("TSan" in err, f"stderr missing target name: {err!r}")


def test_explicit_invalid_json_errors_before_provider_selection() -> None:
    code, _, err = _run([
        "--target-name", "Linux (x64)",
        "--mode", "provider",
        "--github-hosted-label", "ubuntu-latest",
        "--explicit-env", "EXPLICIT_LINUX_RUNNER_SELECTOR_JSON",
        "--namespace-env", "NAMESPACE_LINUX_RUNS_ON_JSON",
    ], env_extra={
        "REQUESTED_PROVIDER": "namespace",
        "EXPLICIT_LINUX_RUNNER_SELECTOR_JSON": "{bad-json",
        "NAMESPACE_LINUX_RUNS_ON_JSON": '"namespace-linux"',
    }, expect_error=True)
    _assert(code == 1, "expected error exit")
    _assert("Linux (x64) runner selector JSON is not valid" in err,
            f"stderr missing explicit selector error: {err!r}")


def test_non_string_or_list_json_errors() -> None:
    code, _, err = _run([
        "--target-name", "Linux (x64)",
        "--mode", "default",
        "--override-env", "LINUX_RUNS_ON_JSON",
        "--default-label", "ubuntu-24.04",
    ], env_extra={"LINUX_RUNS_ON_JSON": "42"}, expect_error=True)
    _assert(code == 1, "expected error exit")


def test_script_entrypoint_success() -> None:
    proc = subprocess.run(
        [
            sys.executable, str(SCRIPT),
            "--target-name", "Linux (x64)",
            "--mode", "default",
            "--default-label", "ubuntu-24.04",
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    _assert(proc.returncode == 0, f"unexpected rc: {proc.stderr!r}")
    _assert(json.loads(proc.stdout) == "ubuntu-24.04",
            f"unexpected script output: {proc.stdout!r}")


def _all_tests() -> list:
    return [
        obj for name, obj in globals().items()
        if name.startswith("test_") and callable(obj)
    ]


def main() -> int:
    failures = 0
    for fn in _all_tests():
        try:
            fn()
            print(f"ok  {fn.__name__}")
        except Exception as exc:  # noqa: BLE001
            failures += 1
            print(f"FAIL {fn.__name__}: {exc}")
    if failures:
        print(f"\n{failures} failing test(s)")
        return 1
    print(f"\nall {len(_all_tests())} tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())

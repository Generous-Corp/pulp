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
import os
import subprocess
import sys
from pathlib import Path

SCRIPT = Path(__file__).parent / "resolve_runs_on.py"


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


def test_optional_namespace_no_env_returns_empty() -> None:
    """macOS path: if provider is not namespace and no selector, emit ''."""
    _, out, _ = _run([
        "--target-name", "macOS (ARM64)",
        "--mode", "optional-namespace",
        "--explicit-env", "EXPLICIT_MACOS_RUNNER_SELECTOR_JSON",
        "--namespace-env", "NAMESPACE_MACOS_RUNS_ON_JSON",
    ], env_extra={"REQUESTED_PROVIDER": "github-hosted"})
    _assert(out == "", f"expected empty stdout, got {out!r}")


def test_optional_namespace_with_explicit_routes_to_namespace() -> None:
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


def test_non_string_or_list_json_errors() -> None:
    code, _, err = _run([
        "--target-name", "Linux (x64)",
        "--mode", "default",
        "--override-env", "LINUX_RUNS_ON_JSON",
        "--default-label", "ubuntu-24.04",
    ], env_extra={"LINUX_RUNS_ON_JSON": "42"}, expect_error=True)
    _assert(code == 1, "expected error exit")


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

#!/usr/bin/env python3
"""Shared runs-on resolver for Pulp GitHub Actions workflows.

Resolves the `runs-on` value for a CI job from (in priority order):

    1. an explicit workflow_dispatch selector passed as JSON
    2. a repository variable JSON (e.g. PULP_SANITIZER_TSAN_RUNS_ON_JSON,
       PULP_NAMESPACE_BUILD_LINUX_RUNS_ON_JSON, PULP_LOCAL_MAC_RUNS_ON_JSON)
    3. a provider-aware fallback (github-hosted label or namespace repo var),
       selected via a REQUESTED_PROVIDER env var whose value is one of
       `github-hosted`, `namespace`, or `local`
    4. a hard-coded last-resort default (e.g. ["macos-14"])

This script is a lift-out of the inline Python that previously lived in
`.github/workflows/build.yml` (commit range: origin/main circa 2026-04).
Lifting it into a shared script lets any workflow (notably sanitizers.yml)
reuse the same resolver without duplicating logic.

The `local` provider is new: it resolves to whatever
`PULP_LOCAL_<PLATFORM>_RUNS_ON_JSON` says (e.g. a self-hosted runner
labelset like ["self-hosted","macos","arm64","sanitizer"]).

### Fluidity invariant

When every new repo variable is unset, this resolver MUST produce the same
output as the previous hard-coded values. Switching a job's runner is
opt-in via `gh variable set ...`; there is no forced migration.

### Modes

The script supports two modes, selected by `--mode`:

- `mode=provider` (default when --github-hosted-label is provided):
    classic build.yml behavior — explicit → provider dispatch
    (github-hosted / namespace / local) → hard-coded
- `mode=default` (used by sanitizers.yml):
    sanitizer/per-target behavior — explicit → repo var override →
    hard-coded default. Provider dispatch is intentionally not used here;
    sanitizer jobs pick their own platform and the repo var is the single
    switch.

Output: a JSON string/array suitable for the GHA `runs-on:` key, printed
on stdout. Exit 0 on success, 1 on validation error (with human-readable
stderr).

Usage (provider mode, equivalent to previous build.yml resolver):

    python3 tools/scripts/resolve_runs_on.py \\
        --target-name "Linux (x64)" \\
        --mode provider \\
        --github-hosted-label ubuntu-latest \\
        --explicit-env EXPLICIT_LINUX_RUNNER_SELECTOR_JSON \\
        --namespace-env NAMESPACE_LINUX_RUNS_ON_JSON \\
        --namespace-setting-name PULP_NAMESPACE_BUILD_LINUX_RUNS_ON_JSON \\
        --local-env LOCAL_LINUX_RUNS_ON_JSON \\
        --local-setting-name PULP_LOCAL_LINUX_RUNS_ON_JSON

Usage (optional-namespace mode — previous macOS behavior):

    python3 tools/scripts/resolve_runs_on.py \\
        --target-name "macOS (ARM64)" \\
        --mode optional-namespace \\
        --explicit-env EXPLICIT_MACOS_RUNNER_SELECTOR_JSON \\
        --namespace-env NAMESPACE_MACOS_RUNS_ON_JSON

    # emits empty stdout if the provider is not `namespace` and no
    # explicit selector is set. Caller decides what to do with empty.

Usage (default mode, used by sanitizers.yml):

    python3 tools/scripts/resolve_runs_on.py \\
        --target-name "ThreadSanitizer (macOS ARM64)" \\
        --mode default \\
        --explicit-env EXPLICIT_TSAN_RUNNER_SELECTOR_JSON \\
        --override-env TSAN_RUNS_ON_JSON \\
        --default-label macos-14
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from typing import Optional


VALID_PROVIDERS = ("github-hosted", "namespace", "local")


def _load_selector(raw: str, target_name: str) -> str:
    """Validate a raw JSON selector string and return it re-serialized."""
    try:
        decoded = json.loads(raw)
    except json.JSONDecodeError as exc:
        print(
            f"{target_name} runner selector JSON is not valid: {exc}",
            file=sys.stderr,
        )
        sys.exit(1)
    if not isinstance(decoded, (str, list)):
        print(
            f"{target_name} runner selector JSON must decode to a string "
            f"or array accepted by runs-on.",
            file=sys.stderr,
        )
        sys.exit(1)
    return json.dumps(decoded)


def _env_nonempty(name: Optional[str]) -> str:
    if not name:
        return ""
    return (os.environ.get(name) or "").strip()


def resolve_provider_mode(
    *,
    target_name: str,
    requested: str,
    github_hosted_label: str,
    explicit_env: Optional[str],
    namespace_env: Optional[str],
    namespace_setting_name: Optional[str],
    local_env: Optional[str],
    local_setting_name: Optional[str],
) -> str:
    """Resolve runs-on for a target that participates in provider dispatch.

    This mirrors the previous inline `resolve_runs_on()` in build.yml and
    adds `local` as a third provider.
    """
    explicit = _env_nonempty(explicit_env)
    if explicit:
        return _load_selector(explicit, target_name)

    if requested == "github-hosted":
        return json.dumps(github_hosted_label)

    if requested == "namespace":
        raw = _env_nonempty(namespace_env)
        if not raw:
            print(
                f"{namespace_setting_name or namespace_env} is not set; "
                f"cannot route {target_name} to Namespace.",
                file=sys.stderr,
            )
            sys.exit(1)
        return _load_selector(raw, target_name)

    if requested == "local":
        raw = _env_nonempty(local_env)
        if not raw:
            print(
                f"{local_setting_name or local_env} is not set; "
                f"cannot route {target_name} to a local self-hosted runner.",
                file=sys.stderr,
            )
            sys.exit(1)
        return _load_selector(raw, target_name)

    print(f"Unsupported runner_provider: {requested}", file=sys.stderr)
    sys.exit(1)


def resolve_optional_namespace_mode(
    *,
    target_name: str,
    requested: str,
    explicit_env: Optional[str],
    namespace_env: Optional[str],
) -> str:
    """Resolve runs-on when the target is only included if a Namespace
    selector is provided (current macOS behavior in build.yml)."""
    if requested != "namespace":
        return ""
    raw = _env_nonempty(explicit_env)
    if not raw:
        raw = _env_nonempty(namespace_env)
    if not raw:
        return ""
    return _load_selector(raw, target_name)


def resolve_default_mode(
    *,
    target_name: str,
    explicit_env: Optional[str],
    override_env: Optional[str],
    default_label: str,
) -> str:
    """Resolve runs-on for a job with a hard-coded fallback label.

    Precedence:
        1. explicit workflow_dispatch input (if set)
        2. repo variable override (if set)
        3. hard-coded `default_label` (e.g. "macos-14" / "ubuntu-24.04")

    This is the path used by sanitizer jobs (asan/tsan/ubsan/rtsan). It
    intentionally does NOT consult `REQUESTED_PROVIDER`; sanitizer jobs
    choose their platform explicitly, and the repo variable is the single
    knob that moves them.
    """
    explicit = _env_nonempty(explicit_env)
    if explicit:
        return _load_selector(explicit, target_name)
    override = _env_nonempty(override_env)
    if override:
        return _load_selector(override, target_name)
    return json.dumps(default_label)


def _build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--target-name", required=True,
                   help="Human-readable target name used in error messages.")
    p.add_argument("--mode",
                   choices=("provider", "optional-namespace", "default"),
                   default="provider")
    p.add_argument("--requested-provider-env",
                   default="REQUESTED_PROVIDER",
                   help="Env var holding the requested provider name "
                        "(github-hosted | namespace | local). Default: "
                        "REQUESTED_PROVIDER.")
    p.add_argument("--github-hosted-label",
                   help="GitHub-hosted runner label (e.g. ubuntu-latest). "
                        "Required in provider mode.")
    p.add_argument("--explicit-env",
                   help="Env var holding an explicit workflow_dispatch "
                        "JSON selector; takes precedence over everything.")
    p.add_argument("--namespace-env",
                   help="Env var holding the Namespace selector JSON.")
    p.add_argument("--namespace-setting-name",
                   help="Human-readable repo-variable name used in errors.")
    p.add_argument("--local-env",
                   help="Env var holding the local (self-hosted) selector "
                        "JSON.")
    p.add_argument("--local-setting-name",
                   help="Human-readable repo-variable name used in errors.")
    p.add_argument("--override-env",
                   help="Default-mode: env var holding a repo-variable JSON "
                        "override (e.g. PULP_SANITIZER_TSAN_RUNS_ON_JSON).")
    p.add_argument("--default-label",
                   help="Default-mode: hard-coded fallback label "
                        "(e.g. macos-14).")
    return p


def main(argv: Optional[list[str]] = None) -> int:
    args = _build_parser().parse_args(argv)
    requested = (os.environ.get(args.requested_provider_env)
                 or "github-hosted").strip()
    if args.mode in ("provider", "optional-namespace"):
        if requested not in VALID_PROVIDERS:
            print(
                f"Unsupported runner_provider: {requested}. "
                f"Expected one of {VALID_PROVIDERS}.",
                file=sys.stderr,
            )
            return 1

    if args.mode == "provider":
        if not args.github_hosted_label:
            print("--github-hosted-label is required in provider mode.",
                  file=sys.stderr)
            return 1
        out = resolve_provider_mode(
            target_name=args.target_name,
            requested=requested,
            github_hosted_label=args.github_hosted_label,
            explicit_env=args.explicit_env,
            namespace_env=args.namespace_env,
            namespace_setting_name=args.namespace_setting_name,
            local_env=args.local_env,
            local_setting_name=args.local_setting_name,
        )
    elif args.mode == "optional-namespace":
        out = resolve_optional_namespace_mode(
            target_name=args.target_name,
            requested=requested,
            explicit_env=args.explicit_env,
            namespace_env=args.namespace_env,
        )
    else:  # default
        if not args.default_label:
            print("--default-label is required in default mode.",
                  file=sys.stderr)
            return 1
        out = resolve_default_mode(
            target_name=args.target_name,
            explicit_env=args.explicit_env,
            override_env=args.override_env,
            default_label=args.default_label,
        )

    sys.stdout.write(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())

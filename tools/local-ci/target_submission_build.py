"""Submission metadata construction helpers for local CI target preflight."""

from __future__ import annotations

from collections.abc import Callable, Mapping
from pathlib import Path


def build_submission_metadata(
    config: dict,
    branch: str,
    sha: str,
    targets: list[str],
    priority: str,
    validation: str,
    *,
    allow_root_mismatch: bool,
    allow_unreachable_targets: bool,
    root: Path,
    cwd_fn: Callable[[], Path],
    git_root_for_fn: Callable[[Path], Path | None],
    config_path_fn: Callable[[], Path],
    config_source_name_fn: Callable[[Path], str],
    preflight_target_host_state_fn: Callable[[str, dict, dict], dict],
    find_material_config_drift_fn: Callable[[list[str]], list[str]],
    normalize_provenance_fn: Callable[[], dict],
    environ: Mapping[str, str],
) -> dict:
    cwd = cwd_fn().resolve()
    cwd_git_root = git_root_for_fn(cwd)
    submission_root = root.resolve()

    if cwd_git_root and cwd_git_root != submission_root and not allow_root_mismatch:
        raise ValueError(
            "Invoked from a different git root than the queued worktree. "
            f"cwd git root={cwd_git_root}, submission root={submission_root}. "
            "Run the worktree-local tools/local-ci/local_ci.py or pass --allow-root-mismatch."
        )

    config_file = config_path_fn().resolve()
    host_preflight: dict[str, dict] = {}
    warnings: list[str] = []
    errors: list[str] = []
    defaults = config.get("defaults", {})
    for name in targets:
        state = preflight_target_host_state_fn(name, config.get("targets", {}).get(name, {}), defaults)
        host_preflight[name] = state
        if state.get("warning"):
            warnings.append(state["warning"])
        if state.get("error"):
            errors.append(state["error"])

    namespace_failover_targets: list[str] = []
    failover_cfg = config.get("failover", {})
    namespace_auto = failover_cfg.get("namespace_auto", True)
    ga_defaults = config.get("github_actions", {}).get("defaults", {})
    default_provider = ga_defaults.get("provider", "github-hosted")

    if errors and namespace_auto and default_provider == "namespace":
        for name in targets:
            state = host_preflight.get(name, {})
            if state.get("status") == "unreachable":
                namespace_failover_targets.append(name)
                state["status"] = "namespace-failover"
                state["warning"] = f"{name}: SSH host unreachable — auto-failover to Namespace"
                state.pop("error", None)
                warnings.append(state["warning"])
        errors = [error for error in errors if not any(target in error for target in namespace_failover_targets)]

    if errors and not allow_unreachable_targets:
        raise ValueError("; ".join(errors) + ". Pass --allow-unreachable-targets to queue anyway.")

    config_drift = [] if environ.get("PULP_LOCAL_CI_CONFIG") else find_material_config_drift_fn(targets)
    if config_drift:
        warnings.append("config drift detected between shared-state and worktree-local config")

    return {
        "submitted_root": str(submission_root),
        "cwd": str(cwd),
        "cwd_git_root": str(cwd_git_root) if cwd_git_root else "",
        "config_path": str(config_file),
        "config_source": config_source_name_fn(config_file),
        "branch": branch,
        "sha": sha,
        "priority": priority,
        "validation": validation,
        "targets": targets,
        "target_hosts": host_preflight,
        "namespace_failover_targets": namespace_failover_targets,
        "config_drift": config_drift,
        "warnings": warnings,
        "provenance": normalize_provenance_fn(),
    }

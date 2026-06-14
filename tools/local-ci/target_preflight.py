"""Target reachability and submission preflight helpers for local CI."""

from __future__ import annotations

from collections.abc import Callable, Mapping
import json
from pathlib import Path

from target_host_reachability import ensure_host_reachable, preflight_target_host_state
from target_ssh_reachability import (
    ssh_command_result,
    ssh_failure_detail,
    ssh_probe,
    ssh_reachable,
)
from target_utm_reachability import (
    utmctl_start,
    utmctl_vm_status,
)


def config_source_name(
    path: Path,
    *,
    environ: Mapping[str, str],
    shared_config_path_fn: Callable[[], Path],
) -> str:
    override = environ.get("PULP_LOCAL_CI_CONFIG")
    if override:
        return "env-override"
    if path == shared_config_path_fn():
        return "shared-state"
    return "worktree-local"


def config_material_for_targets(config: dict, targets: list[str]) -> dict:
    material: dict[str, dict] = {}
    for name in targets:
        target_cfg = config.get("targets", {}).get(name)
        if not target_cfg:
            continue
        entry = {
            "type": target_cfg.get("type", "local"),
            "enabled": bool(target_cfg.get("enabled", True)),
        }
        for key in (
            "host",
            "fallback_host",
            "repo_path",
            "utm_fallback",
            "cmake_generator",
            "cmake_platform",
            "cmake_generator_instance",
        ):
            value = target_cfg.get(key)
            if value not in (None, "", {}):
                entry[key] = value
        material[name] = entry
    return material


def find_material_config_drift(
    targets: list[str],
    *,
    shared_config_path_fn: Callable[[], Path],
    worktree_config_path_fn: Callable[[], Path],
    config_material_for_targets_fn: Callable[[dict, list[str]], dict],
) -> list[str]:
    shared_path = shared_config_path_fn()
    worktree_path = worktree_config_path_fn()
    if not shared_path.exists() or not worktree_path.exists():
        return []
    try:
        shared_cfg = json.loads(shared_path.read_text())
        worktree_cfg = json.loads(worktree_path.read_text())
    except json.JSONDecodeError:
        return []

    drift: list[str] = []
    shared_material = config_material_for_targets_fn(shared_cfg, targets)
    worktree_material = config_material_for_targets_fn(worktree_cfg, targets)
    for name in targets:
        shared_entry = shared_material.get(name)
        worktree_entry = worktree_material.get(name)
        if shared_entry == worktree_entry:
            continue
        drift.append(
            f"{name}: shared-state {shared_entry or '(missing)'} vs worktree-local {worktree_entry or '(missing)'}"
        )
    return drift


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
        errors = [e for e in errors if not any(t in e for t in namespace_failover_targets)]

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


def print_submission_metadata(
    metadata: dict,
    *,
    short_sha_fn: Callable[[str], str],
    provenance_summary_fn: Callable[[dict | None], str],
    print_fn: Callable[..., None],
) -> None:
    print_fn(
        "Submitting: "
        f"{metadata['branch']} @ {short_sha_fn(metadata['sha'])} "
        f"priority={metadata['priority']} targets={','.join(metadata['targets']) or 'none'}"
    )
    print_fn(f"  root: {metadata['submitted_root']}")
    print_fn(f"  cwd: {metadata['cwd']}")
    if metadata.get("cwd_git_root"):
        print_fn(f"  cwd git root: {metadata['cwd_git_root']}")
    print_fn(f"  config: {metadata['config_path']} ({metadata['config_source']})")
    if metadata.get("provenance"):
        print_fn(f"  provenance: {provenance_summary_fn(metadata.get('provenance'))}")
    for drift in metadata.get("config_drift", []):
        print_fn(f"  config drift: {drift}")
    for target_name in metadata.get("targets", []):
        state = metadata.get("target_hosts", {}).get(target_name, {})
        transport = state.get("transport_mode", "local")
        if transport == "local":
            print_fn(f"  {target_name}: local transport")
            continue
        resolved = state.get("resolved_host") or state.get("configured_host") or "?"
        status = state.get("status", "unknown")
        repo_path = state.get("repo_path") or "?"
        print_fn(f"  {target_name}: host={resolved} status={status} transport={transport} repo={repo_path}")
    for warning in metadata.get("warnings", []):
        print_fn(f"  warning: {warning}")

"""Target reachability and submission preflight helpers for local CI."""

from __future__ import annotations

from collections.abc import Callable, Mapping
import json
import shlex
import subprocess
from pathlib import Path


def ssh_probe(
    host: str,
    timeout: int = 5,
    *,
    run_ssh_subprocess_fn: Callable[..., subprocess.CompletedProcess[str]],
) -> subprocess.CompletedProcess[str]:
    cmd = ["ssh", "-o", f"ConnectTimeout={timeout}", "-o", "BatchMode=yes", host, "echo", "up"]
    try:
        return run_ssh_subprocess_fn(
            cmd,
            timeout=max(timeout, 5),
        )
    except subprocess.TimeoutExpired:
        return subprocess.CompletedProcess(cmd, 124, "", f"SSH probe timed out after {max(timeout, 5)}s")


def ssh_reachable(
    host: str,
    timeout: int = 5,
    *,
    ssh_probe_fn: Callable[[str, int], subprocess.CompletedProcess[str]],
) -> bool:
    return ssh_probe_fn(host, timeout).returncode == 0


def ssh_failure_detail(
    host: str,
    timeout: int = 5,
    *,
    ssh_probe_fn: Callable[[str, int], subprocess.CompletedProcess[str]],
) -> str:
    result = ssh_probe_fn(host, timeout)
    stderr = (result.stderr or "").strip()
    if "timed out" in stderr.lower():
        return f"{host} (connection timed out; verify network reachability and SSH service on the target)"
    if "Connection reset by peer" in stderr or "kex_exchange_identification" in stderr:
        return f"{host} (SSH service reset during handshake; verify OpenSSH server on the target)"
    if "Connection refused" in stderr:
        return f"{host} (connection refused; verify the SSH service is running on the target)"
    if stderr:
        first_line = stderr.splitlines()[0].strip()
        return f"{host} ({first_line})"
    return f"{host} (unreachable; verify SSH access from this host)"


def ssh_command_result(
    host: str,
    remote_cmd: str,
    *,
    timeout: int = 30,
    run_ssh_subprocess_fn: Callable[..., subprocess.CompletedProcess[str]],
) -> subprocess.CompletedProcess[str]:
    prefixed_cmd = 'export PATH="$HOME/.local/bin:$PATH"; ' + remote_cmd
    return run_ssh_subprocess_fn(
        ["ssh", "-o", f"ConnectTimeout={max(5, min(timeout, 30))}", host, "bash", "-lc", shlex.quote(prefixed_cmd)],
        timeout=timeout,
    )


def utmctl_vm_status(
    vm_name: str,
    *,
    run_fn: Callable[..., subprocess.CompletedProcess[str]],
) -> str | None:
    result = run_fn(["utmctl", "list"], capture_output=True, text=True)
    if result.returncode != 0:
        return None
    for line in result.stdout.splitlines():
        if vm_name in line:
            parts = line.split()
            if len(parts) >= 2:
                return parts[1]
    return None


def utmctl_start(
    vm_name: str,
    *,
    run_fn: Callable[..., subprocess.CompletedProcess[str]],
) -> bool:
    result = run_fn(["utmctl", "start", vm_name], capture_output=True, text=True)
    return result.returncode == 0


def ensure_host_reachable(
    target_name: str,
    target_cfg: dict,
    defaults: dict,
    *,
    ssh_reachable_fn: Callable[[str, int], bool],
    utmctl_vm_status_fn: Callable[[str], str | None],
    utmctl_start_fn: Callable[[str], bool],
    time_fn: Callable[[], float],
    sleep_fn: Callable[[float], None],
    print_fn: Callable[..., None],
) -> str | None:
    host = target_cfg["host"]
    fallback_host = target_cfg.get("fallback_host")
    timeout = defaults.get("ssh_timeout_secs", 5)

    print_fn(f"  [{target_name}] Checking ssh {host}...")
    if ssh_reachable_fn(host, timeout):
        print_fn(f"  [{target_name}] {host} is up")
        return host

    if fallback_host:
        print_fn(f"  [{target_name}] {host} unreachable, trying fallback ssh {fallback_host}...")
        if ssh_reachable_fn(fallback_host, timeout):
            print_fn(f"  [{target_name}] {fallback_host} is up")
            return fallback_host

    fallback = target_cfg.get("utm_fallback")
    if not fallback:
        print_fn(f"  [{target_name}] {host} unreachable, no UTM fallback configured")
        return None

    vm_name = fallback["vm_name"]
    boot_wait = fallback.get("boot_wait_secs", 30)
    ssh_retry = fallback.get("ssh_retry_secs", 60)

    print_fn(f"  [{target_name}] {host} unreachable, checking UTM VM '{vm_name}'...")
    status = utmctl_vm_status_fn(vm_name)
    if status is None:
        print_fn(f"  [{target_name}] UTM VM '{vm_name}' not found")
        return None

    if status != "started":
        print_fn(f"  [{target_name}] Starting UTM VM '{vm_name}'...")
        if not utmctl_start_fn(vm_name):
            print_fn(f"  [{target_name}] Failed to start UTM VM")
            return None
        print_fn(f"  [{target_name}] Waiting {boot_wait}s for boot...")
        sleep_fn(boot_wait)

    deadline = time_fn() + ssh_retry
    attempt = 0
    while time_fn() < deadline:
        attempt += 1
        if ssh_reachable_fn(host, timeout):
            print_fn(f"  [{target_name}] {host} up after UTM start (attempt {attempt})")
            return host
        sleep_fn(5)

    print_fn(f"  [{target_name}] {host} still unreachable after UTM start")
    return None


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


def preflight_target_host_state(
    target_name: str,
    target_cfg: dict,
    defaults: dict,
    *,
    ssh_reachable_fn: Callable[[str, int], bool],
) -> dict:
    target_type = target_cfg.get("type", "local")
    if target_type != "ssh":
        return {"target": target_name, "transport_mode": "local", "status": "local"}

    host = target_cfg.get("host", "")
    fallback_host = target_cfg.get("fallback_host")
    timeout = defaults.get("ssh_timeout_secs", 5)
    state = {
        "target": target_name,
        "transport_mode": "bundle",
        "configured_host": host,
        "repo_path": target_cfg.get("repo_path"),
        "status": "unknown",
    }

    if host and ssh_reachable_fn(host, timeout):
        state["status"] = "primary-up"
        state["resolved_host"] = host
        return state

    if fallback_host and ssh_reachable_fn(fallback_host, timeout):
        state["status"] = "fallback-up"
        state["resolved_host"] = fallback_host
        state["warning"] = f"{target_name}: primary host {host} is down; fallback {fallback_host} is up"
        return state

    utm_fallback = target_cfg.get("utm_fallback")
    if utm_fallback:
        vm_name = utm_fallback.get("vm_name", "(unknown)")
        state["status"] = "utm-fallback-pending"
        state["resolved_host"] = host
        state["warning"] = f"{target_name}: ssh host {host} is down; queued run would need UTM fallback '{vm_name}'"
        return state

    state["status"] = "unreachable"
    state["resolved_host"] = host
    state["error"] = f"{target_name}: ssh host {host} is down and no fallback host or UTM VM is configured"
    return state


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

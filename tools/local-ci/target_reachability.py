"""Target reachability helpers for local CI."""

from __future__ import annotations

from collections.abc import Callable
import shlex
import subprocess


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

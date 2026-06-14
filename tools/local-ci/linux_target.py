"""Linux desktop target probe and command-building helpers for local CI."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path
import subprocess

from linux_target_commands import (
    build_linux_window_driver_remote_command,
    build_linux_xvfb_remote_command,
)


LINUX_REQUIRED_REMOTE_TOOLS = {
    "git": {"display_name": "git", "package_hint": "sudo apt-get install -y git"},
    "git_lfs": {"display_name": "git-lfs", "package_hint": "sudo apt-get install -y git-lfs && git lfs install"},
    "xvfb_run": {"display_name": "xvfb-run", "package_hint": "sudo apt-get install -y xvfb xauth"},
    "xauth": {"display_name": "xauth", "package_hint": "sudo apt-get install -y xvfb xauth"},
    "xdotool": {"display_name": "xdotool", "package_hint": "sudo apt-get install -y xdotool"},
    "xwininfo": {"display_name": "xwininfo", "package_hint": "sudo apt-get install -y x11-utils"},
    "import": {"display_name": "import", "package_hint": "sudo apt-get install -y imagemagick"},
}
LINUX_OPTIONAL_REMOTE_TOOLS = {
    "wmctrl": {"display_name": "wmctrl", "package_hint": "sudo apt-get install -y wmctrl"},
}


def probe_linux_launch_backend(
    host: str,
    *,
    ssh_command_result_fn: Callable[..., subprocess.CompletedProcess[str]],
) -> dict:
    remote_cmd = """if command -v xvfb-run >/dev/null 2>&1; then
  printf 'mode=xvfb\npath=%s\n' "$(command -v xvfb-run)"
  exit 0
fi
display=''
for sock in /tmp/.X11-unix/X*; do
  [ -S "$sock" ] || continue
  base=$(basename "$sock")
  display=":${base#X}"
  break
done
xdg_runtime_dir=''
candidate="/run/user/$(id -u)"
if [ -d "$candidate" ]; then
  xdg_runtime_dir="$candidate"
fi
if [ -n "$display" ]; then
  printf 'mode=display\ndisplay=%s\n' "$display"
  if [ -n "$xdg_runtime_dir" ]; then
    printf 'xdg_runtime_dir=%s\n' "$xdg_runtime_dir"
  fi
else
  printf 'mode=missing\n'
fi"""
    run = ssh_command_result_fn(host, remote_cmd, timeout=30)
    if run.returncode != 0:
        detail = run.stderr.strip() or run.stdout.strip() or f"ssh exited {run.returncode}"
        raise RuntimeError(detail)
    backend: dict[str, str] = {}
    for line in run.stdout.splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        backend[key.strip()] = value.strip()
    backend.setdefault("mode", "missing")
    return backend


def probe_linux_remote_tooling(
    host: str,
    *,
    ssh_command_result_fn: Callable[..., subprocess.CompletedProcess[str]],
) -> dict:
    remote_cmd = r"""
probe_tool() {
  key="$1"
  cmd="$2"
  shift 2
  if command -v "$cmd" >/dev/null 2>&1; then
    path=$(command -v "$cmd")
    version=$("$cmd" "$@" 2>&1 | head -n 1 || true)
    printf '%s_found=true\n' "$key"
    printf '%s_path=%s\n' "$key" "$path"
    printf '%s_version=%s\n' "$key" "$version"
  else
    printf '%s_found=false\n' "$key"
  fi
}
probe_git_lfs() {
  if git lfs version >/dev/null 2>&1; then
    path=$(command -v git-lfs || true)
    version=$(git lfs version 2>&1 | head -n 1 || true)
    if [ -z "$path" ]; then
      path="git lfs"
    fi
    printf 'git_lfs_found=true\n'
    printf 'git_lfs_path=%s\n' "$path"
    printf 'git_lfs_version=%s\n' "$version"
  elif [ -x "$HOME/.local/bin/git-lfs" ]; then
    path="$HOME/.local/bin/git-lfs"
    version=$("$path" version 2>&1 | head -n 1 || true)
    printf 'git_lfs_found=false\n'
    printf 'git_lfs_path=%s\n' "$path"
    printf 'git_lfs_version=%s\n' "$version"
    printf 'git_lfs_hint=installed at %s but unavailable to non-interactive shells; add $HOME/.local/bin to PATH or install git-lfs system-wide\n' "$path"
  else
    printf 'git_lfs_found=false\n'
  fi
}
probe_tool git git --version
probe_git_lfs
probe_tool xvfb_run xvfb-run --help
probe_tool xauth xauth -V
probe_tool xdotool xdotool -v
probe_tool xwininfo xwininfo -version
probe_tool import import -version
probe_tool wmctrl wmctrl -V
"""
    run = ssh_command_result_fn(host, remote_cmd, timeout=30)
    if run.returncode != 0:
        detail = run.stderr.strip() or run.stdout.strip() or f"ssh exited {run.returncode}"
        raise RuntimeError(detail)
    result: dict[str, str] = {}
    for line in run.stdout.splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        result[key.strip()] = value.strip()
    return result


def linux_tooling_detail(probe: dict, tool_name: str, *, missing_hint: str | None = None) -> str:
    if probe.get(f"{tool_name}_found"):
        version = (probe.get(f"{tool_name}_version") or "").strip()
        path = probe.get(f"{tool_name}_path") or tool_name
        return f"{version} ({path})" if version else path
    hint = (probe.get(f"{tool_name}_hint") or "").strip()
    if hint:
        return hint
    if missing_hint:
        return missing_hint
    return "missing"


def linux_remote_tooling_ready(probe: dict, *, required_tools: dict = LINUX_REQUIRED_REMOTE_TOOLS) -> bool:
    return all(bool(probe.get(f"{tool_name}_found")) for tool_name in required_tools)


def remote_linux_bundle_relpath(target_name: str, action_name: str, bundle_dir: Path) -> str:
    return f".local/state/pulp/desktop-automation/remote/{target_name}/{action_name}/{bundle_dir.name}"

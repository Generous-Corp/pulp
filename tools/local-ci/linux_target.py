"""Linux desktop target probe and command-building helpers for local CI."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path
import shlex
import subprocess


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


def build_linux_xvfb_remote_command(
    repo_path: str,
    remote_bundle_relpath: str,
    command: str,
    *,
    launch_backend: dict | None = None,
    launch_cwd: str | None = None,
    capture_ui_snapshot: bool,
    click_point: str | None,
    click_view_id: str | None,
    click_view_type: str | None,
    click_view_text: str | None,
    click_view_label: str | None,
    capture_before: bool,
    settle_secs: float,
) -> str:
    remote_bundle_expr = f"$HOME/{remote_bundle_relpath}"
    launch_cwd_value = launch_cwd or repo_path
    backend = dict(launch_backend or {})
    if launch_cwd_value.startswith("$HOME/"):
        launch_cwd_assignment = f"launch_cwd={launch_cwd_value}"
    else:
        launch_cwd_assignment = f"launch_cwd={shlex.quote(launch_cwd_value)}"
    exports = [
        f"export PULP_AUTOMATION_AFTER_OUT={shlex.quote(remote_bundle_expr + '/screenshots/window.png')}",
        f"export PULP_AUTOMATION_DELAY_MS={shlex.quote('1000')}",
        f"export PULP_AUTOMATION_AFTER_DELAY_MS={shlex.quote(str(max(0, int(settle_secs * 1000.0))))}",
        f"export PULP_AUTOMATION_EXIT_AFTER={shlex.quote('1')}",
    ]
    if capture_ui_snapshot:
        exports.append(f"export PULP_VIEW_TREE_OUT={shlex.quote(remote_bundle_expr + '/ui-tree.json')}")
    if capture_before:
        exports.append(f"export PULP_AUTOMATION_BEFORE_OUT={shlex.quote(remote_bundle_expr + '/screenshots/before.png')}")
    if click_point:
        exports.append(f"export PULP_AUTOMATION_CLICK_POINT={shlex.quote(click_point)}")
    if click_view_id:
        exports.append(f"export PULP_AUTOMATION_CLICK_VIEW_ID={shlex.quote(click_view_id)}")
    if click_view_type:
        exports.append(f"export PULP_AUTOMATION_CLICK_VIEW_TYPE={shlex.quote(click_view_type)}")
    if click_view_text:
        exports.append(f"export PULP_AUTOMATION_CLICK_VIEW_TEXT={shlex.quote(click_view_text)}")
    if click_view_label:
        exports.append(f"export PULP_AUTOMATION_CLICK_VIEW_LABEL={shlex.quote(click_view_label)}")
    if backend.get("mode") == "display":
        exports.append(f"export DISPLAY={shlex.quote(backend.get('display') or ':0')}")
        if backend.get("xdg_runtime_dir"):
            exports.append(f"export XDG_RUNTIME_DIR={shlex.quote(backend['xdg_runtime_dir'])}")

    launch_driver = "xvfb-run -a"
    if backend.get("mode") == "display":
        launch_driver = ""
    launch_command = f"bash -lc {shlex.quote(command)}"
    if launch_driver:
        launch_command = f"{launch_driver} {launch_command}"

    parts = [
        "set -euo pipefail",
        f"repo_path={shlex.quote(repo_path)}",
        launch_cwd_assignment,
        f'remote_bundle="$HOME/{remote_bundle_relpath}"',
        'mkdir -p "$remote_bundle/screenshots"',
        'cd "$launch_cwd"',
        *exports,
        f'{launch_command} > "$remote_bundle/stdout.log" 2> "$remote_bundle/stderr.log"',
    ]
    return "; ".join(parts)


def build_linux_window_driver_remote_command(
    repo_path: str,
    remote_bundle_relpath: str,
    command: str,
    *,
    launch_backend: dict | None = None,
    launch_cwd: str | None = None,
    click_point: str | None,
    capture_before: bool,
    settle_secs: float,
    parse_coordinate_pair_fn: Callable[..., tuple[float, float]],
) -> str:
    backend = dict(launch_backend or {})
    launch_cwd_value = launch_cwd or repo_path
    if launch_cwd_value.startswith("$HOME/"):
        launch_cwd_assignment = f"launch_cwd={launch_cwd_value}"
    else:
        launch_cwd_assignment = f"launch_cwd={shlex.quote(launch_cwd_value)}"

    click_lines: list[str] = []
    if click_point:
        click_x, click_y = parse_coordinate_pair_fn(click_point, flag_name="--click")
        click_lines.extend([
            f'xdotool mousemove --window "$window_id" {click_x} {click_y}',
            'xdotool click 1',
        ])

    settle_delay = max(0.0, settle_secs)
    x11_window_scan = "awk '/^[[:space:]]*0x[0-9A-Fa-f]+/ {print $1}'"
    inner_lines = [
        "set -euo pipefail",
        launch_cwd_assignment,
        f'remote_bundle="$HOME/{remote_bundle_relpath}"',
        'mkdir -p "$remote_bundle/screenshots"',
        'cd "$launch_cwd"',
        f'xwininfo -root -tree 2>/dev/null | {x11_window_scan} > "$remote_bundle/windows.before" || true',
        f'bash -lc {shlex.quote(command)} > "$remote_bundle/stdout.log" 2> "$remote_bundle/stderr.log" &',
        "app_pid=$!",
        'printf "%s\n" "$app_pid" > "$remote_bundle/pid.txt"',
        'window_id=""',
        "for _ in $(seq 1 200); do",
        f'  xwininfo -root -tree 2>/dev/null | {x11_window_scan} > "$remote_bundle/windows.after" || true',
        '  window_id=$(grep -Fvx -f "$remote_bundle/windows.before" "$remote_bundle/windows.after" 2>/dev/null | head -n1 || true)',
        '  if [ -n "$window_id" ]; then',
        "    break",
        "  fi",
        "  sleep 0.1",
        "done",
        'if [ -z "$window_id" ]; then',
        '  echo "No top-level X11 window detected for launch command" >&2',
        '  kill "$app_pid" >/dev/null 2>&1 || true',
        '  wait "$app_pid" >/dev/null 2>&1 || true',
        "  exit 21",
        "fi",
        'printf "%s\n" "$window_id" > "$remote_bundle/window-id.txt"',
        'xdotool getwindowname "$window_id" > "$remote_bundle/window-title.txt" 2>/dev/null || true',
        'xdotool windowactivate --sync "$window_id" >/dev/null 2>&1 || true',
        "sleep 0.2",
    ]
    if capture_before:
        inner_lines.append('import -window "$window_id" png:"$remote_bundle/screenshots/before.png"')
    inner_lines.extend(click_lines)
    if settle_delay > 0.0:
        inner_lines.append(f"sleep {settle_delay:.3f}")
    inner_lines.extend([
        'import -window "$window_id" png:"$remote_bundle/screenshots/window.png"',
        'kill "$app_pid" >/dev/null 2>&1 || true',
        'wait "$app_pid" >/dev/null 2>&1 || true',
    ])
    wrapped_script = "\n".join(inner_lines)
    if backend.get("mode") == "display":
        prefix_lines = [f'export DISPLAY={shlex.quote(backend.get("display") or ":0")}']
        if backend.get("xdg_runtime_dir"):
            prefix_lines.append(f'export XDG_RUNTIME_DIR={shlex.quote(backend["xdg_runtime_dir"])}')
        wrapped_script = "\n".join(prefix_lines + [wrapped_script])
        return f"bash -lc {shlex.quote(wrapped_script)}"
    return f"xvfb-run -a bash -lc {shlex.quote(wrapped_script)}"

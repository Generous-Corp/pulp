"""Linux desktop remote command builders."""
from __future__ import annotations

from collections.abc import Callable
import shlex


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

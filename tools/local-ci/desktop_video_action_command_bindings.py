"""Bindings from the local_ci facade to the desktop `video` action command."""

from __future__ import annotations

from collections.abc import Mapping
import json
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


DESKTOP_VIDEO_ACTION_COMMAND_EXPORTS = (
    "cmd_desktop_video",
)


def _terminal_stdout(result: Mapping[str, Any]) -> str:
    stdout = str(result.get("stdout") or "")
    cleanup = result.get("terminal_cleanup")
    title = result.get("terminal_title")
    if not stdout or not (cleanup or title):
        return stdout
    try:
        payload = json.loads(stdout)
    except json.JSONDecodeError:
        return stdout
    if not isinstance(payload, dict):
        return stdout
    payload["terminal_reinvoke"] = {
        "title": title,
        "cleanup": cleanup,
        "timed_out": bool(result.get("timed_out")),
    }
    return json.dumps(payload, indent=2) + "\n"


def _maybe_run_local_ci_in_terminal(bindings: Mapping[str, Any], args: Any) -> int | None:
    if not getattr(args, "run_in_terminal", False):
        return None
    sys_mod = _binding(bindings, "sys")
    os_mod = _binding(bindings, "os")
    terminal_runner = _binding(bindings, "_macos_terminal_runner")
    if not terminal_runner.should_reinvoke_in_terminal(
        requested=True,
        sys_platform=sys_mod.platform,
        environ=os_mod.environ,
    ):
        return None
    result = terminal_runner.run_local_ci_in_terminal(
        sys_mod.argv[1:],
        cwd=_binding(bindings, "ROOT"),
        python_executable=sys_mod.executable,
        script_path=_binding(bindings, "ROOT") / "tools" / "local-ci" / "local_ci.py",
    )
    if result.get("stdout"):
        sys_mod.stdout.write(_terminal_stdout(result))
    if result.get("stderr"):
        sys_mod.stderr.write(result["stderr"])
    terminal_title = result.get("terminal_title")
    if terminal_title and not result.get("terminal_cleanup"):
        terminal_runner.close_terminal_windows_with_title(terminal_title)
    return int(result["returncode"])


def cmd_desktop_video(bindings: Mapping[str, Any], args: Any) -> int:
    terminal_result = _maybe_run_local_ci_in_terminal(bindings, args)
    if terminal_result is not None:
        return terminal_result
    return _binding(bindings, "_desktop_video_action_commands_cli").cmd_desktop_video(
        args,
        cmd_desktop_smoke_fn=lambda video_args: _binding(bindings, "cmd_desktop_smoke")(video_args),
        cmd_desktop_click_fn=lambda video_args: _binding(bindings, "cmd_desktop_click")(video_args),
        cmd_desktop_inspect_fn=lambda video_args: _binding(bindings, "cmd_desktop_inspect")(video_args),
    )


def install_desktop_video_action_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_VIDEO_ACTION_COMMAND_EXPORTS,
) -> None:
    known_names = set(DESKTOP_VIDEO_ACTION_COMMAND_EXPORTS)
    command_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), command_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)

"""Desktop launch command rewrite helpers."""

from __future__ import annotations

from pathlib import Path
import shlex
import subprocess


def command_path_rewrite_candidate(token: str, *, root: Path) -> Path | None:
    if not token:
        return None
    candidate = Path(token).expanduser()
    if candidate.is_absolute():
        try:
            candidate.relative_to(root)
        except ValueError:
            return None
        return candidate
    if token.startswith("./") or token.startswith("../") or token.startswith(".\\") or token.startswith("..\\"):
        normalized = Path(token.replace("\\", "/"))
        return root / normalized
    return None


def rewrite_launch_command_for_mapper(
    command: str | None,
    mapper,
    *,
    root: Path,
    windows: bool = False,
) -> str | None:
    if not command:
        return command
    try:
        args = shlex.split(command)
    except ValueError:
        return command
    if args and ("\\" in command or args[0].startswith(".") and "\\" not in args[0] and "\\\\" in command):
        try:
            windows_args = shlex.split(command, posix=False)
        except ValueError:
            windows_args = []
        if windows_args:
            args = windows_args
    if not args:
        return command
    token = args[0]
    if len(token) >= 2 and token[0] == token[-1] and token[0] in {"'", '"'}:
        token = token[1:-1]
    candidate = command_path_rewrite_candidate(token, root=root)
    if candidate is not None:
        rel = candidate.relative_to(root)
        args[0] = mapper(rel)
    if windows:
        return subprocess.list2cmdline(args)
    return " ".join(shlex.quote(part) for part in args)

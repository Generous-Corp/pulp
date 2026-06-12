"""Desktop source-preparation helpers for local CI."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
from typing import Callable

from source_prep_exact import (
    local_worktree_matches,
    prepare_linux_exact_sha_source,
    prepare_macos_exact_sha_source,
    prepare_windows_exact_sha_source,
    reset_local_worktree,
)


def make_desktop_source_request(
    args: argparse.Namespace,
    *,
    normalize_desktop_source_mode_fn: Callable[[str | None], str],
    current_branch_fn: Callable[[], str],
    current_sha_fn: Callable[[], str],
) -> dict:
    mode = normalize_desktop_source_mode_fn(getattr(args, "source_mode", "live"))
    return {
        "mode": mode,
        "branch": getattr(args, "branch", None) or current_branch_fn(),
        "sha": getattr(args, "sha", None) or current_sha_fn(),
        "prepare_command": (getattr(args, "prepare_command", None) or "").strip() or None,
        "prepare_timeout_secs": float(getattr(args, "prepare_timeout", 900.0) or 900.0),
    }


def desktop_source_cache_key(source_request: dict) -> str:
    raw = json.dumps(
        {
            "sha": source_request.get("sha"),
            "prepare_command": source_request.get("prepare_command") or "",
        },
        sort_keys=True,
    )
    return hashlib.sha256(raw.encode("utf-8")).hexdigest()[:12]


def desktop_source_root(
    target_name: str,
    source_request: dict,
    *,
    state_dir_fn: Callable[[], Path],
) -> Path:
    return state_dir_fn() / "desktop-source" / target_name / desktop_source_cache_key(source_request)


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


def rewrite_launch_command_for_source_root(command: str | None, source_root: Path, *, root: Path) -> str | None:
    return rewrite_launch_command_for_mapper(command, lambda rel: str(source_root / rel), root=root)


def rewrite_launch_command_for_posix_root(command: str | None, remote_root: str, *, root: Path) -> str | None:
    return rewrite_launch_command_for_mapper(command, lambda rel: f"{remote_root}/{rel.as_posix()}", root=root)


def rewrite_launch_command_for_windows_root(
    command: str | None,
    remote_root: str,
    *,
    root: Path,
    windows_path_join_fn: Callable[..., str],
) -> str | None:
    return rewrite_launch_command_for_mapper(
        command,
        lambda rel: windows_path_join_fn(remote_root, str(rel).replace("/", "\\")),
        root=root,
        windows=True,
    )


def split_windows_prepare_commands(command: str) -> list[str]:
    parts: list[str] = []
    current: list[str] = []
    quote: str | None = None
    for ch in command:
        if quote is not None:
            current.append(ch)
            if ch == quote:
                quote = None
            continue
        if ch in {"'", '"'}:
            quote = ch
            current.append(ch)
            continue
        if ch in {";", "\n"}:
            segment = "".join(current).strip()
            if segment:
                parts.append(segment)
            current = []
            continue
        current.append(ch)
    segment = "".join(current).strip()
    if segment:
        parts.append(segment)
    return parts


def validate_windows_prepare_commands(commands: list[str]) -> None:
    suspicious = [cmd for cmd in commands if re.search(r"(^|[\s=])'[^']+'(?=$|[\s&|;])", cmd)]
    if suspicious:
        sample = suspicious[0]
        raise ValueError(
            "Windows prepare commands run under cmd.exe, where single-quoted tokens are literal text. "
            "Use double quotes for paths, generator names, and arguments instead. "
            f"Suspicious command: {sample}"
        )


def attach_desktop_source_to_manifest(manifest: dict, source_context: dict | None) -> None:
    if not source_context:
        return
    source_manifest = {
        "mode": source_context.get("mode", "live"),
        "branch": source_context.get("branch"),
        "sha": source_context.get("sha"),
        "prepare_command": source_context.get("prepare_command"),
        "prepare_timeout_secs": source_context.get("prepare_timeout_secs"),
        "prepared_root": source_context.get("prepared_root_display", source_context.get("prepared_root")),
        "launch_cwd": source_context.get("launch_cwd_display", source_context.get("launch_cwd")),
    }
    manifest["source"] = source_manifest
    prepare_log = source_context.get("prepare_log")
    if prepare_log:
        manifest.setdefault("artifacts", {})["prepare_log"] = str(prepare_log)

"""The one env-path fence for optional external tools.

Every opt-in integration in the lab (MIR oracle, perceptual model, advisory
reviewer, image artifacts) takes its binary from an environment variable and
degrades to `skipped` with the reason when it is unset or not found. This is
that lookup, written once, so the wording and the rules (a name on PATH or an
existing path; a command string may carry arguments) cannot drift between
modules.
"""
from __future__ import annotations

import os
import shlex
import shutil


def resolve_env_binary(env_var: str, purpose: str) -> tuple[str | None, str]:
    """Return (resolved binary path, "") or (None, reason) for `env_var`.
    `purpose` names the integration in the skip reason, e.g. "MIR oracle"."""
    path = os.environ.get(env_var, "").strip()
    if not path:
        return None, f"{env_var} not set (opt-in {purpose}; skipping)"
    resolved = shutil.which(path) or (path if os.path.exists(path) else None)
    if not resolved:
        return None, f"{env_var}={path} not found on disk/PATH; skipping"
    return resolved, ""


def resolve_env_command(env_var: str, purpose: str) -> tuple[list[str] | None, str]:
    """Like `resolve_env_binary` for a variable holding a command line: the first
    word is resolved as a binary and the remaining words are kept as arguments."""
    cmd = os.environ.get(env_var, "").strip()
    if not cmd:
        return None, f"{env_var} not set (opt-in {purpose}; skipping)"
    parts = shlex.split(cmd)
    binary = shutil.which(parts[0]) or (parts[0] if os.path.exists(parts[0]) else None)
    if not binary:
        return None, f"{env_var} provider {parts[0]!r} not found on disk/PATH; skipping"
    return [binary, *parts[1:]], ""

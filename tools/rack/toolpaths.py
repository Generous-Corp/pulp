#!/usr/bin/env python3
"""Where the tools are, for a process that did not inherit a login shell.

A non-interactive SSH session gets a minimal PATH: no Homebrew, no /usr/local.
That is fine until something we shell out to shells out again -- the `claude`
CLI runs plugin hooks with `node`, and on a machine where node lives in
/opt/homebrew the hook dies with "node: command not found". The failure names
node, mentions a hook nobody here wrote, and looks nothing like what it is,
which is that the PATH is short.

This is also the one place that answers "where is claude". There were two
implementations of that before this file, in two scripts, and they had already
started to drift.
"""

from __future__ import annotations

import os
import shutil

# Everywhere a Mac keeps user-installed tools. Ordered: a version a person
# chose beats one that arrived with the system.
TOOL_DIRS = [
    os.path.expanduser("~/.local/bin"),
    os.path.expanduser("~/.claude/local"),
    os.path.expanduser("~/bin"),
    "/opt/homebrew/bin",
    "/usr/local/bin",
    "/usr/bin",
    "/bin",
]


def enriched_path(base: str | None = None) -> str:
    """PATH with the usual tool locations appended, none of them duplicated."""
    seen: list[str] = []
    for entry in (base if base is not None else os.environ.get("PATH", "")).split(":"):
        if entry and entry not in seen:
            seen.append(entry)
    for entry in TOOL_DIRS:
        if os.path.isdir(entry) and entry not in seen:
            seen.append(entry)
    return ":".join(seen)


def tool_env(extra: dict | None = None) -> dict:
    """The environment to run a tool in, so its own children can be found."""
    env = dict(os.environ)
    env["PATH"] = enriched_path(env.get("PATH"))
    if extra:
        env.update(extra)
    return env


def find_claude() -> str:
    """The claude binary, or a clear failure naming where we looked.

    Searched against the enriched PATH rather than the inherited one: an app
    launched from Finder inherits almost nothing, which is how "Build" once
    failed with FileNotFoundError on a machine where claude was plainly
    installed.
    """
    found = shutil.which("claude", path=enriched_path())
    if found:
        return found
    for directory in TOOL_DIRS:
        candidate = os.path.join(directory, "claude")
        if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return candidate
    raise SystemExit(
        "could not find `claude`. Looked on PATH and in:\n  " +
        "\n  ".join(TOOL_DIRS))

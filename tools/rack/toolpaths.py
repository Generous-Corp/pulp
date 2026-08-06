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
import sys

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
    """The model CLI, or a clear failure naming what to install.

    Searched against the enriched PATH rather than the inherited one: an app
    launched from Finder inherits almost nothing, which is how "Build" once
    failed with FileNotFoundError on a machine where claude was plainly
    installed.

    THE ONE ANSWER TO THIS QUESTION. patch.py kept a second copy that ended
    `return "claude"` when it found nothing -- a bare string, handed straight
    to subprocess, so a machine without it got
    `FileNotFoundError: [Errno 2] ... 'claude'` as the entire explanation of
    why nothing was generated. Every beta user's machine is that machine.
    """
    # An explicit choice beats a search. Both agents drive this generator, and
    # a shim may be the only thing named `claude` on either one's PATH.
    for var in ("FORGE_CLAUDE_BIN", "FORGE_CODEX_BIN", "CODEX_BIN"):
        chosen = os.environ.get(var)
        if chosen and os.path.isfile(chosen) and os.access(chosen, os.X_OK):
            return chosen

    for name in ("claude", "codex"):
        found = shutil.which(name, path=enriched_path())
        if found:
            return found
        for directory in TOOL_DIRS:
            candidate = os.path.join(directory, name)
            if os.path.isfile(candidate) and os.access(candidate, os.X_OK):
                return candidate

    # Named and actionable, never a traceback. This is a real prerequisite:
    # the model is not bundled and there is no API-key path, so without it
    # nothing generates -- not a module, not even a patch.
    raise SystemExit(
        "the model CLI is not installed, so nothing can be generated.\n"
        "  Forge Modular writes patches and modules with Claude Code, which is\n"
        "  a separate free download and not something we can ship for you.\n"
        "  Install it, run `claude` once to sign in, then try again:\n"
        "      https://claude.com/claude-code\n"
        "  If it is already installed somewhere unusual, point us at it:\n"
        "      export FORGE_CLAUDE_BIN=/full/path/to/claude\n"
        "  Looked on PATH and in:\n    " + "\n    ".join(TOOL_DIRS))


def model_cli_kind(executable: str) -> str:
    """Return the selected CLI's argument protocol, or refuse ambiguity.

    Claude and Codex are both valid model backends, but their non-interactive
    flags and streaming formats are unrelated. An explicit environment
    variable may name an arbitrarily named wrapper; otherwise only the real
    CLI names identify a protocol.
    """
    chosen = os.path.normcase(os.path.realpath(os.path.abspath(executable)))
    matches = set()
    for var, kind in (("FORGE_CLAUDE_BIN", "claude"),
                      ("FORGE_CODEX_BIN", "codex"),
                      ("CODEX_BIN", "codex")):
        value = os.environ.get(var)
        explicit = (os.path.normcase(os.path.realpath(os.path.abspath(value)))
                    if value else None)
        if explicit == chosen:
            matches.add(kind)
    if len(matches) > 1:
        raise SystemExit(
            f"the model CLI {executable!r} is selected as both Claude and "
            "Codex; set only the matching FORGE_*_BIN variable")
    if matches:
        return matches.pop()

    name = os.path.basename(executable)
    if name in ("claude", "codex"):
        return name
    raise SystemExit(
        f"cannot determine the argument protocol for model CLI {executable!r}.\n"
        "  Name the executable `claude` or `codex`, or identify an arbitrary "
        "wrapper explicitly with FORGE_CLAUDE_BIN or FORGE_CODEX_BIN.")


def missing_prerequisites() -> list:
    """Everything this machine needs and does not have, in one list.

    A prerequisite discovered one at a time, minutes apart, at the end of
    successive failed runs is the worst possible way to learn what is needed.
    Asked all at once, it is a short shopping list.
    """
    import subprocess

    out = []
    try:
        find_claude()
    except SystemExit:
        out.append(
            "Claude Code (the model that writes the patch) is not installed. "
            "It is a free separate download: https://claude.com/claude-code")

    # python3 is running this, so it is present by definition -- but on macOS
    # /usr/bin/python3 is the Xcode shim, and the SAME missing component means
    # no compiler either. Reported once, in the terms that fix both.
    if sys.platform == "darwin":
        try:
            if subprocess.run(["xcode-select", "-p"],
                              capture_output=True).returncode != 0:
                out.append(
                    "Apple's Command Line Tools are not installed. Run:  "
                    "xcode-select --install")
        except Exception:                                       # noqa: BLE001
            out.append("could not ask whether the Command Line Tools are "
                       "installed. Run:  xcode-select --install")
    return out

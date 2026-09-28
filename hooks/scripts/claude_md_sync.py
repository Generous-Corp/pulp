#!/usr/bin/env python3
"""Inject origin/main's agent instructions when a checkout's copy is stale.

Claude Code and Codex load CLAUDE.md / AGENTS.md from the working tree the
session starts in. A primary checkout parked on an old branch therefore serves
old instructions (retired routing, missing gates) and agents act on them. At
SessionStart this hook compares each instruction file with the copy on the
locally fetched ``origin/main`` and, when they differ, injects a one-line note
plus the changed sections of main's version, which take precedence.

Contract:
  * Read-only git (``cat-file``, ``rev-parse``, ``rev-list``, ``log``) with
    ``GIT_OPTIONAL_LOCKS=0``; the working tree and index are never written.
  * No network: it trusts the ``origin/main`` already fetched and states that
    ref's commit date so the reader can judge how stale it is.
  * Silent when in sync, outside a Pulp checkout, without ``origin/main``, or
    when ``PULP_CLAUDE_MD_SYNC=0``. Any error exits 0 with no output.
  * Output is the SessionStart ``hookSpecificOutput.additionalContext`` JSON
    both Claude Code and Codex accept; ``--plain`` prints the text instead.
  * The script never reads ``__file__``, so ``pulp-claude-md-sync-session``
    can run origin/main's copy straight from the object store.
"""

from __future__ import annotations

import difflib
import json
import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

TRACKED_FILES = ("CLAUDE.md", "AGENTS.md")
# Claude Code stores hook context longer than 10,000 characters in a file and
# injects only a short preview, so the note must stay under that ceiling to
# arrive intact. 9,000 characters (~2.3k tokens at ~4 chars/token) leaves
# headroom for the JSON envelope.
CONTEXT_CAP_CHARS = 9000
GIT_TIMEOUT_S = 2.0
PULP_REMOTE = re.compile(r"(?:Generous-Corp|danielraffel)/pulp(?:\.git)?/?$")
HEADING = re.compile(r"^(#{1,4})\s+(.*\S)\s*$")
MAIN_REF = "refs/remotes/origin/main"


def git(root: str, *args: str) -> str | None:
    env = dict(os.environ, GIT_OPTIONAL_LOCKS="0", GIT_TERMINAL_PROMPT="0")
    try:
        result = subprocess.run(
            ("git", "-C", root, *args),
            capture_output=True,
            text=True,
            timeout=GIT_TIMEOUT_S,
            env=env,
            stdin=subprocess.DEVNULL,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    return result.stdout if result.returncode == 0 else None


def resolve_root(cwd: str) -> str | None:
    for args in (
        ("rev-parse", "--show-superproject-working-tree"),
        ("rev-parse", "--show-toplevel"),
    ):
        out = (git(cwd, *args) or "").strip()
        if out:
            return out
    return None


def is_pulp_checkout(root: str) -> bool:
    url = (git(root, "config", "--get", "remote.origin.url") or "").strip()
    return bool(PULP_REMOTE.search(url))


def split_sections(text: str) -> list[tuple[str, str]]:
    """Split markdown into (key, body) at headings outside code fences."""
    sections: list[tuple[str, str]] = []
    seen: dict[str, int] = {}
    key, body, in_fence = "(preamble)", [], False
    for line in text.splitlines(keepends=True):
        if line.lstrip().startswith(("```", "~~~")):
            in_fence = not in_fence
        match = None if in_fence else HEADING.match(line.rstrip("\r\n"))
        if match:
            sections.append((key, "".join(body)))
            title = match.group(2)
            seen[title] = seen.get(title, 0) + 1
            key = title if seen[title] == 1 else f"{title} [{seen[title]}]"
            body = [line]
        else:
            body.append(line)
    sections.append((key, "".join(body)))
    return [(k, b) for k, b in sections if b.strip()]


def normalized(body: str) -> str:
    return "\n".join(line.rstrip() for line in body.strip().splitlines())


def line_delta(old: str, new: str) -> tuple[int, int]:
    added = removed = 0
    for line in difflib.unified_diff(old.splitlines(), new.splitlines(), lineterm="", n=0):
        if line.startswith("+") and not line.startswith("+++"):
            added += 1
        elif line.startswith("-") and not line.startswith("---"):
            removed += 1
    return added, removed


def describe_drift(root: str, name: str, main_text: str) -> dict | None:
    try:
        working = (Path(root) / name).read_text(encoding="utf-8", errors="replace")
    except OSError:
        working = ""
    if normalized(working) == normalized(main_text):
        return None
    working_sections = dict(split_sections(working))
    main_sections = split_sections(main_text)
    main_keys = {key for key, _ in main_sections}
    changed = [
        (key, body)
        for key, body in main_sections
        if key not in working_sections
        or normalized(working_sections[key]) != normalized(body)
    ]
    retired = [key for key in working_sections if key not in main_keys]
    commits = (git(root, "rev-list", "--count", f"HEAD..{MAIN_REF}", "--", name) or "?").strip()
    added, removed = line_delta(working, main_text)
    return {
        "name": name,
        "main": main_text,
        "changed": changed,
        "retired": retired,
        "commits": commits,
        "added": added,
        "removed": removed,
    }


def main_ref_summary(root: str) -> str:
    out = (git(root, "log", "-1", "--format=%h%x09%ct", MAIN_REF) or "").strip()
    try:
        short, stamp = out.split("\t")
        epoch = int(stamp)
    except ValueError:
        return "origin/main"
    age_h = max(0.0, (time.time() - epoch) / 3600.0)
    age = f"{age_h:.0f}h" if age_h < 48 else f"{age_h / 24:.0f}d"
    when = time.strftime("%Y-%m-%d %H:%M UTC", time.gmtime(epoch))
    return f"origin/main {short} (committed {when}, {age} ago; no fetch was run)"


def build_context(root: str, drifts: list[dict]) -> str:
    ref = main_ref_summary(root)
    lines = [
        f"PULP INSTRUCTIONS DRIFT: {d['name']} in this checkout is {d['commits']} "
        f"commits / +{d['added']} -{d['removed']} lines behind {ref}; the current "
        f"main version follows and takes precedence over the {d['name']} loaded "
        f"from {root}. If this branch deliberately edits {d['name']}, treat those "
        f"edits as unmerged proposals."
        for d in drifts
    ]
    for d in drifts:
        if d["changed"]:
            lines.append(
                f"Sections of {d['name']} that changed on main: "
                + "; ".join(key for key, _ in d["changed"])
            )
        if d["retired"]:
            lines.append(
                f"Sections of this checkout's {d['name']} that no longer exist on "
                "main (retired; disregard them): " + "; ".join(d["retired"])
            )
    lines.append(
        "Full current files: "
        + ", ".join(f"`git show origin/main:{d['name']}`" for d in drifts)
    )
    text = "\n".join(lines) + "\n"

    for d in drifts:
        budget = CONTEXT_CAP_CHARS - len(text)
        if len(d["main"]) + 80 <= budget:
            text += f"\n===== origin/main:{d['name']} (full) =====\n{d['main']}"
            continue
        header = f"\n===== origin/main:{d['name']}, changed sections =====\n"
        if budget <= len(header) + 200:
            text += (
                f"\n[{d['name']}: {len(d['changed'])} changed sections not shown "
                f"for size; read `git show origin/main:{d['name']}`]\n"
            )
            continue
        text += header
        omitted: list[str] = []
        for key, body in d["changed"]:
            # Keep room for the omission footer before admitting a section.
            if len(text) + len(body) + 400 <= CONTEXT_CAP_CHARS:
                text += body if body.endswith("\n") else body + "\n"
            else:
                omitted.append(key)
        if omitted:
            text += (
                f"[{len(omitted)} changed sections of {d['name']} listed above are "
                f"not shown in full (context cap {CONTEXT_CAP_CHARS} chars); read "
                f"them with `git show origin/main:{d['name']}` before relying on "
                "this checkout's copy of them]\n"
            )
    return text[:CONTEXT_CAP_CHARS]


def claim_session(session_id: str) -> bool:
    """False when another copy of this hook already spoke for this session."""
    safe = re.sub(r"[^A-Za-z0-9._-]", "", session_id or "")
    if not safe:
        return True
    marker = Path(tempfile.gettempdir()) / f"pulp-claude-md-sync-{safe}"
    try:
        os.close(os.open(marker, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600))
    except FileExistsError:
        return False
    except OSError:
        return True
    return True


def read_hook_input(mode: str) -> dict:
    if mode != "--hook-json" or sys.stdin is None or sys.stdin.isatty():
        return {}
    try:
        raw = sys.stdin.read()
        value = json.loads(raw) if raw.strip() else {}
    except (OSError, ValueError):
        return {}
    return value if isinstance(value, dict) else {}


def run_hook(mode: str) -> int:
    payload = read_hook_input(mode)
    if os.environ.get("PULP_CLAUDE_MD_SYNC", "1").strip().lower() in ("0", "off", "false", "no"):
        return 0
    cwd = (
        payload.get("cwd")
        or payload.get("project_dir")
        or os.environ.get("CLAUDE_PROJECT_DIR")
        or os.getcwd()
    )
    root = resolve_root(str(cwd))
    if not root or not is_pulp_checkout(root):
        return 0
    if git(root, "rev-parse", "--verify", "--quiet", f"{MAIN_REF}^{{commit}}") is None:
        return 0
    drifts = []
    for name in TRACKED_FILES:
        main_text = git(root, "cat-file", "blob", f"{MAIN_REF}:{name}")
        if main_text is None:
            continue
        drift = describe_drift(root, name, main_text)
        if drift:
            drifts.append(drift)
    if not drifts:
        return 0
    context = build_context(root, drifts)
    if mode == "--plain":
        sys.stdout.write(context)
        return 0
    if not claim_session(str(payload.get("session_id") or "")):
        return 0
    envelope = {
        "hookSpecificOutput": {"hookEventName": "SessionStart", "additionalContext": context},
        "suppressOutput": False,
    }
    sys.stdout.write(json.dumps(envelope) + "\n")
    return 0


def main(argv: list[str]) -> int:
    mode = argv[0] if argv else "--hook-json"
    if mode not in ("--hook-json", "--plain"):
        print("claude_md_sync.py: expected --hook-json or --plain", file=sys.stderr)
        return 2
    try:
        return run_hook(mode)
    except Exception:  # noqa: BLE001 - a SessionStart hook must never fail the session
        return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

#!/usr/bin/env python3
"""Wire the user-level claude_md_sync launcher into every agent config on a host.

A checkout parked on an old branch runs its own old project hooks, and
subrouter sessions read a per-account Claude config folder
(~/.subrouter/codex/claude-proxy/<id>/) instead of ~/.claude. So the
SessionStart entry has to live in each user-level config, outside any checkout:

  * ~/.local/bin/pulp-claude-md-sync-session          the launcher itself
  * ~/.claude/settings.json                           plain Claude sessions
  * ~/.subrouter/codex/claude-proxy/*/settings.json   subrouter sessions
  * ~/.codex/hooks.json + ~/.codex/config.toml        Codex, plus its hook trust

Idempotent: an entry that already names the launcher is left alone, and a file
is only rewritten (atomically, via rename) when something is missing. A file
that does not parse is never rewritten. The launcher runs this script in
--sweep mode after each session start, so a config folder created later is
wired the first time any Pulp session runs on the host.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

LAUNCHER_NAME = "pulp-claude-md-sync-session"
MATCHER = "startup|resume"
TIMEOUT_S = 10


def atomic_write(path: Path, text: str, mode: int | None = None) -> None:
    fd, tmp = tempfile.mkstemp(dir=str(path.parent), prefix=f".{path.name}.")
    try:
        with os.fdopen(fd, "w") as fh:
            fh.write(text)
        if mode is None and path.exists():
            mode = path.stat().st_mode & 0o777
        os.chmod(tmp, mode if mode is not None else 0o644)
        os.replace(tmp, path)
    except BaseException:
        Path(tmp).unlink(missing_ok=True)
        raise


def launcher_source(repo: str | None) -> str | None:
    if repo:
        result = subprocess.run(
            ["git", "-C", repo, "cat-file", "blob",
             f"refs/remotes/origin/main:hooks/scripts/{LAUNCHER_NAME}"],
            capture_output=True, text=True,
            env={**os.environ, "GIT_OPTIONAL_LOCKS": "0"})
        return result.stdout if result.returncode == 0 and result.stdout else None
    sibling = Path(__file__).resolve().parent / LAUNCHER_NAME
    return sibling.read_text() if sibling.is_file() else None


def names_launcher(group: object) -> bool:
    return isinstance(group, dict) and any(
        isinstance(h, dict) and LAUNCHER_NAME in str(h.get("command", ""))
        for h in group.get("hooks", []) or [])


def wire_json(path: Path, command: str) -> tuple[bool, int | None]:
    """Ensure a SessionStart group runs the launcher.

    Returns (changed, index of the launcher's group), or (False, None) when
    the file cannot be safely edited.
    """
    try:
        data = json.loads(path.read_text()) if path.exists() else {}
    except (OSError, ValueError):
        return False, None
    if not isinstance(data, dict):
        return False, None
    hooks = data.setdefault("hooks", {})
    if not isinstance(hooks, dict):
        return False, None
    groups = hooks.setdefault("SessionStart", [])
    if not isinstance(groups, list):
        return False, None
    for i, group in enumerate(groups):
        if names_launcher(group):
            return False, i
    groups.append({"matcher": MATCHER,
                   "hooks": [{"type": "command", "command": command,
                              "timeout": TIMEOUT_S}]})
    atomic_write(path, json.dumps(data, indent=2) + "\n")
    return True, len(groups) - 1


def codex_trust_hash(command: str) -> str:
    # Codex trusts a hook by the hash of its normalized settings, not the
    # script contents, so later launcher updates keep the trust.
    identity = {"event_name": "session_start", "matcher": MATCHER,
                "hooks": [{"async": False, "command": command,
                           "timeout": TIMEOUT_S, "type": "command"}]}
    blob = json.dumps(identity, sort_keys=True, separators=(",", ":"))
    return "sha256:" + hashlib.sha256(blob.encode()).hexdigest()


def trust_codex(config: Path, hooks_file: Path, index: int, command: str) -> bool:
    key = f'[hooks.state."{hooks_file}:session_start:{index}:0"]'
    text = config.read_text() if config.exists() else ""
    if key in text:
        return False
    sep = "" if not text or text.endswith("\n") else "\n"
    atomic_write(config,
                 f'{text}{sep}\n{key}\ntrusted_hash = "{codex_trust_hash(command)}"\n')
    return True


def install(home: Path, repo: str | None) -> list[str]:
    changes: list[str] = []
    source = launcher_source(repo)
    bindir = home / ".local" / "bin"
    launcher = bindir / LAUNCHER_NAME
    if source and (not launcher.is_file() or launcher.read_text() != source):
        bindir.mkdir(parents=True, exist_ok=True)
        atomic_write(launcher, source, mode=0o755)
        changes.append(str(launcher))
    if not launcher.is_file():
        return changes
    command = f"{launcher} --hook-json"

    claude_dirs = [home / ".claude"]
    proxies = home / ".subrouter" / "codex" / "claude-proxy"
    if proxies.is_dir():
        claude_dirs += sorted(p for p in proxies.iterdir() if p.is_dir())
    for d in claude_dirs:
        if d.is_dir() and wire_json(d / "settings.json", command)[0]:
            changes.append(str(d / "settings.json"))

    codex = home / ".codex"
    if codex.is_dir():
        hooks_file = codex / "hooks.json"
        changed, index = wire_json(hooks_file, command)
        if changed:
            changes.append(str(hooks_file))
        if index is not None and trust_codex(codex / "config.toml", hooks_file,
                                             index, command):
            changes.append(str(codex / "config.toml"))
    return changes


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo", help="read the launcher from this checkout's origin/main")
    parser.add_argument("--sweep", action="store_true",
                        help="quiet mode used by the launcher; never fails")
    parser.add_argument("--home", default=os.path.expanduser("~"))
    args = parser.parse_args()
    try:
        changes = install(Path(args.home), args.repo)
    except Exception as exc:  # noqa: BLE001 - a sweep must never break a session
        if args.sweep:
            return 0
        print(f"install failed: {exc}", file=sys.stderr)
        return 1
    if not args.sweep:
        print("\n".join(f"wired {c}" for c in changes) or "already wired")
    return 0


if __name__ == "__main__":
    sys.exit(main())

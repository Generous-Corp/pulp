"""Git + time helpers for local CI.

Extracted from local_ci.py to give downstream modules (queue,
recovery, transport) a thin git-and-time seam without dragging in the
11k-line orchestrator. All six helpers shell out to git or return ISO
timestamps; nothing here touches local CI state files.

`ROOT` resolves to the repo root via parents[2] — that's the same
resolution local_ci.py uses, and it matches because both files sit
under tools/local-ci/.
"""

from __future__ import annotations

import subprocess
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def now_iso() -> str:
    return datetime.now(timezone.utc).isoformat()


def current_branch() -> str:
    result = subprocess.run(
        ["git", "rev-parse", "--abbrev-ref", "HEAD"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=True,
    )
    return result.stdout.strip()


def current_sha() -> str:
    result = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=True,
    )
    return result.stdout.strip()


def git_root_for(path: Path) -> Path | None:
    result = subprocess.run(
        ["git", "rev-parse", "--show-toplevel"],
        cwd=path,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        return None
    return Path(result.stdout.strip()).resolve()


def resolve_git_ref_sha(ref: str) -> str:
    result = subprocess.run(
        ["git", "rev-parse", "--verify", f"{ref}^{{commit}}"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        detail = (result.stderr or result.stdout or "").strip()
        raise ValueError(f"Could not resolve git ref '{ref}': {detail or 'unknown ref'}")
    return result.stdout.strip()


def short_sha(sha: str) -> str:
    return sha[:12] if sha else "?"

"""State directory + lock + log path helpers for local CI.

Extracted from local_ci.py to provide a stable, testable seam for code
that reasons about where local CI state lives on disk. Nothing in this
module touches the queue/results JSON contents — only paths.

The path-resolution rules are:

- PULP_LOCAL_CI_HOME overrides everything (used by tests and validation
  hosts that want a scratch state dir).
- On macOS, state lives under ~/Library/Application Support/Pulp/local-ci.
- On Linux/Windows, state follows XDG_STATE_HOME when set, otherwise
  defaults to ~/.local/state/pulp/local-ci.
- The config.json file is preferred from the shared state dir; the
  worktree-local config.json next to this script is the fallback so a
  fresh checkout can still run before the shared file is materialised.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent


def state_dir() -> Path:
    override = os.environ.get("PULP_LOCAL_CI_HOME")
    if override:
        return Path(override).expanduser()

    home = Path.home()
    if sys.platform == "darwin":
        return home / "Library" / "Application Support" / "Pulp" / "local-ci"

    xdg_state = os.environ.get("XDG_STATE_HOME")
    if xdg_state:
        return Path(xdg_state).expanduser() / "pulp" / "local-ci"
    return home / ".local" / "state" / "pulp" / "local-ci"


def config_path() -> Path:
    override = os.environ.get("PULP_LOCAL_CI_CONFIG")
    if override:
        return Path(override).expanduser()

    shared = state_dir() / "config.json"
    if shared.exists():
        return shared

    return SCRIPT_DIR / "config.json"


def worktree_config_path() -> Path:
    return SCRIPT_DIR / "config.json"


def shared_config_path() -> Path:
    return state_dir() / "config.json"


def queue_path() -> Path:
    return state_dir() / "queue.json"


def results_dir() -> Path:
    return state_dir() / "results"


def cloud_runs_dir() -> Path:
    return state_dir() / "cloud-runs"


def evidence_path() -> Path:
    return state_dir() / "evidence.json"


def logs_dir() -> Path:
    return state_dir() / "logs"


def bundles_dir() -> Path:
    return state_dir() / "bundles"


def prepared_dir() -> Path:
    return state_dir() / "prepared"


def desktop_state_dir() -> Path:
    return state_dir() / "desktop-automation"


def desktop_receipts_dir() -> Path:
    return desktop_state_dir() / "receipts"


def queue_lock_path() -> Path:
    return state_dir() / "queue.lock"


def evidence_lock_path() -> Path:
    return state_dir() / "evidence.lock"


def drain_lock_path() -> Path:
    return state_dir() / "drain.lock"


def runner_info_path() -> Path:
    return state_dir() / "runner.json"


def ensure_state_dirs() -> None:
    state_dir().mkdir(parents=True, exist_ok=True)
    results_dir().mkdir(parents=True, exist_ok=True)
    cloud_runs_dir().mkdir(parents=True, exist_ok=True)
    logs_dir().mkdir(parents=True, exist_ok=True)
    bundles_dir().mkdir(parents=True, exist_ok=True)
    desktop_state_dir().mkdir(parents=True, exist_ok=True)
    desktop_receipts_dir().mkdir(parents=True, exist_ok=True)


def job_logs_dir(job_id: str) -> Path:
    return logs_dir() / job_id


def target_log_path(job_id: str, target_name: str) -> Path:
    return job_logs_dir(job_id) / f"{target_name}.log"


def prepare_target_log(job_id: str, target_name: str) -> Path:
    path = target_log_path(job_id, target_name)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("")
    return path

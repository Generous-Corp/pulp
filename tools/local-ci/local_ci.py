#!/usr/bin/env python3
"""Local CI runner for Pulp — validates queued jobs on Mac, Ubuntu, and Windows.

Usage:
    pulp ci-local run [branch]                # Queue and wait for completion
    pulp ci-local run [branch] --smoke        # Fast install/export preflight, no tests
    pulp ci-local ship [branch]               # PR -> queued CI -> merge on green
    pulp ci-local check <PR#|latest>          # Validate an existing PR
    pulp ci-local check <PR#|latest> --smoke  # Fast PR smoke preflight
    pulp ci-local cloud workflows             # List supported GitHub workflows/providers
    pulp ci-local cloud defaults              # Show effective cloud defaults
    pulp ci-local cloud run [workflow]        # Dispatch a GitHub workflow
    pulp ci-local cloud status [id|latest]    # Show tracked GitHub workflow state
    pulp ci-local cloud history               # Show recent tracked cloud run history
    pulp ci-local cloud compare [workflow]    # Compare observed cloud providers
    pulp ci-local cloud recommend [workflow]  # Suggest a cloud provider from recorded history
    pulp ci-local cloud namespace doctor      # Check Namespace CLI/login/workspace state
    pulp ci-local cloud namespace setup       # Thin Namespace setup wrapper (`nsc login`)
    pulp ci-local list                        # Show open PRs
    pulp ci-local status                      # Show queue, runner, and VM status
    pulp ci-local enqueue [branch]            # Queue for later drain
    pulp ci-local drain                       # Drain pending jobs if no runner is active
    pulp ci-local bump <job> <priority>       # Reprioritize a pending job

Queueing model:
    - CI state is machine-global, not per worktree.
    - Only one drain owner runs jobs at a time.
    - Jobs are ordered by priority, then FIFO within each priority.
    - Each job validates an exact git SHA.
    - SSH targets receive the queued SHA via a git bundle before validation.
"""

from __future__ import annotations

import argparse
from collections import deque
from collections import defaultdict
import fcntl
import hashlib
import json
import os
import queue as queue_module
import shlex
import shutil
import statistics
import subprocess
import sys
import threading
import time
import uuid
from concurrent.futures import ThreadPoolExecutor, as_completed
from contextlib import contextmanager
from datetime import date, datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT_DIR = Path(__file__).resolve().parent
PRIORITY_VALUES = {"low": 10, "normal": 50, "high": 100}
WAIT_POLL_SECS = 3
KEEP_COMPLETED_JOBS = 25
HEARTBEAT_INTERVAL_SECS = 15.0
STUCK_IDLE_SECS = 90.0
_BUNDLE_BUILD_LOCK = threading.Lock()
GITHUB_ACTIONS_DEFAULTS = {
    "repository": "",
    "workflow": "build",
    "provider": "github-hosted",
    "wait_poll_secs": 10,
    "match_timeout_secs": 60,
}
BUILTIN_GITHUB_WORKFLOWS = {
    "build": {
        "file": "build.yml",
        "display_name": "Build and Test",
        "providers": ["github-hosted", "namespace"],
        "provider_input": "runner_provider",
        "dispatch_fields": [
            "linux_runner_selector_json",
            "windows_runner_selector_json",
            "macos_runner_selector_json",
        ],
    },
    "validate": {
        "file": "validate.yml",
        "display_name": "Plugin Validation",
        "providers": ["github-hosted"],
    },
    "sanitizers": {
        "file": "sanitizers.yml",
        "display_name": "Sanitizer Tests",
        "providers": ["github-hosted"],
    },
    "docs-check": {
        "file": "docs-check.yml",
        "display_name": "Docs Consistency",
        "providers": ["github-hosted", "namespace"],
        "provider_input": "runner_provider",
        "selector_input": "runner_selector_json",
    },
}
REPO_VARIABLE_FALLBACKS = {
    ("build", "namespace", "linux_runner_selector_json"): "PULP_NAMESPACE_BUILD_LINUX_RUNS_ON_JSON",
    ("build", "namespace", "windows_runner_selector_json"): "PULP_NAMESPACE_BUILD_WINDOWS_RUNS_ON_JSON",
    ("build", "namespace", "macos_runner_selector_json"): "PULP_NAMESPACE_BUILD_MACOS_RUNS_ON_JSON",
    ("docs-check", "namespace", "runner_selector_json"): "PULP_NAMESPACE_DOCS_CHECK_RUNS_ON_JSON",
}


class LockBusyError(RuntimeError):
    """Raised when a non-blocking lock cannot be acquired."""


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


def job_logs_dir(job_id: str) -> Path:
    return logs_dir() / job_id


def target_log_path(job_id: str, target_name: str) -> Path:
    return job_logs_dir(job_id) / f"{target_name}.log"


def prepare_target_log(job_id: str, target_name: str) -> Path:
    path = target_log_path(job_id, target_name)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("")
    return path


def format_size_bytes(value: int | float | None) -> str:
    if value in (None, ""):
        return ""
    amount = float(value)
    units = ["B", "KB", "MB", "GB", "TB"]
    for unit in units:
        if amount < 1024.0 or unit == units[-1]:
            if unit == "B":
                return f"{int(amount)} {unit}"
            return f"{amount:.1f} {unit}"
        amount /= 1024.0
    return f"{amount:.1f} TB"


def path_size_bytes(path: Path) -> int:
    try:
        if not path.exists():
            return 0
        if path.is_file():
            return int(path.stat().st_size)
    except OSError:
        return 0

    total = 0
    for root, _dirs, files in os.walk(path):
        for filename in files:
            try:
                total += int((Path(root) / filename).stat().st_size)
            except OSError:
                continue
    return total


def local_ci_state_footprint() -> dict:
    entries = {}
    total = 0
    for label, path in (
        ("bundles", bundles_dir()),
        ("prepared", prepared_dir()),
        ("logs", logs_dir()),
        ("results", results_dir()),
        ("cloud-runs", cloud_runs_dir()),
    ):
        size_bytes = path_size_bytes(path)
        entries[label] = {
            "path": path,
            "size_bytes": size_bytes,
        }
        total += size_bytes
    return {
        "entries": entries,
        "total_bytes": total,
    }


def describe_path_for_cleanup(path: Path) -> str:
    try:
        return str(path.relative_to(state_dir()))
    except ValueError:
        return str(path)


def bundle_ref_name(job_id: str) -> str:
    return f"refs/pulp-ci-bundles/{job_id}"


def remote_bundle_name(job_id: str) -> str:
    return f"pulp-ci-{job_id}.bundle"


def create_job_bundle(job: dict) -> Path:
    ensure_state_dirs()
    bundle_path = bundles_dir() / f"{job['id']}.bundle"
    bundle_lock_path = Path(f"{bundle_path}.lock")

    with _BUNDLE_BUILD_LOCK:
        if bundle_path.exists() and bundle_path.stat().st_size > 0:
            return bundle_path

        bundle_lock_path.unlink(missing_ok=True)
        bundle_path.unlink(missing_ok=True)

        temp_ref = bundle_ref_name(job["id"])
        subprocess.run(["git", "update-ref", temp_ref, job["sha"]], cwd=ROOT, check=True)
        try:
            subprocess.run(["git", "bundle", "create", str(bundle_path), temp_ref], cwd=ROOT, check=True)
        finally:
            subprocess.run(["git", "update-ref", "-d", temp_ref], cwd=ROOT, check=True)
    return bundle_path


def sync_job_bundle_to_ssh_host(host: str, job: dict, report_progress=None) -> tuple[str, str]:
    bundle_path = create_job_bundle(job)
    remote_name = remote_bundle_name(job["id"])
    try:
        if report_progress:
            report_progress(
                phase="bundle-upload",
                host=host,
                bundle=remote_name,
                last_output_at=now_iso(),
                transport_mode="bundle",
            )
        subprocess.run(
            ["scp", str(bundle_path), f"{host}:{remote_name}"],
            capture_output=True,
            text=True,
            timeout=300,
            check=True,
        )
    except subprocess.SubprocessError as exc:
        detail = ""
        if isinstance(exc, subprocess.CalledProcessError):
            detail = (exc.stderr or exc.stdout or "").strip()
        raise RuntimeError(
            f"failed to upload git bundle to {host}: {detail or exc}"
        ) from exc
    return remote_name, bundle_ref_name(job["id"])


def tail_lines(path: Path, limit: int = 80) -> list[str]:
    if not path.exists():
        return []
    with path.open("r", errors="replace") as handle:
        return list(deque(handle, maxlen=limit))


def trim_line(value: str, max_len: int = 160) -> str:
    value = value.strip()
    if len(value) <= max_len:
        return value
    return "…" + value[-(max_len - 1):]


def atomic_write_text(path: Path, text: str) -> None:
    ensure_state_dirs()
    tmp = path.with_name(f".{path.name}.{os.getpid()}.{uuid.uuid4().hex}.tmp")
    try:
        tmp.write_text(text)
        tmp.replace(path)
    finally:
        tmp.unlink(missing_ok=True)


@contextmanager
def file_lock(path: Path, *, blocking: bool):
    ensure_state_dirs()
    handle = path.open("a+")
    mode = fcntl.LOCK_EX
    if not blocking:
        mode |= fcntl.LOCK_NB

    try:
        fcntl.flock(handle.fileno(), mode)
    except BlockingIOError as exc:
        handle.close()
        raise LockBusyError(str(path)) from exc

    try:
        yield handle
    finally:
        fcntl.flock(handle.fileno(), fcntl.LOCK_UN)
        handle.close()


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


def normalize_priority(priority: str | None) -> str:
    value = (priority or "normal").strip().lower()
    if value not in PRIORITY_VALUES:
        raise ValueError(f"Invalid priority '{priority}'. Use one of: low, normal, high.")
    return value


def priority_value(priority: str | None) -> int:
    return PRIORITY_VALUES[normalize_priority(priority)]


def normalize_validation_mode(mode: str | None) -> str:
    value = (mode or "full").strip().lower()
    if value not in {"full", "smoke"}:
        raise ValueError(f"Invalid validation mode '{mode}'. Use one of: full, smoke.")
    return value


def load_config() -> dict:
    path = config_path()
    if not path.exists():
        raise FileNotFoundError(
            f"Local CI config not found at {path}. Copy tools/local-ci/config.example.json first."
        )
    return json.loads(path.read_text())


def load_optional_config() -> dict | None:
    path = config_path()
    if not path.exists():
        return None
    return json.loads(path.read_text())


def github_actions_settings_for_display(config: dict | None) -> dict:
    settings = dict(GITHUB_ACTIONS_DEFAULTS)
    github_actions = (config or {}).get("github_actions", {})
    defaults = github_actions.get("defaults", {})

    repository = github_actions.get("repository")
    if isinstance(repository, str) and repository.strip():
        settings["repository"] = repository.strip()

    workflow = defaults.get("workflow")
    if isinstance(workflow, str) and workflow.strip():
        settings["workflow"] = workflow.strip()

    provider = defaults.get("provider")
    if isinstance(provider, str) and provider.strip():
        settings["provider"] = provider.strip()

    return settings


def resolve_github_actions_settings(config: dict | None) -> dict:
    settings = github_actions_settings_for_display(config)
    defaults = ((config or {}).get("github_actions") or {}).get("defaults", {})

    for key in ("wait_poll_secs", "match_timeout_secs"):
        value = defaults.get(key)
        if value in (None, ""):
            continue
        try:
            parsed = int(value)
        except (TypeError, ValueError) as exc:
            raise ValueError(f"github_actions.defaults.{key} must be an integer.") from exc
        if parsed <= 0:
            raise ValueError(f"github_actions.defaults.{key} must be positive.")
        settings[key] = parsed

    return settings


def normalize_runs_on_json(raw: str, *, setting_name: str) -> str:
    value = (raw or "").strip()
    if not value:
        return ""
    try:
        decoded = json.loads(value)
    except json.JSONDecodeError as exc:
        raise ValueError(f"{setting_name} must be valid JSON.") from exc
    if not isinstance(decoded, (str, list)):
        raise ValueError(f"{setting_name} must decode to a string or array accepted by runs-on.")
    return json.dumps(decoded)


def resolve_workflow_runner_selector_json(
    config: dict | None, workflow_key: str, provider: str
) -> str:
    github_actions = (config or {}).get("github_actions", {})
    workflows = github_actions.get("workflows", {})
    if not isinstance(workflows, dict):
        return ""
    workflow = workflows.get(workflow_key, {})
    if not isinstance(workflow, dict):
        return ""
    providers = workflow.get("providers", {})
    if not isinstance(providers, dict):
        return ""
    provider_info = providers.get(provider, {})
    if not isinstance(provider_info, dict):
        return ""
    selector = provider_info.get("runner_selector_json")
    if not isinstance(selector, str) or not selector.strip():
        return ""
    return normalize_runs_on_json(
        selector,
        setting_name=f"github_actions.workflows.{workflow_key}.providers.{provider}.runner_selector_json",
    )


def resolve_workflow_dispatch_field_values(
    config: dict | None,
    workflow_key: str,
    provider: str,
    field_names: list[str] | tuple[str, ...] | None,
) -> dict[str, str]:
    if not field_names:
        return {}

    github_actions = (config or {}).get("github_actions", {})
    workflows = github_actions.get("workflows", {})
    if not isinstance(workflows, dict):
        return {}
    workflow = workflows.get(workflow_key, {})
    if not isinstance(workflow, dict):
        return {}
    providers = workflow.get("providers", {})
    if not isinstance(providers, dict):
        return {}
    provider_info = providers.get(provider, {})
    if not isinstance(provider_info, dict):
        return {}

    resolved: dict[str, str] = {}
    for field_name in field_names:
        value = provider_info.get(field_name)
        if not isinstance(value, str) or not value.strip():
            continue
        resolved[field_name] = normalize_runs_on_json(
            value,
            setting_name=(
                f"github_actions.workflows.{workflow_key}.providers.{provider}.{field_name}"
            ),
        )
    return resolved


def repo_variable_name_for_workflow_field(
    workflow_key: str, provider: str, field_name: str
) -> str:
    return REPO_VARIABLE_FALLBACKS.get((workflow_key, provider, field_name), "")


def resolve_default_provider_for_workflow(
    settings: dict,
    workflow_key: str,
    *,
    explicit_provider: str | None = None,
) -> tuple[str, str]:
    workflow = BUILTIN_GITHUB_WORKFLOWS.get(workflow_key)
    if workflow is None:
        raise ValueError(f"Unknown workflow '{workflow_key}'.")

    supported = workflow.get("providers", ["github-hosted"])
    if explicit_provider:
        provider = explicit_provider.strip()
        if provider not in supported:
            raise ValueError(
                f"workflow '{workflow_key}' does not support provider '{provider}'. "
                f"Supported: {', '.join(supported)}"
            )
        return provider, "cli"

    preferred = (settings.get("provider") or "github-hosted").strip() or "github-hosted"
    if preferred in supported:
        source = "github_actions.defaults.provider" if settings.get("provider") else "builtin default"
        return preferred, source

    return "github-hosted", f"workflow fallback (default provider '{preferred}' unsupported)"


def resolve_workflow_field_value_and_source(
    config: dict | None,
    repository_variables: dict[str, str],
    workflow_key: str,
    provider: str,
    field_name: str,
) -> tuple[str, str]:
    config_values = resolve_workflow_dispatch_field_values(config, workflow_key, provider, [field_name])
    value = config_values.get(field_name, "")
    if value:
        return (
            value,
            f"config github_actions.workflows.{workflow_key}.providers.{provider}.{field_name}",
        )

    variable_name = repo_variable_name_for_workflow_field(workflow_key, provider, field_name)
    if variable_name:
        variable_value = repository_variables.get(variable_name, "")
        if variable_value:
            return (
                normalize_runs_on_json(variable_value, setting_name=variable_name),
                f"repo variable {variable_name}",
            )

    return "", ""


def resolve_workflow_dispatch_defaults(
    config: dict | None,
    repository_variables: dict[str, str],
    workflow_key: str,
    provider: str,
    field_names: list[str] | tuple[str, ...] | None,
) -> tuple[dict[str, str], dict[str, str]]:
    resolved: dict[str, str] = {}
    sources: dict[str, str] = {}
    for field_name in field_names or []:
        value, source = resolve_workflow_field_value_and_source(
            config,
            repository_variables,
            workflow_key,
            provider,
            field_name,
        )
        if not value:
            continue
        resolved[field_name] = value
        if source:
            sources[field_name] = source
    return resolved, sources


def summarize_workflow_provider_defaults(
    config: dict | None,
    repository_variables: dict[str, str],
    settings: dict,
    workflow_key: str,
) -> dict:
    workflow = BUILTIN_GITHUB_WORKFLOWS[workflow_key]
    provider, provider_source = resolve_default_provider_for_workflow(settings, workflow_key)
    dispatch_fields, dispatch_sources = resolve_workflow_dispatch_defaults(
        config,
        repository_variables,
        workflow_key,
        provider,
        workflow.get("dispatch_fields"),
    )
    selector_value = ""
    selector_source = ""
    selector_input = workflow.get("selector_input")
    if selector_input:
        selector_value, selector_source = resolve_workflow_field_value_and_source(
            config,
            repository_variables,
            workflow_key,
            provider,
            selector_input,
        )
    return {
        "provider": provider,
        "provider_source": provider_source,
        "selector_input": selector_input or "",
        "selector_value": selector_value,
        "selector_source": selector_source,
        "dispatch_fields": dispatch_fields,
        "dispatch_sources": dispatch_sources,
    }


def resolve_cli_dispatch_field_values(
    args: argparse.Namespace,
    field_names: list[str] | tuple[str, ...] | None,
) -> dict[str, str]:
    supported = set(field_names or [])
    override_names = (
        "linux_runner_selector_json",
        "windows_runner_selector_json",
        "macos_runner_selector_json",
    )
    resolved: dict[str, str] = {}
    for field_name in override_names:
        value = getattr(args, field_name, None)
        if not value:
            continue
        if field_name not in supported:
            raise ValueError(
                f"--{field_name.replace('_', '-')} is not supported for this workflow."
            )
        resolved[field_name] = normalize_runs_on_json(
            value,
            setting_name=f"--{field_name.replace('_', '-')}",
        )
    return resolved


def normalize_provenance(provenance: dict | None = None) -> dict:
    normalized = dict(provenance or {})
    normalized.setdefault("execution_kind", "direct")
    normalized.setdefault("control_plane", "pulp-ci-local")
    normalized.setdefault("direct_backend", "local-ci")
    normalized.setdefault("hosted_orchestrator", "")
    normalized.setdefault("runner_provider", "")
    normalized.setdefault("runner_selector", "")
    normalized.setdefault("run_id", "")
    normalized.setdefault("run_url", "")
    return normalized


def provenance_summary(provenance: dict | None) -> str:
    info = normalize_provenance(provenance)
    execution_kind = info.get("execution_kind", "direct")
    selector = info.get("runner_selector", "")
    run_id = info.get("run_id", "")

    if execution_kind == "hosted":
        orchestrator = info.get("hosted_orchestrator", "") or "unknown-orchestrator"
        provider = info.get("runner_provider", "")
        summary = f"hosted via {orchestrator}"
        if provider:
            summary += f"/{provider}"
    else:
        summary = f"direct via {info.get('direct_backend', 'local-ci') or 'local-ci'}"

    if selector:
        summary += f" selector={selector}"
    if run_id:
        summary += f" run={run_id}"
    return summary


def normalize_result(result: dict) -> dict:
    normalized = dict(result)
    submission = normalized.get("submission") or {}
    normalized["provenance"] = normalize_provenance(
        normalized.get("provenance") or submission.get("provenance")
    )
    return normalized


def normalize_job(job: dict) -> dict:
    normalized = dict(job)
    if "id" not in normalized:
        legacy_raw = "|".join(
            [normalized.get("branch", ""), normalized.get("sha", ""), normalized.get("queued_at", "")]
        )
        normalized["id"] = hashlib.sha1(legacy_raw.encode("utf-8")).hexdigest()[:12]
    normalized["priority"] = normalize_priority(normalized.get("priority", "normal"))
    normalized["targets"] = sorted(dict.fromkeys(normalized.get("targets") or []))
    normalized["status"] = normalized.get("status", "pending")
    normalized["validation"] = normalize_validation_mode(normalized.get("validation", "full"))
    submission = dict(normalized.get("submission") or {})
    submission["provenance"] = normalize_provenance(submission.get("provenance"))
    normalized["submission"] = submission
    normalized["provenance"] = normalize_provenance(
        normalized.get("provenance") or submission.get("provenance")
    )
    return normalized


def load_queue_unlocked() -> list[dict]:
    path = queue_path()
    if not path.exists():
        return []

    raw = json.loads(path.read_text())
    jobs = raw.get("jobs", raw) if isinstance(raw, dict) else raw
    return [normalize_job(job) for job in jobs]


def save_queue_unlocked(queue: list[dict]) -> None:
    atomic_write_text(queue_path(), json.dumps(queue, indent=2) + "\n")


def load_queue() -> list[dict]:
    with file_lock(queue_lock_path(), blocking=True):
        queue = load_queue_unlocked()
        queue, changed = reconcile_running_jobs_unlocked(queue)
        if changed:
            save_queue_unlocked(queue)
        return queue


def enabled_targets(config: dict) -> list[str]:
    return [
        name
        for name, target_cfg in config.get("targets", {}).items()
        if target_cfg.get("enabled", True)
    ]


def parse_targets_arg(value: str | None) -> list[str] | None:
    if value is None or value.strip() == "":
        return None
    parts = [part.strip() for part in value.split(",") if part.strip()]
    return sorted(dict.fromkeys(parts))


def resolve_targets(config: dict, requested: list[str] | None) -> list[str]:
    if requested is None:
        configured = config.get("defaults", {}).get("targets")
        if configured is not None:
            if isinstance(configured, str):
                requested = parse_targets_arg(configured)
            else:
                requested = sorted(dict.fromkeys(configured))
        else:
            requested = enabled_targets(config)

    if not requested:
        return []

    valid = set(config.get("targets", {}).keys())
    unknown = [target for target in requested if target not in valid]
    if unknown:
        raise ValueError(f"Unknown target(s): {', '.join(unknown)}")

    disabled = [
        target
        for target in requested
        if not config["targets"].get(target, {}).get("enabled", True)
    ]
    if disabled:
        raise ValueError(
            f"Requested target(s) disabled in config: {', '.join(disabled)}"
        )

    return sorted(dict.fromkeys(requested))


def default_priority_for(command: str, config: dict) -> str:
    defaults = config.get("defaults", {})
    if command in {"ship", "check"}:
        return normalize_priority(defaults.get(f"{command}_priority", "high"))
    return normalize_priority(defaults.get("priority", "normal"))


def make_fingerprint(branch: str, sha: str, targets: list[str], validation: str) -> str:
    raw = json.dumps(
        {"branch": branch, "sha": sha, "targets": sorted(targets), "validation": validation},
        sort_keys=True,
    )
    return hashlib.sha256(raw.encode("utf-8")).hexdigest()


def make_job(
    branch: str,
    sha: str,
    priority: str,
    targets: list[str],
    mode: str,
    validation: str,
    submission: dict | None = None,
) -> dict:
    normalized_validation = normalize_validation_mode(validation)
    job = {
        "id": uuid.uuid4().hex[:12],
        "branch": branch,
        "sha": sha,
        "priority": normalize_priority(priority),
        "targets": sorted(targets),
        "queued_at": now_iso(),
        "status": "pending",
        "fingerprint": make_fingerprint(branch, sha, targets, normalized_validation),
        "mode": mode,
        "validation": normalized_validation,
        "submitted_root": str(ROOT),
    }
    if submission:
        job["submission"] = submission
        if submission.get("submitted_root"):
            job["submitted_root"] = submission["submitted_root"]
        if submission.get("provenance"):
            job["provenance"] = normalize_provenance(submission.get("provenance"))
    if "provenance" not in job:
        job["provenance"] = normalize_provenance()
    return job


def supersedence_key(job: dict) -> tuple[str, tuple[str, ...], str]:
    return (
        job.get("branch", ""),
        tuple(sorted(job.get("targets") or [])),
        normalize_validation_mode(job.get("validation", "full")),
    )


def supersedence_identity_key(job: dict) -> tuple[str, str, str]:
    return (
        job.get("branch", ""),
        job.get("sha", ""),
        normalize_validation_mode(job.get("validation", "full")),
    )


def jobs_share_supersedence_scope(newer_job: dict, older_job: dict) -> bool:
    return (
        newer_job.get("id") != older_job.get("id")
        and newer_job.get("fingerprint") != older_job.get("fingerprint")
        and supersedence_key(newer_job) == supersedence_key(older_job)
    )


def job_has_narrower_same_identity_scope(newer_job: dict, older_job: dict) -> bool:
    if (
        newer_job.get("id") == older_job.get("id")
        or newer_job.get("fingerprint") == older_job.get("fingerprint")
        or supersedence_identity_key(newer_job) != supersedence_identity_key(older_job)
    ):
        return False

    newer_targets = set(newer_job.get("targets") or [])
    older_targets = set(older_job.get("targets") or [])
    return bool(newer_targets) and newer_targets < older_targets


def supersedence_reason(newer_job: dict, older_job: dict) -> str | None:
    if jobs_share_supersedence_scope(newer_job, older_job):
        return "newer_sha_queued"
    if job_has_narrower_same_identity_scope(newer_job, older_job):
        return "narrower_scope_queued"
    return None


def supersedence_result(job: dict, superseded_by: str, reason: str) -> dict:
    return {
        "job_id": job["id"],
        "branch": job["branch"],
        "sha": job["sha"],
        "priority": job["priority"],
        "validation": job.get("validation", "full"),
        "targets": job.get("targets", []),
        "queued_at": job.get("queued_at", ""),
        "completed_at": now_iso(),
        "provenance": normalize_provenance(job.get("provenance")),
        "results": [],
        "overall": "superseded",
        "superseded_by": superseded_by,
        "superseded_reason": reason,
    }


def supersede_job_unlocked(job: dict, superseded_by: str, reason: str) -> None:
    result = supersedence_result(job, superseded_by, reason)
    result_path = save_result(result)
    job["status"] = "completed"
    job["completed_at"] = result["completed_at"]
    job["result_file"] = str(result_path)
    job["overall"] = "superseded"
    job["superseded_by"] = superseded_by
    job["superseded_reason"] = reason
    job.pop("runner", None)
    job.pop("active_targets", None)
    job.pop("last_progress_at", None)


def cancellation_result(job: dict, reason: str) -> dict:
    return {
        "job_id": job["id"],
        "branch": job["branch"],
        "sha": job["sha"],
        "priority": job["priority"],
        "validation": job.get("validation", "full"),
        "targets": job.get("targets", []),
        "queued_at": job.get("queued_at", ""),
        "completed_at": now_iso(),
        "provenance": normalize_provenance(job.get("provenance")),
        "results": [],
        "overall": "canceled",
        "canceled_reason": reason,
    }


def cancel_job_unlocked(job: dict, reason: str = "operator_canceled") -> None:
    result = cancellation_result(job, reason)
    result_path = save_result(result)
    job["status"] = "completed"
    job["completed_at"] = result["completed_at"]
    job["result_file"] = str(result_path)
    job["overall"] = "canceled"
    job["canceled_reason"] = reason
    job.pop("runner", None)
    job.pop("active_targets", None)
    job.pop("last_progress_at", None)


def summarize_job(job: dict) -> str:
    targets = ",".join(job.get("targets") or []) or "none"
    validation = job.get("validation", "full")
    validation_suffix = f" validation={validation}" if validation != "full" else ""
    return (
        f"[{job['id']}] {job['branch']} @ {short_sha(job.get('sha', ''))} "
        f"priority={job.get('priority', 'normal')} targets={targets}{validation_suffix}"
    )


def summarize_active_targets(active_targets: dict | None, preferred_order: list[str] | None = None) -> str:
    if not active_targets:
        return ""

    parts: list[str] = []
    seen: set[str] = set()
    for name in preferred_order or []:
        state = active_targets.get(name)
        if not state:
            continue
        parts.append(f"{name}={state.get('status', '?')}")
        seen.add(name)

    for name in sorted(active_targets):
        if name in seen:
            continue
        state = active_targets.get(name) or {}
        parts.append(f"{name}={state.get('status', '?')}")

    return ", ".join(parts)


def upsert_job_active_targets_unlocked(queue: list[dict], job_id: str, active_targets: dict | None) -> bool:
    for job in queue:
        if job["id"] != job_id:
            continue
        if active_targets:
            job["active_targets"] = active_targets
            job["last_progress_at"] = now_iso()
        else:
            job.pop("active_targets", None)
            job.pop("last_progress_at", None)
        return True
    return False


def update_job_active_targets(job_id: str, active_targets: dict | None) -> None:
    with file_lock(queue_lock_path(), blocking=True):
        queue = load_queue_unlocked()
        if upsert_job_active_targets_unlocked(queue, job_id, active_targets):
            save_queue_unlocked(queue)


def enqueue_job(
    branch: str,
    sha: str,
    priority: str,
    targets: list[str],
    mode: str,
    validation: str,
    submission: dict | None = None,
) -> tuple[dict, bool]:
    requested_priority = normalize_priority(priority)
    normalized_validation = normalize_validation_mode(validation)

    with file_lock(queue_lock_path(), blocking=True):
        queue = load_queue_unlocked()
        queue, changed = reconcile_running_jobs_unlocked(queue)
        if changed:
            save_queue_unlocked(queue)
        fingerprint = make_fingerprint(branch, sha, targets, normalized_validation)

        for job in queue:
            if job.get("fingerprint") != fingerprint or job.get("status") not in {"pending", "running"}:
                continue

            changed = False
            if (
                job["status"] == "pending"
                and priority_value(requested_priority) > priority_value(job.get("priority", "normal"))
            ):
                job["priority"] = requested_priority
                job["bumped_at"] = now_iso()
                changed = True

            if changed:
                save_queue_unlocked(queue)
            return normalize_job(job), False

        job = make_job(branch, sha, requested_priority, targets, mode, normalized_validation, submission=submission)
        queue.append(job)
        for existing in queue:
            if existing.get("status") != "pending":
                continue
            reason = supersedence_reason(job, existing)
            if reason:
                supersede_job_unlocked(existing, job["id"], reason)
        save_queue_unlocked(trim_completed_jobs(queue))
        return job, True


def trim_completed_jobs_with_removed_ids(queue: list[dict]) -> tuple[list[dict], set[str]]:
    completed = [job for job in queue if job.get("status") == "completed"]
    if len(completed) <= KEEP_COMPLETED_JOBS:
        return queue, set()

    completed_by_time = sorted(completed, key=lambda job: job.get("completed_at", job.get("queued_at", "")))
    remove_ids = {job["id"] for job in completed_by_time[:-KEEP_COMPLETED_JOBS]}
    return [job for job in queue if job["id"] not in remove_ids], remove_ids


def trim_completed_jobs(queue: list[dict]) -> list[dict]:
    trimmed, _removed_ids = trim_completed_jobs_with_removed_ids(queue)
    return trimmed


def result_file_job_id(path: Path) -> str | None:
    if path.suffix != ".json":
        return None
    stem = path.stem
    parts = stem.split("-", 3)
    if len(parts) < 3:
        return None
    return parts[2]


def artifact_entry_sort_key(entry: dict) -> tuple[float, str]:
    return (float(entry.get("mtime", 0.0)), str(entry.get("path", "")))


def collect_local_ci_cleanup_plan(
    queue: list[dict],
    *,
    keep_results: int = KEEP_COMPLETED_JOBS,
    keep_logs: int = KEEP_COMPLETED_JOBS,
    keep_bundles: int = 0,
    include_prepared: bool = False,
) -> dict:
    keep_results = max(0, int(keep_results))
    keep_logs = max(0, int(keep_logs))
    keep_bundles = max(0, int(keep_bundles))
    retained_job_ids = {job["id"] for job in queue}
    live_job_ids = {job["id"] for job in queue if job.get("status") in {"pending", "running"}}
    categories: dict[str, list[dict]] = {
        "bundles": [],
        "logs": [],
        "results": [],
        "prepared": [],
    }

    def add_file_entry(category: str, path: Path, job_id: str | None) -> None:
        try:
            stat = path.stat()
        except OSError:
            return
        categories[category].append(
            {
                "path": path,
                "job_id": job_id,
                "size_bytes": int(stat.st_size),
                "mtime": float(stat.st_mtime),
            }
        )

    def add_dir_entry(category: str, path: Path, job_id: str | None) -> None:
        if not path.exists() or not path.is_dir():
            return
        try:
            stat = path.stat()
        except OSError:
            return
        categories[category].append(
            {
                "path": path,
                "job_id": job_id,
                "size_bytes": path_size_bytes(path),
                "mtime": float(stat.st_mtime),
            }
        )

    for path in bundles_dir().glob("*.bundle"):
        add_file_entry("bundles", path, path.stem)
    log_root = logs_dir()
    for path in (log_root.iterdir() if log_root.exists() else []):
        if path.is_dir():
            add_dir_entry("logs", path, path.name)
    for path in results_dir().glob("*.json"):
        add_file_entry("results", path, result_file_job_id(path))
    if include_prepared and prepared_dir().exists():
        for target_dir in prepared_dir().iterdir():
            if not target_dir.is_dir():
                continue
            for mode_dir in target_dir.iterdir():
                if mode_dir.is_dir():
                    add_dir_entry("prepared", mode_dir, None)

    plan_categories: dict[str, list[dict]] = {
        "bundles": [],
        "logs": [],
        "results": [],
        "prepared": [],
    }

    bundle_candidates = [
        entry for entry in sorted(categories["bundles"], key=artifact_entry_sort_key, reverse=True)
        if entry.get("job_id") not in live_job_ids
    ]
    plan_categories["bundles"] = bundle_candidates[keep_bundles:]

    def select_queue_orphans(entries: list[dict], keep_count: int) -> list[dict]:
        always_keep = [entry for entry in entries if entry.get("job_id") in retained_job_ids]
        orphaned = [entry for entry in entries if entry.get("job_id") not in retained_job_ids]
        orphaned.sort(key=artifact_entry_sort_key, reverse=True)
        del always_keep  # clarity: retained-job artifacts are never candidates
        return orphaned[keep_count:]

    plan_categories["logs"] = select_queue_orphans(categories["logs"], keep_logs)
    plan_categories["results"] = select_queue_orphans(categories["results"], keep_results)
    plan_categories["prepared"] = sorted(
        categories["prepared"],
        key=artifact_entry_sort_key,
        reverse=True,
    )

    total_bytes = sum(
        int(entry.get("size_bytes", 0))
        for entries in plan_categories.values()
        for entry in entries
    )
    total_paths = sum(len(entries) for entries in plan_categories.values())
    return {
        "categories": plan_categories,
        "total_bytes": total_bytes,
        "total_paths": total_paths,
        "keep_results": keep_results,
        "keep_logs": keep_logs,
        "keep_bundles": keep_bundles,
        "include_prepared": include_prepared,
    }


def apply_local_ci_cleanup_plan(plan: dict) -> dict:
    removed: list[dict] = []
    failed: list[dict] = []
    for category, entries in (plan.get("categories") or {}).items():
        for entry in entries:
            path = Path(entry["path"])
            try:
                if path.is_dir():
                    shutil.rmtree(path)
                else:
                    path.unlink(missing_ok=True)
                removed.append(
                    {
                        "category": category,
                        "path": path,
                        "size_bytes": int(entry.get("size_bytes", 0)),
                    }
                )
            except OSError as exc:
                failed.append(
                    {
                        "category": category,
                        "path": path,
                        "error": str(exc),
                    }
                )
    return {
        "removed": removed,
        "failed": failed,
        "removed_bytes": sum(item["size_bytes"] for item in removed),
    }


def job_sort_key(job: dict) -> tuple[int, str, str]:
    return (-priority_value(job.get("priority", "normal")), job.get("queued_at", ""), job["id"])


def reconcile_running_jobs_unlocked(queue: list[dict]) -> tuple[list[dict], bool]:
    changed = False
    for job in stale_running_jobs_unlocked(queue):
        replacement = None
        for candidate in queue:
            if candidate.get("status") not in {"pending", "running"}:
                continue
            reason = supersedence_reason(candidate, job)
            if not reason:
                continue
            if replacement is None or candidate.get("queued_at", "") > replacement.get("queued_at", ""):
                replacement = candidate

        if replacement is not None:
            supersede_job_unlocked(job, replacement["id"], supersedence_reason(replacement, job) or "newer_sha_queued")
            changed = True
            continue

        job["status"] = "pending"
        job["requeued_at"] = now_iso()
        job.pop("started_at", None)
        job.pop("runner", None)
        changed = True

    return queue, changed


def read_runner_info() -> dict | None:
    path = runner_info_path()
    if not path.exists():
        return None
    return json.loads(path.read_text())


def pid_alive(pid: int | None) -> bool:
    if not pid or pid <= 0:
        return False
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    return True


def current_runner_info() -> dict | None:
    info = read_runner_info()
    if not info:
        return None

    if pid_alive(info.get("pid")):
        return info

    try:
        with file_lock(drain_lock_path(), blocking=False):
            runner_info_path().unlink(missing_ok=True)
            return None
    except LockBusyError:
        return info


def stale_running_jobs_unlocked(queue: list[dict]) -> list[dict]:
    runner = read_runner_info()
    runner_pid = runner.get("pid") if runner else None
    runner_alive = pid_alive(runner_pid)

    if runner and not runner_alive:
        runner_info_path().unlink(missing_ok=True)
        runner = None
        runner_pid = None

    stale: list[dict] = []
    for job in queue:
        if job.get("status") != "running":
            continue
        job_runner = job.get("runner") or {}
        if runner and runner_pid and job_runner.get("pid") == runner_pid:
            continue
        stale.append(job)
    return stale


def update_job_target_state(job_id: str, target_name: str, **fields) -> None:
    with file_lock(queue_lock_path(), blocking=True):
        queue = load_queue_unlocked()
        job = find_job_unlocked(queue, job_id)
        if job is None:
            return

        active_targets = dict(job.get("active_targets") or {})
        state = dict(active_targets.get(target_name) or {})
        for key, value in fields.items():
            if value is None:
                state.pop(key, None)
            else:
                state[key] = value

        if state:
            active_targets[target_name] = state
        else:
            active_targets.pop(target_name, None)

        if active_targets:
            job["active_targets"] = active_targets
            job["last_progress_at"] = now_iso()
        else:
            job.pop("active_targets", None)
            job.pop("last_progress_at", None)

        save_queue_unlocked(queue)


def collect_stale_windows_cleanup_candidates_unlocked(queue: list[dict]) -> list[dict]:
    candidates: list[dict] = []
    for job in stale_running_jobs_unlocked(queue):
        active_targets = job.get("active_targets") or {}
        state = dict(active_targets.get("windows") or {})
        host = state.get("host")
        validator_pid = state.get("validator_pid")
        validator_started_at = state.get("validator_started_at")
        if not host or validator_pid is None or not validator_started_at:
            continue
        if state.get("cleanup_requested_at"):
            continue

        state["cleanup_requested_at"] = now_iso()
        state["cleanup_status"] = "requested"
        state["cleanup_reason"] = "stale_runner_recovery"
        active_targets["windows"] = state
        job["active_targets"] = active_targets
        job["last_progress_at"] = now_iso()
        candidates.append(
            {
                "job_id": job["id"],
                "target": "windows",
                "host": host,
                "validator_pid": int(validator_pid),
                "validator_started_at": validator_started_at,
            }
        )
    return candidates


def cleanup_stale_windows_validator(host: str, pid: int, started_at: str) -> dict:
    ps_script = f"""
$PidToKill = {pid}
$ExpectedStart = '{ps_literal(started_at)}'

function Get-DescendantProcessIds {{
    param([int]$RootPid)
    $result = New-Object System.Collections.Generic.List[int]
    $queue = New-Object System.Collections.Generic.Queue[int]
    $queue.Enqueue($RootPid)
    while ($queue.Count -gt 0) {{
        $current = $queue.Dequeue()
        $children = @(Get-CimInstance Win32_Process -Filter "ParentProcessId = $current" -ErrorAction SilentlyContinue)
        foreach ($child in $children) {{
            $childPid = [int]$child.ProcessId
            $result.Add($childPid)
            $queue.Enqueue($childPid)
        }}
    }}
    return $result
}}

$result = [ordered]@{{
    found = $false
    matched = $false
    killed = $false
    pid = $PidToKill
}}

try {{
    $proc = Get-Process -Id $PidToKill -ErrorAction SilentlyContinue
    if ($null -ne $proc) {{
        $result.found = $true
        $start = $proc.StartTime.ToUniversalTime().ToString('o')
        $result.start = $start
        if ($ExpectedStart -and $start -ne $ExpectedStart) {{
            $result.matched = $false
        }} else {{
            $result.matched = $true
            $children = @(Get-DescendantProcessIds -RootPid $PidToKill | Sort-Object -Descending -Unique)
            foreach ($childPid in $children) {{
                try {{
                    Stop-Process -Id $childPid -Force -ErrorAction Stop
                }} catch {{
                }}
            }}
            Stop-Process -Id $PidToKill -Force -ErrorAction Stop
            $result.killed = $true
            $result.children = @($children)
        }}
    }}
}} catch {{
    $result.error = $_.Exception.Message
}}

$result | ConvertTo-Json -Compress
""".strip()
    run = run_logged_command(
        windows_ssh_powershell_command(host),
        input_text=ps_script,
        timeout=120,
    )
    lines = [line.strip() for line in run["output"].splitlines() if line.strip()]
    payload = {}
    if lines:
        try:
            payload = json.loads(lines[-1])
        except json.JSONDecodeError:
            payload = {"error": trim_line(lines[-1])}
    if run["returncode"] != 0:
        payload.setdefault("error", f"cleanup command exited {run['returncode']}")
    return payload


def reclaim_stale_remote_validators(_config: dict) -> int:
    with file_lock(queue_lock_path(), blocking=True):
        queue = load_queue_unlocked()
        candidates = collect_stale_windows_cleanup_candidates_unlocked(queue)
        if candidates:
            save_queue_unlocked(queue)

    for candidate in candidates:
        result = cleanup_stale_windows_validator(
            candidate["host"],
            candidate["validator_pid"],
            candidate["validator_started_at"],
        )
        update_job_target_state(
            candidate["job_id"],
            candidate["target"],
            cleanup_completed_at=now_iso(),
            cleanup_status=(
                "killed"
                if result.get("killed")
                else "not-found"
                if not result.get("found", True)
                else "mismatch"
                if result.get("found") and not result.get("matched", True)
                else "error"
                if result.get("error")
                else "checked"
            ),
            cleanup_result=trim_line(json.dumps(result, sort_keys=True)),
            validator_pid=None if result.get("killed") or not result.get("found", True) else candidate["validator_pid"],
            validator_started_at=None
            if result.get("killed") or not result.get("found", True)
            else candidate["validator_started_at"],
        )
    return len(candidates)


def write_runner_info(info: dict) -> None:
    atomic_write_text(runner_info_path(), json.dumps(info, indent=2) + "\n")


def update_runner_active_targets(job_id: str, active_targets: dict | None) -> None:
    info = current_runner_info()
    if not info or info.get("active_job_id") != job_id:
        return

    if active_targets:
        info["active_targets"] = active_targets
    else:
        info.pop("active_targets", None)
    info["updated_at"] = now_iso()
    write_runner_info(info)


def clear_runner_info() -> None:
    runner_info_path().unlink(missing_ok=True)


def find_job_unlocked(queue: list[dict], job_ref: str, statuses: set[str] | None = None) -> dict | None:
    candidates = queue
    if statuses is not None:
        candidates = [job for job in candidates if job.get("status") in statuses]

    for job in candidates:
        if job["id"] == job_ref:
            return job

    id_prefix = [job for job in candidates if job["id"].startswith(job_ref)]
    if len(id_prefix) == 1:
        return id_prefix[0]
    if len(id_prefix) > 1:
        raise ValueError(f"Job reference '{job_ref}' is ambiguous.")

    branch_matches = [job for job in candidates if job.get("branch") == job_ref]
    if len(branch_matches) == 1:
        return branch_matches[0]
    if len(branch_matches) > 1:
        raise ValueError(
            f"Multiple jobs match branch '{job_ref}'. Use a job id or unique prefix."
        )

    return None


def load_job(job_id: str) -> dict | None:
    with file_lock(queue_lock_path(), blocking=True):
        queue = load_queue_unlocked()
        queue, changed = reconcile_running_jobs_unlocked(queue)
        if changed:
            save_queue_unlocked(queue)
        job = find_job_unlocked(queue, job_id)
        return normalize_job(job) if job else None


def claim_next_job() -> dict | None:
    with file_lock(queue_lock_path(), blocking=True):
        queue = load_queue_unlocked()
        queue, changed = reconcile_running_jobs_unlocked(queue)
        if changed:
            save_queue_unlocked(queue)
        pending = sorted(
            [job for job in queue if job.get("status") == "pending"],
            key=job_sort_key,
        )
        if not pending:
            return None

        selected_id = pending[0]["id"]
        claimed = None
        for job in queue:
            if job["id"] != selected_id:
                continue
            job["status"] = "running"
            job["started_at"] = now_iso()
            job["runner"] = {"pid": os.getpid(), "root": str(ROOT)}
            job.pop("active_targets", None)
            job.pop("last_progress_at", None)
            claimed = normalize_job(job)
            break

        save_queue_unlocked(queue)
        return claimed


def finalize_job(job_id: str, result: dict, result_path: Path) -> None:
    retained_queue: list[dict] | None = None
    with file_lock(queue_lock_path(), blocking=True):
        queue = load_queue_unlocked()
        for job in queue:
            if job["id"] != job_id:
                continue
            job["status"] = "completed"
            job["completed_at"] = now_iso()
            job["result_file"] = str(result_path)
            job["overall"] = result.get("overall")
            job.pop("runner", None)
            job.pop("active_targets", None)
            job.pop("last_progress_at", None)
            break

        retained_queue, _removed_ids = trim_completed_jobs_with_removed_ids(queue)
        save_queue_unlocked(retained_queue)

    if retained_queue is not None:
        apply_local_ci_cleanup_plan(
            collect_local_ci_cleanup_plan(
                retained_queue,
                keep_results=KEEP_COMPLETED_JOBS,
                keep_logs=KEEP_COMPLETED_JOBS,
                keep_bundles=0,
                include_prepared=False,
            )
        )


def parse_iso_datetime(value: str | None) -> datetime | None:
    if not value:
        return None
    normalized = value.replace("Z", "+00:00")
    try:
        return datetime.fromisoformat(normalized)
    except ValueError:
        return None


def load_result(path: Path) -> dict:
    return normalize_result(json.loads(path.read_text()))


def normalize_cloud_record(record: dict | None) -> dict:
    normalized = dict(record or {})
    normalized.setdefault("kind", "github-actions-run")
    normalized.setdefault("dispatch_id", "")
    normalized.setdefault("run_id", None)
    normalized.setdefault("repository", "")
    normalized.setdefault("workflow_key", "")
    normalized.setdefault("workflow_file", "")
    normalized.setdefault("workflow_name", "")
    normalized.setdefault("requested_ref", "")
    normalized.setdefault("head_branch", "")
    normalized.setdefault("head_sha", "")
    normalized.setdefault("requested_by", "")
    normalized.setdefault("orchestrator", "github-actions")
    normalized.setdefault("provider_requested", "github-hosted")
    normalized.setdefault("provider_resolved", "")
    normalized.setdefault("runner_selector_json", "")
    normalized.setdefault("dispatch_fields", {})
    normalized.setdefault("status", "unresolved")
    normalized.setdefault("conclusion", "")
    normalized.setdefault("url", "")
    normalized.setdefault("dispatched_at", "")
    normalized.setdefault("matched_at", "")
    normalized.setdefault("started_at", "")
    normalized.setdefault("updated_at", "")
    normalized.setdefault("completed_at", "")
    normalized.setdefault("queue_delay_secs", None)
    normalized.setdefault("duration_secs", None)
    normalized.setdefault("match_strategy", "")
    normalized.setdefault("match_ambiguous", False)
    normalized.setdefault("jobs", [])
    normalized.setdefault("provider_metadata", {})
    normalized.setdefault("usage_summary", {})
    normalized.setdefault("cost_summary", {})
    if not isinstance(normalized.get("dispatch_fields"), dict):
        normalized["dispatch_fields"] = {}
    if not isinstance(normalized.get("jobs"), list):
        normalized["jobs"] = []
    for field_name in ("provider_metadata", "usage_summary", "cost_summary"):
        if not isinstance(normalized.get(field_name), dict):
            normalized[field_name] = {}
    return normalized


def cloud_run_path(dispatch_id: str) -> Path:
    return cloud_runs_dir() / f"{dispatch_id}.json"


def save_cloud_record(record: dict) -> Path:
    ensure_state_dirs()
    normalized = normalize_cloud_record(record)
    path = cloud_run_path(normalized["dispatch_id"])
    atomic_write_text(path, json.dumps(normalized, indent=2) + "\n")
    return path


def load_cloud_record(path: Path) -> dict:
    return normalize_cloud_record(json.loads(path.read_text()))


def cloud_record_sort_key(record: dict) -> tuple[str, str]:
    timestamp = (
        record.get("completed_at")
        or record.get("updated_at")
        or record.get("matched_at")
        or record.get("dispatched_at")
        or ""
    )
    return (timestamp, record.get("dispatch_id", ""))


def list_cloud_records(limit: int | None = None) -> list[dict]:
    ensure_state_dirs()
    records: list[dict] = []
    for path in cloud_runs_dir().glob("*.json"):
        try:
            records.append(load_cloud_record(path))
        except (OSError, json.JSONDecodeError):
            continue
    records.sort(key=cloud_record_sort_key, reverse=True)
    if limit is not None:
        return records[:limit]
    return records


def find_cloud_record(records: list[dict], identifier: str | None) -> dict | None:
    if not records:
        return None
    if not identifier or identifier == "latest":
        return records[0]

    exact_dispatch = [record for record in records if record.get("dispatch_id") == identifier]
    if len(exact_dispatch) == 1:
        return exact_dispatch[0]

    prefix_dispatch = [record for record in records if record.get("dispatch_id", "").startswith(identifier)]
    if len(prefix_dispatch) == 1:
        return prefix_dispatch[0]
    if len(prefix_dispatch) > 1:
        raise ValueError(f"Cloud run reference '{identifier}' is ambiguous.")

    run_matches = [record for record in records if str(record.get("run_id") or "") == identifier]
    if len(run_matches) == 1:
        return run_matches[0]
    if len(run_matches) > 1:
        raise ValueError(f"Cloud run id '{identifier}' matched multiple records.")
    return None


def cloud_record_summary(record: dict, config: dict | None = None) -> str:
    record = normalize_cloud_record(record)
    status = record.get("status", "unknown").upper()
    conclusion = (record.get("conclusion") or "").upper()
    state = status if not conclusion else f"{status}/{conclusion}"
    provider = record.get("provider_resolved") or record.get("provider_requested") or "github-hosted"
    ref = record.get("head_branch") or record.get("requested_ref") or "?"
    summary = (
        f"[{record.get('dispatch_id', '?')}] {record.get('workflow_key', '?')} "
        f"ref={ref} provider={provider} {state}"
    )
    selector = summarize_runner_selector(record.get("runner_selector_json", ""))
    if selector:
        summary += f" selector={selector}"
    if record.get("run_id"):
        summary += f" gha#{record['run_id']}"
    duration = format_duration_secs(record.get("duration_secs"))
    if duration:
        summary += f" duration={duration}"
    provider_runtime = format_duration_secs((record.get("usage_summary") or {}).get("provider_runtime_secs"))
    if provider_runtime:
        summary += f" provider_time={provider_runtime}"
    if config is not None:
        cost = estimate_cloud_record_cost(record, config)
        if cost.get("status") == "estimated":
            amount = format_currency_amount(cost.get("estimated_total"), cost.get("currency", "USD"))
            if amount:
                summary += f" cost=est {amount}"
    return summary


def summarize_runner_selector(selector_json: str) -> str:
    raw = (selector_json or "").strip()
    if not raw:
        return ""
    try:
        decoded = json.loads(raw)
    except json.JSONDecodeError:
        return raw
    if isinstance(decoded, str):
        return decoded
    if isinstance(decoded, list):
        return ",".join(str(item) for item in decoded)
    return raw


def normalize_github_timestamp(value: str | None) -> str:
    raw = (value or "").strip()
    if not raw or raw.startswith("0001-01-01T00:00:00"):
        return ""
    return raw


def duration_between(started_at: str | None, completed_at: str | None) -> float | None:
    start_dt = parse_iso_datetime(normalize_github_timestamp(started_at))
    end_dt = parse_iso_datetime(normalize_github_timestamp(completed_at))
    if not start_dt or not end_dt:
        return None
    return round(max(0.0, (end_dt - start_dt).total_seconds()), 1)


def format_duration_secs(value: float | int | str | None) -> str:
    if value in (None, ""):
        return ""
    try:
        total = float(value)
    except (TypeError, ValueError):
        return ""
    if total < 0:
        return ""
    rounded = int(round(total))
    hours, remainder = divmod(rounded, 3600)
    minutes, seconds = divmod(remainder, 60)
    if hours:
        return f"{hours}h{minutes:02d}m{seconds:02d}s"
    if minutes:
        return f"{minutes}m{seconds:02d}s"
    if abs(total - rounded) >= 0.05:
        return f"{total:.1f}s"
    return f"{rounded}s"


def format_memory_megabytes(value: int | float | str | None) -> str:
    if value in (None, ""):
        return ""
    try:
        megabytes = float(value)
    except (TypeError, ValueError):
        return ""
    if megabytes <= 0:
        return ""
    gigabytes = megabytes / 1024.0
    return f"{gigabytes:g} GB"


def render_selector_value(value: str) -> str:
    return summarize_runner_selector(value) if value else ""


def parse_rate_value(value) -> float | None:
    if value in (None, ""):
        return None
    try:
        parsed = float(value)
    except (TypeError, ValueError):
        return None
    if parsed < 0:
        return None
    return parsed


def parse_optional_bool(value, setting_name: str) -> bool | None:
    if value in (None, ""):
        return None
    if isinstance(value, bool):
        return value
    raise ValueError(f"{setting_name} must be true or false.")


def resolve_billing_settings(config: dict | None) -> dict:
    billing = (((config or {}).get("telemetry") or {}).get("billing") or {})
    settings = {
        "currency": "USD",
        "billing_period_start_day": 1,
        "enable_provider_reported_totals": False,
        "github_hosted_job_os_rates_per_minute": {},
        "namespace_profile_tag_rates_per_hour": {},
        "namespace_machine_shape_rates_per_hour": [],
    }
    if not isinstance(billing, dict):
        return settings

    currency = billing.get("currency")
    if isinstance(currency, str) and currency.strip():
        settings["currency"] = currency.strip().upper()

    start_day = billing.get("billing_period_start_day")
    if start_day not in (None, ""):
        try:
            parsed_start_day = int(start_day)
        except (TypeError, ValueError) as exc:
            raise ValueError("telemetry.billing.billing_period_start_day must be an integer.") from exc
        if parsed_start_day < 1 or parsed_start_day > 28:
            raise ValueError("telemetry.billing.billing_period_start_day must be between 1 and 28.")
        settings["billing_period_start_day"] = parsed_start_day

    provider_reported_totals = parse_optional_bool(
        billing.get("enable_provider_reported_totals"),
        "telemetry.billing.enable_provider_reported_totals",
    )
    if provider_reported_totals is not None:
        settings["enable_provider_reported_totals"] = provider_reported_totals

    github_rates = billing.get("github_hosted_job_os_rates_per_minute")
    if isinstance(github_rates, dict):
        for os_name, value in github_rates.items():
            if not isinstance(os_name, str) or not os_name.strip():
                continue
            parsed = parse_rate_value(value)
            if parsed is not None:
                settings["github_hosted_job_os_rates_per_minute"][os_name.strip().lower()] = parsed

    namespace_profile_rates = billing.get("namespace_profile_tag_rates_per_hour")
    if isinstance(namespace_profile_rates, dict):
        for tag, value in namespace_profile_rates.items():
            if not isinstance(tag, str) or not tag.strip():
                continue
            parsed = parse_rate_value(value)
            if parsed is not None:
                settings["namespace_profile_tag_rates_per_hour"][tag.strip()] = parsed

    shape_rates = billing.get("namespace_machine_shape_rates_per_hour")
    if isinstance(shape_rates, list):
        normalized_shape_rates = []
        for raw in shape_rates:
            if not isinstance(raw, dict):
                continue
            parsed_rate = parse_rate_value(raw.get("rate"))
            if parsed_rate is None:
                continue
            normalized_shape_rates.append(
                {
                    "os": str(raw.get("os", "")).strip().lower(),
                    "arch": str(raw.get("arch", "")).strip().lower(),
                    "virtual_cpu": int(raw.get("virtual_cpu") or 0),
                    "memory_megabytes": int(raw.get("memory_megabytes") or 0),
                    "rate": parsed_rate,
                }
            )
        settings["namespace_machine_shape_rates_per_hour"] = normalized_shape_rates

    return settings


def format_currency_amount(amount: float | int | None, currency: str = "USD") -> str:
    if amount is None:
        return ""
    try:
        value = float(amount)
    except (TypeError, ValueError):
        return ""
    if currency.upper() == "USD":
        return f"${value:.2f}"
    return f"{currency.upper()} {value:.2f}"


def billing_note_text() -> str:
    return "estimated; verify provider pricing"


def provider_billing_note_text() -> str:
    return "actual when available"


def billing_period_window(
    start_day: int,
    *,
    now_dt: datetime | None = None,
) -> tuple[datetime, datetime]:
    current = now_dt or datetime.now(timezone.utc)
    year = current.year
    month = current.month
    if current.day < start_day:
        month -= 1
        if month == 0:
            month = 12
            year -= 1
    period_start = datetime(year, month, start_day, tzinfo=timezone.utc)
    next_year = year
    next_month = month + 1
    if next_month == 13:
        next_month = 1
        next_year += 1
    period_end = datetime(next_year, next_month, start_day, tzinfo=timezone.utc)
    return period_start, period_end


def iter_year_months(start_dt: datetime, end_dt: datetime) -> list[tuple[int, int]]:
    current_year = start_dt.year
    current_month = start_dt.month
    months: list[tuple[int, int]] = []
    while True:
        months.append((current_year, current_month))
        if current_year == end_dt.year and current_month == end_dt.month:
            break
        current_month += 1
        if current_month == 13:
            current_month = 1
            current_year += 1
    return months


def parse_iso_date(value: str | None) -> date | None:
    raw = (value or "").strip()
    if not raw:
        return None
    try:
        return date.fromisoformat(raw)
    except ValueError:
        return None


def infer_job_os(workflow_key: str, job_name: str) -> str:
    name = (job_name or "").strip().lower()
    if "windows" in name:
        return "windows"
    if "macos" in name or "mac " in name or "mac (" in name:
        return "macos"
    if "linux" in name or "ubuntu" in name:
        return "linux"
    if workflow_key in {"docs-check", "sanitizers"}:
        return "linux"
    return ""


def match_namespace_shape_rate(shape: dict, billing: dict) -> float | None:
    profile_tag = (shape.get("profile_tag") or "").strip()
    if profile_tag:
        tagged_rate = (billing.get("namespace_profile_tag_rates_per_hour") or {}).get(profile_tag)
        if tagged_rate is not None:
            return float(tagged_rate)

    for candidate in billing.get("namespace_machine_shape_rates_per_hour") or []:
        if candidate.get("os") and candidate["os"] != str(shape.get("os", "")).strip().lower():
            continue
        if candidate.get("arch") and candidate["arch"] != str(shape.get("arch", "")).strip().lower():
            continue
        if candidate.get("virtual_cpu") and candidate["virtual_cpu"] != int(shape.get("virtual_cpu") or 0):
            continue
        if candidate.get("memory_megabytes") and candidate["memory_megabytes"] != int(shape.get("memory_megabytes") or 0):
            continue
        return float(candidate["rate"])
    return None


def estimate_namespace_cost(record: dict, billing: dict) -> dict:
    metadata = (record.get("provider_metadata") or {}).get("namespace_instances") or []
    shapes = (record.get("usage_summary") or {}).get("machine_shapes") or []
    currency = billing.get("currency", "USD")
    total = 0.0
    estimated_items = 0

    if metadata:
        for instance in metadata:
            rate = match_namespace_shape_rate(instance, billing)
            if rate is None:
                continue
            duration_secs = float(instance.get("duration_secs") or 0)
            total += (duration_secs / 3600.0) * rate
            estimated_items += 1
    elif shapes:
        for shape in shapes:
            rate = match_namespace_shape_rate(shape, billing)
            if rate is None:
                continue
            duration_secs = float(shape.get("duration_secs") or 0)
            total += (duration_secs / 3600.0) * rate
            estimated_items += 1

    if estimated_items:
        return {
            "status": "estimated",
            "currency": currency,
            "estimated_total": round(total, 4),
            "reason": billing_note_text(),
        }

    return {
        "status": "unavailable",
        "reason": "configure telemetry.billing Namespace rates",
    }


def estimate_github_hosted_cost(record: dict, billing: dict) -> dict:
    currency = billing.get("currency", "USD")
    rates = billing.get("github_hosted_job_os_rates_per_minute") or {}
    total = 0.0
    estimated_jobs = 0

    for job in record.get("jobs") or []:
        job_name = str(job.get("name", ""))
        if job_name == "resolve-provider":
            continue
        os_name = infer_job_os(record.get("workflow_key", ""), job_name)
        if not os_name:
            continue
        rate = rates.get(os_name)
        if rate is None:
            continue
        duration_secs = duration_between(job.get("started_at"), job.get("completed_at"))
        if duration_secs is None:
            continue
        total += (duration_secs / 60.0) * float(rate)
        estimated_jobs += 1

    if estimated_jobs:
        return {
            "status": "estimated",
            "currency": currency,
            "estimated_total": round(total, 4),
            "reason": billing_note_text(),
        }

    return {
        "status": "unavailable",
        "reason": "configure telemetry.billing GitHub-hosted rates",
    }


def estimate_cloud_record_cost(record: dict, config: dict | None) -> dict:
    record = normalize_cloud_record(record)
    provider = record.get("provider_resolved") or record.get("provider_requested") or "github-hosted"
    billing = resolve_billing_settings(config)
    if provider == "namespace":
        return estimate_namespace_cost(record, billing)
    if provider == "github-hosted":
        return estimate_github_hosted_cost(record, billing)
    return {"status": "unavailable", "reason": f"no estimator for provider '{provider}'"}


def estimate_billing_period_totals(
    records: list[dict],
    config: dict | None,
    *,
    provider: str | None = None,
) -> dict:
    billing = resolve_billing_settings(config)
    period_start, period_end = billing_period_window(billing["billing_period_start_day"])
    matched_runs = 0
    estimated_runs = 0
    estimated_total = 0.0

    for raw_record in records:
        record = normalize_cloud_record(raw_record)
        completed = parse_iso_datetime(record.get("completed_at") or record.get("updated_at"))
        if not completed:
            continue
        if provider and (record.get("provider_resolved") or record.get("provider_requested")) != provider:
            continue
        if completed < period_start or completed >= period_end:
            continue
        matched_runs += 1
        summary = estimate_cloud_record_cost(record, config)
        if summary.get("status") == "estimated":
            estimated_runs += 1
            estimated_total += float(summary.get("estimated_total") or 0.0)

    return {
        "currency": billing.get("currency", "USD"),
        "period_start": period_start.isoformat(),
        "period_end": period_end.isoformat(),
        "matched_runs": matched_runs,
        "estimated_runs": estimated_runs,
        "estimated_total": round(estimated_total, 4),
        "status": "estimated" if estimated_runs else "unavailable",
        "reason": billing_note_text() if estimated_runs else "configure telemetry.billing rates",
    }


def fetch_github_repo_actions_billing_summary(repository: str, config: dict | None) -> dict:
    billing = resolve_billing_settings(config)
    if not billing.get("enable_provider_reported_totals"):
        return {"status": "disabled", "reason": "disabled (opt-in)"}
    if not gh_available():
        return {"status": "unavailable", "reason": "gh CLI unavailable"}

    repo_payload, repo_error = gh_api_json(f"/repos/{repository}")
    if not isinstance(repo_payload, dict):
        return {
            "status": "unavailable",
            "reason": f"repo lookup failed ({repo_error or 'gh api failed'})",
        }

    owner = ((repo_payload.get("owner") or {}).get("login") or "").strip()
    owner_type = ((repo_payload.get("owner") or {}).get("type") or "").strip().lower()
    if not owner:
        return {"status": "unavailable", "reason": "repo owner unknown"}

    if owner_type == "organization":
        endpoint = f"/organizations/{owner}/settings/billing/usage"
    elif owner_type == "user":
        endpoint = f"/users/{owner}/settings/billing/usage"
    else:
        return {"status": "unavailable", "reason": f"unsupported owner type '{owner_type or 'unknown'}'"}

    period_start, period_end = billing_period_window(billing["billing_period_start_day"])
    month_pairs = iter_year_months(period_start, period_end)
    matched_items: list[dict] = []

    for year, month in month_pairs:
        payload, error = gh_api_json(endpoint, fields={"year": year, "month": month})
        if not isinstance(payload, dict):
            reason = "GitHub billing API unavailable; check auth/platform"
            if owner_type == "user" and "user" not in gh_token_scopes():
                reason = "GitHub billing API unavailable; check auth/platform"
            return {
                "status": "unavailable",
                "reason": reason,
                "detail": error,
            }
        for item in payload.get("usageItems") or []:
            if str(item.get("product", "")).strip().lower() != "actions":
                continue
            if str(item.get("repositoryName", "")).strip() != repository:
                continue
            item_date = parse_iso_date(item.get("date"))
            if not item_date:
                continue
            item_dt = datetime(item_date.year, item_date.month, item_date.day, tzinfo=timezone.utc)
            if item_dt < period_start or item_dt >= period_end:
                continue
            matched_items.append(item)

    total = 0.0
    for item in matched_items:
        amount = item.get("netAmount")
        if amount in (None, ""):
            amount = item.get("grossAmount")
        try:
            total += float(amount or 0.0)
        except (TypeError, ValueError):
            continue

    return {
        "status": "actual",
        "provider": "github-hosted",
        "scope": "repo current period",
        "currency": "USD",
        "period_start": period_start.isoformat(),
        "period_end": period_end.isoformat(),
        "matched_items": len(matched_items),
        "actual_total": round(total, 4),
        "reason": provider_billing_note_text(),
    }


def print_github_repo_billing_summary(summary: dict, *, indent: str = "  ") -> None:
    status = (summary.get("status") or "").strip()
    if status == "disabled":
        return
    if status == "actual":
        amount = format_currency_amount(summary.get("actual_total"), summary.get("currency", "USD"))
        if amount:
            print(
                f"{indent}github repo billing: actual {amount} current period (repo-wide)"
            )
        return
    reason = (summary.get("reason") or "").strip()
    if reason:
        print(f"{indent}github repo billing: unavailable ({reason})")


def print_cloud_field_detail(
    name: str,
    value: str,
    source: str = "",
    *,
    indent: str = "    ",
    unset_note: str = "",
) -> None:
    rendered = render_selector_value(value) if name.endswith("_selector_json") else str(value)
    if rendered:
        suffix = f" ({source})" if source else ""
        print(f"{indent}{name}: {rendered}{suffix}")
        return
    if unset_note:
        print(f"{indent}{name}: unset ({unset_note})")
    else:
        print(f"{indent}{name}: unset")


def print_namespace_usage_summary(record: dict) -> None:
    usage = (record.get("usage_summary") or {})
    if not usage:
        return

    instances_count = usage.get("instances_count")
    provider_runtime = format_duration_secs(usage.get("provider_runtime_secs"))
    if instances_count:
        runtime_suffix = f" runtime={provider_runtime}" if provider_runtime else ""
        print(f"  provider usage: {instances_count} Namespace instance(s){runtime_suffix}")
    for shape in usage.get("machine_shapes") or []:
        os_arch = "/".join(part for part in [shape.get("os", ""), shape.get("arch", "")] if part) or "unknown"
        resources = []
        if shape.get("virtual_cpu"):
            resources.append(f"{shape['virtual_cpu']} vCPU")
        memory = format_memory_megabytes(shape.get("memory_megabytes"))
        if memory:
            resources.append(memory)
        resources_text = f" {' '.join(resources)}" if resources else ""
        count = int(shape.get("count") or 0)
        runtime = format_duration_secs(shape.get("duration_secs"))
        runtime_text = f" runtime={runtime}" if runtime else ""
        profile_tag = shape.get("profile_tag") or "unlabeled"
        print(
            f"    {profile_tag}: {os_arch}{resources_text} x{count}{runtime_text}"
        )

    cost = record.get("cost_summary") or {}
    reason = (cost.get("reason") or "").strip()
    status = (cost.get("status") or "").strip()
    if status == "estimated":
        amount = format_currency_amount(cost.get("estimated_total"), cost.get("currency", "USD"))
        if amount:
            print(f"  cost: est {amount}; {reason or billing_note_text()}")
    elif status == "unavailable" and reason:
        print(f"  cost: unavailable ({reason})")


def print_billing_period_summary(summary: dict, *, indent: str = "  ") -> None:
    status = (summary.get("status") or "").strip()
    if status != "estimated":
        reason = (summary.get("reason") or "").strip()
        if reason:
            print(f"{indent}period cost: unavailable ({reason})")
        return
    amount = format_currency_amount(summary.get("estimated_total"), summary.get("currency", "USD"))
    if not amount:
        return
    runs_text = f"{int(summary.get('estimated_runs') or 0)} run(s)"
    print(f"{indent}period cost: est {amount} over {runs_text}; {summary.get('reason') or billing_note_text()}")


def summarize_cloud_timing(snapshot: dict) -> dict[str, str | float | None]:
    created_at = normalize_github_timestamp(snapshot.get("createdAt"))
    updated_at = normalize_github_timestamp(snapshot.get("updatedAt"))
    observed_updates = [updated_at] if updated_at else []
    job_starts = [
        normalize_github_timestamp(job.get("startedAt"))
        for job in snapshot.get("jobs", []) or []
        if normalize_github_timestamp(job.get("startedAt"))
    ]
    job_completions = [
        normalize_github_timestamp(job.get("completedAt"))
        for job in snapshot.get("jobs", []) or []
        if normalize_github_timestamp(job.get("completedAt"))
    ]
    for job in snapshot.get("jobs", []) or []:
        for step in job.get("steps", []) or []:
            step_started = normalize_github_timestamp(step.get("startedAt"))
            if step_started:
                observed_updates.append(step_started)
            step_completed = normalize_github_timestamp(step.get("completedAt"))
            if step_completed:
                observed_updates.append(step_completed)

    started_at = min(job_starts) if job_starts else ""
    status = snapshot.get("status", "")
    if status == "completed":
        if job_completions:
            completed_at = max(job_completions)
        else:
            completed_at = updated_at
    else:
        completed_at = ""

    duration_anchor = completed_at or (max(observed_updates) if observed_updates else "")
    return {
        "started_at": started_at,
        "completed_at": completed_at,
        "queue_delay_secs": duration_between(created_at, started_at),
        "duration_secs": duration_between(started_at, duration_anchor),
    }


def namespace_instance_duration_secs(instance: dict) -> float | None:
    created_at = instance.get("created")
    completed_at = instance.get("destroyed_at") or now_iso()
    return duration_between(created_at, completed_at)


def normalize_namespace_instance(instance: dict) -> dict:
    shape = instance.get("shape") or {}
    user_label = instance.get("user_label") or {}
    github_workflow = instance.get("github_workflow") or {}
    duration_secs = namespace_instance_duration_secs(instance)
    return {
        "cluster_id": instance.get("cluster_id", ""),
        "created_at": normalize_github_timestamp(instance.get("created", "")),
        "destroyed_at": normalize_github_timestamp(instance.get("destroyed_at", "")),
        "os": shape.get("os", ""),
        "arch": shape.get("machine_arch", ""),
        "virtual_cpu": shape.get("virtual_cpu", 0),
        "memory_megabytes": shape.get("memory_megabytes", 0),
        "profile_tag": user_label.get("nsc.runner-profile-tag", ""),
        "profile_id": user_label.get("nsc.runner-profile-id", ""),
        "repository": github_workflow.get("repository", ""),
        "run_id": github_workflow.get("run_id", ""),
        "workflow": github_workflow.get("workflow", ""),
        "duration_secs": duration_secs,
    }


def summarize_namespace_usage(instances: list[dict]) -> dict:
    machine_shapes: dict[tuple[str, str, int, int, str], dict] = {}
    total_runtime = 0.0
    for instance in instances:
        duration_secs = float(instance.get("duration_secs") or 0)
        total_runtime += duration_secs
        key = (
            instance.get("os", ""),
            instance.get("arch", ""),
            int(instance.get("virtual_cpu") or 0),
            int(instance.get("memory_megabytes") or 0),
            instance.get("profile_tag", ""),
        )
        shape = machine_shapes.setdefault(
            key,
            {
                "os": instance.get("os", ""),
                "arch": instance.get("arch", ""),
                "virtual_cpu": int(instance.get("virtual_cpu") or 0),
                "memory_megabytes": int(instance.get("memory_megabytes") or 0),
                "profile_tag": instance.get("profile_tag", ""),
                "count": 0,
                "duration_secs": 0.0,
            },
        )
        shape["count"] += 1
        shape["duration_secs"] += duration_secs

    summarized_shapes = sorted(
        machine_shapes.values(),
        key=lambda item: (item["os"], item["arch"], item["profile_tag"]),
    )
    return {
        "instances_count": len(instances),
        "provider_runtime_secs": round(total_runtime, 1),
        "machine_shapes": summarized_shapes,
    }


def enrich_cloud_record_provider_metadata(record: dict) -> dict:
    updated = normalize_cloud_record(record)
    provider = updated.get("provider_resolved") or updated.get("provider_requested") or "github-hosted"
    if provider != "namespace" or not updated.get("run_id") or not nsc_logged_in():
        if provider != "namespace":
            updated["provider_metadata"] = {}
            updated["usage_summary"] = {}
            updated["cost_summary"] = {}
        return updated

    instances = namespace_instances_for_run(updated.get("repository", ""), int(updated["run_id"]))
    if not instances:
        return updated

    updated["provider_metadata"] = {"namespace_instances": instances}
    updated["usage_summary"] = summarize_namespace_usage(instances)
    updated["cost_summary"] = {
        "status": "unavailable",
        "reason": "Namespace CLI does not expose billing totals; provider runtime is shown instead.",
    }
    return updated


def empty_evidence_index() -> dict:
    return {"version": 3, "entries": {}}


def evidence_entry_key(branch: str, sha: str, target: str, validation: str) -> str:
    return f"{branch}:{sha}:{validation}:{target}"


def normalize_evidence_index(index: dict | None) -> dict:
    if not isinstance(index, dict):
        return empty_evidence_index()
    entries = index.get("entries")
    if not isinstance(entries, dict):
        entries = {}
    return {"version": int(index.get("version", 1)), "entries": entries}


def evidence_record_from_result(result: dict, item: dict, result_path: Path) -> dict:
    return {
        "job_id": result.get("job_id", ""),
        "branch": result.get("branch", ""),
        "sha": result.get("sha", ""),
        "validation": result.get("validation", "full"),
        "provenance": normalize_provenance(result.get("provenance")),
        "target": item.get("target", ""),
        "status": item.get("status", ""),
        "completed_at": result.get("completed_at", ""),
        "duration_secs": item.get("duration_secs", 0),
        "result_file": str(result_path),
    }


def merge_result_into_evidence_index(index: dict, result: dict, result_path: Path) -> bool:
    changed = False
    for item in result.get("results", []):
        if item.get("status") != "pass":
            continue
        record = evidence_record_from_result(result, item, result_path)
        key = evidence_entry_key(
            record["branch"], record["sha"], record["target"], record["validation"]
        )
        existing = index["entries"].get(key)
        if existing and existing.get("completed_at", "") >= record["completed_at"]:
            continue
        index["entries"][key] = record
        changed = True
    return changed


def rebuild_evidence_index_unlocked() -> dict:
    index = empty_evidence_index()
    for path in sorted(results_dir().glob("*.json")):
        try:
            result = load_result(path)
        except (OSError, json.JSONDecodeError):
            continue
        merge_result_into_evidence_index(index, result, path)
    return index


def load_evidence_index_unlocked() -> tuple[dict, bool]:
    path = evidence_path()
    if not path.exists():
        return rebuild_evidence_index_unlocked(), True

    try:
        index = normalize_evidence_index(json.loads(path.read_text()))
    except (OSError, json.JSONDecodeError):
        return rebuild_evidence_index_unlocked(), True
    if index.get("version") != empty_evidence_index()["version"]:
        return rebuild_evidence_index_unlocked(), True
    return index, False


def save_evidence_index_unlocked(index: dict) -> None:
    atomic_write_text(evidence_path(), json.dumps(index, indent=2) + "\n")


def load_evidence_index() -> dict:
    with file_lock(evidence_lock_path(), blocking=True):
        index, rebuilt = load_evidence_index_unlocked()
        if rebuilt:
            save_evidence_index_unlocked(index)
        return index


def update_evidence_index(result: dict, result_path: Path) -> None:
    with file_lock(evidence_lock_path(), blocking=True):
        index, rebuilt = load_evidence_index_unlocked()
        changed = merge_result_into_evidence_index(index, result, result_path)
        if rebuilt or changed:
            save_evidence_index_unlocked(index)


def collect_evidence_groups(branch: str | None = None, sha: str | None = None) -> dict[str, list[dict]]:
    index = load_evidence_index()
    grouped: dict[str, dict[str, dict]] = defaultdict(dict)

    for record in index.get("entries", {}).values():
        if branch and record.get("branch") != branch:
            continue
        if sha and record.get("sha") != sha:
            continue

        validation = record.get("validation", "full")
        sha_value = record.get("sha", "")
        if not sha_value:
            continue

        bucket = grouped[validation].setdefault(
            sha_value,
            {
                "sha": sha_value,
                "branch": record.get("branch", ""),
                "validation": validation,
                "completed_at": record.get("completed_at", ""),
                "targets": {},
            },
        )
        bucket["targets"][record.get("target", "")] = record
        if record.get("completed_at", "") > bucket.get("completed_at", ""):
            bucket["completed_at"] = record.get("completed_at", "")

    return {
        validation: sorted(
            sha_groups.values(),
            key=lambda item: (item.get("completed_at", ""), item.get("sha", "")),
            reverse=True,
        )
        for validation, sha_groups in grouped.items()
    }


def print_evidence_summary(
    *,
    branch: str | None = None,
    sha: str | None = None,
    limit: int = 3,
    indent: str = "",
) -> bool:
    groups = collect_evidence_groups(branch=branch, sha=sha)
    if not groups:
        return False

    for validation in sorted(groups):
        print(f"{indent}{validation}:")
        for item in groups[validation][:limit]:
            targets = ", ".join(f"{target}=pass" for target in sorted(item.get("targets", {})))
            print(
                f"{indent}  {short_sha(item.get('sha', ''))} [{targets}] "
                f"last={item.get('completed_at', '?')} "
                f"via {provenance_summary(item.get('provenance'))}"
            )
    return True


def wait_for_job(job_id: str, config: dict) -> tuple[dict | None, int]:
    announced_wait = False

    while True:
        job = load_job(job_id)
        if job is None:
            print(f"Job not found: {job_id}")
            return None, 1

        if job.get("status") == "completed":
            result_file = job.get("result_file")
            if not result_file:
                print(f"Job completed without a result file: {job_id}")
                return None, 1
            result = load_result(Path(result_file))
            return result, 0 if result.get("overall") == "pass" else 1

        acquired, _ = drain_pending_jobs(config, blocking=False)
        if acquired:
            continue

        runner = current_runner_info()
        if runner and not announced_wait:
            active_job = runner.get("active_job_id")
            active_branch = runner.get("active_branch")
            if active_job and active_branch:
                print(
                    f"Another local CI runner is active [{active_job}] {active_branch}; waiting for {job_id}..."
                )
            else:
                print("Another local CI runner is active; waiting for queued job completion...")
            announced_wait = True

        time.sleep(WAIT_POLL_SECS)


def notify(message: str) -> None:
    print("\a", end="", flush=True)
    try:
        subprocess.run(
            ["osascript", "-e", f'display notification "{message}" with title "Pulp CI"'],
            capture_output=True,
            timeout=5,
        )
    except Exception:
        pass


# ── VM Management ────────────────────────────────────────────────────────────


def ssh_reachable(host: str, timeout: int = 5) -> bool:
    result = subprocess.run(
        ["ssh", "-o", f"ConnectTimeout={timeout}", "-o", "BatchMode=yes", host, "echo", "up"],
        capture_output=True,
        text=True,
    )
    return result.returncode == 0


def utmctl_vm_status(vm_name: str) -> str | None:
    result = subprocess.run(["utmctl", "list"], capture_output=True, text=True)
    if result.returncode != 0:
        return None
    for line in result.stdout.splitlines():
        if vm_name in line:
            parts = line.split()
            if len(parts) >= 2:
                return parts[1]
    return None


def utmctl_start(vm_name: str) -> bool:
    result = subprocess.run(["utmctl", "start", vm_name], capture_output=True, text=True)
    return result.returncode == 0


def ensure_host_reachable(target_name: str, target_cfg: dict, defaults: dict) -> str | None:
    host = target_cfg["host"]
    fallback_host = target_cfg.get("fallback_host")
    timeout = defaults.get("ssh_timeout_secs", 5)

    print(f"  [{target_name}] Checking ssh {host}...")
    if ssh_reachable(host, timeout):
        print(f"  [{target_name}] {host} is up")
        return host

    if fallback_host:
        print(f"  [{target_name}] {host} unreachable, trying fallback ssh {fallback_host}...")
        if ssh_reachable(fallback_host, timeout):
            print(f"  [{target_name}] {fallback_host} is up")
            return fallback_host

    fallback = target_cfg.get("utm_fallback")
    if not fallback:
        print(f"  [{target_name}] {host} unreachable, no UTM fallback configured")
        return None

    vm_name = fallback["vm_name"]
    boot_wait = fallback.get("boot_wait_secs", 30)
    ssh_retry = fallback.get("ssh_retry_secs", 60)

    print(f"  [{target_name}] {host} unreachable, checking UTM VM '{vm_name}'...")
    status = utmctl_vm_status(vm_name)
    if status is None:
        print(f"  [{target_name}] UTM VM '{vm_name}' not found")
        return None

    if status != "started":
        print(f"  [{target_name}] Starting UTM VM '{vm_name}'...")
        if not utmctl_start(vm_name):
            print(f"  [{target_name}] Failed to start UTM VM")
            return None
        print(f"  [{target_name}] Waiting {boot_wait}s for boot...")
        time.sleep(boot_wait)

    deadline = time.time() + ssh_retry
    attempt = 0
    while time.time() < deadline:
        attempt += 1
        if ssh_reachable(host, timeout):
            print(f"  [{target_name}] {host} up after UTM start (attempt {attempt})")
            return host
        time.sleep(5)

    print(f"  [{target_name}] {host} still unreachable after UTM start")
    return None


def config_source_name(path: Path) -> str:
    override = os.environ.get("PULP_LOCAL_CI_CONFIG")
    if override:
        return "env-override"
    if path == shared_config_path():
        return "shared-state"
    return "worktree-local"


def config_material_for_targets(config: dict, targets: list[str]) -> dict:
    material: dict[str, dict] = {}
    for name in targets:
        target_cfg = config.get("targets", {}).get(name)
        if not target_cfg:
            continue
        entry = {
            "type": target_cfg.get("type", "local"),
            "enabled": bool(target_cfg.get("enabled", True)),
        }
        for key in (
            "host",
            "fallback_host",
            "repo_path",
            "utm_fallback",
            "cmake_generator",
            "cmake_platform",
            "cmake_generator_instance",
        ):
            value = target_cfg.get(key)
            if value not in (None, "", {}):
                entry[key] = value
        material[name] = entry
    return material


def find_material_config_drift(targets: list[str]) -> list[str]:
    shared_path = shared_config_path()
    worktree_path = worktree_config_path()
    if not shared_path.exists() or not worktree_path.exists():
        return []
    try:
        shared_cfg = json.loads(shared_path.read_text())
        worktree_cfg = json.loads(worktree_path.read_text())
    except json.JSONDecodeError:
        return []

    drift: list[str] = []
    shared_material = config_material_for_targets(shared_cfg, targets)
    worktree_material = config_material_for_targets(worktree_cfg, targets)
    for name in targets:
        shared_entry = shared_material.get(name)
        worktree_entry = worktree_material.get(name)
        if shared_entry == worktree_entry:
            continue
        drift.append(
            f"{name}: shared-state {shared_entry or '(missing)'} vs worktree-local {worktree_entry or '(missing)'}"
        )
    return drift


def preflight_target_host_state(target_name: str, target_cfg: dict, defaults: dict) -> dict:
    target_type = target_cfg.get("type", "local")
    if target_type != "ssh":
        return {"target": target_name, "transport_mode": "local", "status": "local"}

    host = target_cfg.get("host", "")
    fallback_host = target_cfg.get("fallback_host")
    timeout = defaults.get("ssh_timeout_secs", 5)
    state = {
        "target": target_name,
        "transport_mode": "bundle",
        "configured_host": host,
        "repo_path": target_cfg.get("repo_path"),
        "status": "unknown",
    }

    if host and ssh_reachable(host, timeout):
        state["status"] = "primary-up"
        state["resolved_host"] = host
        return state

    if fallback_host and ssh_reachable(fallback_host, timeout):
        state["status"] = "fallback-up"
        state["resolved_host"] = fallback_host
        state["warning"] = f"{target_name}: primary host {host} is down; fallback {fallback_host} is up"
        return state

    utm_fallback = target_cfg.get("utm_fallback")
    if utm_fallback:
        vm_name = utm_fallback.get("vm_name", "(unknown)")
        state["status"] = "utm-fallback-pending"
        state["resolved_host"] = host
        state["warning"] = f"{target_name}: ssh host {host} is down; queued run would need UTM fallback '{vm_name}'"
        return state

    state["status"] = "unreachable"
    state["resolved_host"] = host
    state["error"] = f"{target_name}: ssh host {host} is down and no fallback host or UTM VM is configured"
    return state


def build_submission_metadata(
    config: dict,
    branch: str,
    sha: str,
    targets: list[str],
    priority: str,
    validation: str,
    *,
    allow_root_mismatch: bool,
    allow_unreachable_targets: bool,
) -> dict:
    cwd = Path.cwd().resolve()
    cwd_git_root = git_root_for(cwd)
    submission_root = ROOT.resolve()

    if cwd_git_root and cwd_git_root != submission_root and not allow_root_mismatch:
        raise ValueError(
            "Invoked from a different git root than the queued worktree. "
            f"cwd git root={cwd_git_root}, submission root={submission_root}. "
            "Run the worktree-local tools/local-ci/local_ci.py or pass --allow-root-mismatch."
        )

    config_file = config_path().resolve()
    host_preflight: dict[str, dict] = {}
    warnings: list[str] = []
    errors: list[str] = []
    defaults = config.get("defaults", {})
    for name in targets:
        state = preflight_target_host_state(name, config.get("targets", {}).get(name, {}), defaults)
        host_preflight[name] = state
        if state.get("warning"):
            warnings.append(state["warning"])
        if state.get("error"):
            errors.append(state["error"])

    if errors and not allow_unreachable_targets:
        raise ValueError("; ".join(errors) + ". Pass --allow-unreachable-targets to queue anyway.")

    config_drift = [] if os.environ.get("PULP_LOCAL_CI_CONFIG") else find_material_config_drift(targets)
    if config_drift:
        warnings.append("config drift detected between shared-state and worktree-local config")

    return {
        "submitted_root": str(submission_root),
        "cwd": str(cwd),
        "cwd_git_root": str(cwd_git_root) if cwd_git_root else "",
        "config_path": str(config_file),
        "config_source": config_source_name(config_file),
        "branch": branch,
        "sha": sha,
        "priority": priority,
        "validation": validation,
        "targets": targets,
        "target_hosts": host_preflight,
        "config_drift": config_drift,
        "warnings": warnings,
        "provenance": normalize_provenance(),
    }


def print_submission_metadata(metadata: dict) -> None:
    print(
        "Submitting: "
        f"{metadata['branch']} @ {short_sha(metadata['sha'])} "
        f"priority={metadata['priority']} targets={','.join(metadata['targets']) or 'none'}"
    )
    print(f"  root: {metadata['submitted_root']}")
    print(f"  cwd: {metadata['cwd']}")
    if metadata.get("cwd_git_root"):
        print(f"  cwd git root: {metadata['cwd_git_root']}")
    print(f"  config: {metadata['config_path']} ({metadata['config_source']})")
    if metadata.get("provenance"):
        print(f"  provenance: {provenance_summary(metadata.get('provenance'))}")
    for drift in metadata.get("config_drift", []):
        print(f"  config drift: {drift}")
    for target_name in metadata.get("targets", []):
        state = metadata.get("target_hosts", {}).get(target_name, {})
        transport = state.get("transport_mode", "local")
        if transport == "local":
            print(f"  {target_name}: local transport")
            continue
        resolved = state.get("resolved_host") or state.get("configured_host") or "?"
        status = state.get("status", "unknown")
        repo_path = state.get("repo_path") or "?"
        print(f"  {target_name}: host={resolved} status={status} transport={transport} repo={repo_path}")
    for warning in metadata.get("warnings", []):
        print(f"  warning: {warning}")


# ── Validation Runners ───────────────────────────────────────────────────────


def remote_commit_error(target_name: str, host: str, job: dict) -> str:
    return (
        f"{target_name} cannot validate {short_sha(job['sha'])} on {host}: "
        f"commit is not available on origin. Push the branch first or use --targets mac."
    )


def parse_progress_marker(line: str) -> dict:
    stripped = line.strip()
    if stripped.startswith("__PULP_PHASE__:"):
        return {"phase": stripped.split(":", 1)[1]}
    if stripped.startswith("__PULP_WAIT__:"):
        return {"wait_reason": stripped.split(":", 1)[1]}
    if stripped.startswith("__PULP_VALIDATION__:"):
        return {"validation_mode": stripped.split(":", 1)[1]}
    if stripped.startswith("__PULP_TEST_POLICY__:"):
        return {"test_policy": stripped.split(":", 1)[1]}
    if stripped.startswith("__PULP_PREPARED__:"):
        return {"prepared_state": stripped.split(":", 1)[1]}
    if stripped.startswith("__PULP_VALIDATOR_PID__:"):
        value = stripped.split(":", 1)[1]
        try:
            return {"validator_pid": int(value)}
        except ValueError:
            return {"validator_pid": value}
    if stripped.startswith("__PULP_VALIDATOR_STARTED__:"):
        return {"validator_started_at": stripped.split(":", 1)[1]}
    return {}


def prepared_state_root(target_name: str, validation: str) -> Path:
    return state_dir() / "prepared" / target_name / normalize_validation_mode(validation)


def should_reuse_prepared_state(job: dict) -> bool:
    return len(job.get("targets", [])) == 1


def run_logged_command(
    cmd: list[str],
    *,
    cwd: Path | None = None,
    input_text: str | None = None,
    timeout: int = 3600,
    log_path: Path | None = None,
    report_progress=None,
    heartbeat_interval_secs: float = HEARTBEAT_INTERVAL_SECS,
    stuck_idle_secs: float = STUCK_IDLE_SECS,
) -> dict:
    start = time.time()
    proc = subprocess.Popen(
        cmd,
        cwd=cwd,
        stdin=subprocess.PIPE if input_text is not None else None,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        bufsize=1,
    )

    output_queue: queue_module.Queue[str | None] = queue_module.Queue()
    input_error: list[BaseException] = []
    input_done = threading.Event()

    def reader() -> None:
        assert proc.stdout is not None
        for line in proc.stdout:
            output_queue.put(line)
        output_queue.put(None)

    threading.Thread(target=reader, daemon=True).start()

    def writer() -> None:
        try:
            if input_text is not None and proc.stdin is not None:
                proc.stdin.write(input_text)
        except BaseException as exc:  # pragma: no cover - surfaced through polling loop
            input_error.append(exc)
        finally:
            if proc.stdin is not None:
                try:
                    proc.stdin.close()
                except OSError:
                    pass
            input_done.set()

    threading.Thread(target=writer, daemon=True).start()

    combined: list[str] = []
    saw_eof = False
    last_output_ts = start
    last_heartbeat_ts = start
    log_handle = log_path.open("a", errors="replace") if log_path else None
    try:
        while True:
            remaining = timeout - (time.time() - start)
            if remaining <= 0:
                proc.kill()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    pass
                return {
                    "timed_out": True,
                    "returncode": -1,
                    "output": "".join(combined),
                    "duration_secs": round(time.time() - start, 1),
                }

            if input_error:
                proc.kill()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    pass
                raise input_error[0]

            try:
                poll_timeout = 0.25
                if heartbeat_interval_secs > 0:
                    poll_timeout = min(poll_timeout, max(heartbeat_interval_secs / 2.0, 0.01))
                item = output_queue.get(timeout=min(poll_timeout, max(remaining, 0.01)))
            except queue_module.Empty:
                if proc.poll() is not None and saw_eof and input_done.is_set():
                    break
                now = time.time()
                quiet_for_secs_raw = now - last_output_ts
                quiet_for_secs = int(round(quiet_for_secs_raw))
                if (
                    report_progress
                    and proc.poll() is None
                    and (now - last_heartbeat_ts) >= heartbeat_interval_secs
                ):
                    report_progress(
                        last_heartbeat_at=now_iso(),
                        quiet_for_secs=quiet_for_secs,
                        liveness="stuck" if quiet_for_secs_raw >= stuck_idle_secs else "quiet",
                    )
                    last_heartbeat_ts = now
                continue

            if item is None:
                saw_eof = True
                if proc.poll() is not None and input_done.is_set():
                    break
                continue

            progress = parse_progress_marker(item)
            if progress:
                combined.append(item)
                if log_handle is not None:
                    log_handle.write(item)
                    log_handle.flush()
                last_output_ts = time.time()
                last_heartbeat_ts = last_output_ts
                progress["last_output_at"] = now_iso()
                progress["last_heartbeat_at"] = None
                progress["quiet_for_secs"] = None
                progress["liveness"] = None
                if report_progress:
                    report_progress(**progress)
                continue

            combined.append(item)
            if log_handle is not None:
                log_handle.write(item)
                log_handle.flush()

            stripped = item.strip()
            if report_progress:
                last_output_ts = time.time()
                last_heartbeat_ts = last_output_ts
                fields = {
                    "last_output_at": now_iso(),
                    "last_heartbeat_at": None,
                    "quiet_for_secs": None,
                    "liveness": None,
                }
                if stripped:
                    fields["last_line"] = trim_line(stripped)
                report_progress(**fields)

        return {
            "timed_out": False,
            "returncode": proc.wait(),
            "output": "".join(combined),
            "duration_secs": round(time.time() - start, 1),
        }
    finally:
        if proc.stdout is not None:
            proc.stdout.close()
        if log_handle is not None:
            log_handle.close()


def run_local_validation(job: dict, exclude_tests: str = "", report_progress=None) -> dict:
    print(f"  [mac] Running local validation on {job['branch']} @ {short_sha(job['sha'])}...")
    log_path = prepare_target_log(job["id"], "mac")
    if report_progress:
        report_progress(
            phase="validate",
            log_path=str(log_path),
            last_output_at=now_iso(),
            transport_mode="local",
        )

    validation = job.get("validation", "full")
    prepared_root = prepared_state_root("mac", validation)
    reuse_prepared = should_reuse_prepared_state(job)
    env_args = [
        f"PULP_VALIDATE_ROOT_OVERRIDE={prepared_root}",
        f"PULP_VALIDATE_REUSE_PREPARED={'1' if reuse_prepared else '0'}",
    ]
    cmd = ["env", *env_args, "./validate-build.sh", "--quiet", "--keep-worktree", "--ref", job["sha"]]
    if validation == "smoke":
        cmd = [
            "env",
            *env_args,
            "PULP_EXPECT_SMOKE=1",
            "./validate-build.sh",
            "--quiet",
            "--keep-worktree",
            "--ref",
            job["sha"],
            "--smoke",
            "--no-tests",
        ]
    if exclude_tests:
        cmd += ["--exclude-regex", exclude_tests]

    run = run_logged_command(cmd, cwd=ROOT, timeout=3600, log_path=log_path, report_progress=report_progress)
    if run["timed_out"]:
        return {
            "target": "mac",
            "status": "timeout",
            "exit_code": -1,
            "duration_secs": run["duration_secs"],
            "stdout_tail": "",
            "stderr_tail": "Validation timed out after 3600s",
            "log_file": str(log_path),
            "transport_mode": "local",
        }
    tail = run["output"][-2000:] if run["output"] else ""
    if validation == "smoke":
        if "__PULP_VALIDATION__:smoke" not in run["output"] or "__PULP_TEST_POLICY__:skip" not in run["output"]:
            failed = True
            tail = (
                "Smoke validation contract violated: expected validation=smoke and test_policy=skip markers.\n"
                + tail
            )[-2000:]
        else:
            failed = run["returncode"] != 0
    else:
        failed = run["returncode"] != 0
    return {
        "target": "mac",
        "status": "pass" if not failed else "fail",
        "exit_code": run["returncode"],
        "duration_secs": run["duration_secs"],
        "stdout_tail": "" if failed else tail,
        "stderr_tail": tail if failed else "",
        "log_file": str(log_path),
        "validation": validation,
        "transport_mode": "local",
    }


def run_posix_ssh_validation(
    target_name: str,
    host: str,
    repo_path: str,
    job: dict,
    exclude_tests: str = "",
    report_progress=None,
) -> dict:
    print(f"  [{target_name}] Running validation on {host}:{repo_path} @ {short_sha(job['sha'])}...")
    log_path = prepare_target_log(job["id"], target_name)
    if report_progress:
        report_progress(
            phase="connect",
            host=host,
            log_path=str(log_path),
            last_output_at=now_iso(),
            transport_mode="bundle",
        )

    try:
        bundle_name, bundle_ref = sync_job_bundle_to_ssh_host(host, job, report_progress=report_progress)
    except RuntimeError as exc:
        return {
            "target": target_name,
            "status": "error",
            "exit_code": -1,
            "duration_secs": 0.0,
            "stdout_tail": "",
            "stderr_tail": str(exc),
            "log_file": str(log_path),
            "transport_mode": "bundle",
        }

    branch_q = shlex.quote(job["branch"])
    sha_q = shlex.quote(job["sha"])
    repo_q = shlex.quote(repo_path)
    bundle_name_q = shlex.quote(bundle_name)
    bundle_ref_q = shlex.quote(bundle_ref)
    script_name_q = shlex.quote(f".pulp-ci-validate-{job['id']}.sh")
    validation = normalize_validation_mode(job.get("validation", "full"))
    reuse_prepared_q = shlex.quote("1" if should_reuse_prepared_state(job) else "0")
    remote_cmd = (
        "set -euo pipefail; "
        f"branch={branch_q}; "
        f"sha={sha_q}; "
        f"bundle_name={bundle_name_q}; "
        f"bundle_ref={bundle_ref_q}; "
        f"script_name={script_name_q}; "
        f"reuse_prepared={reuse_prepared_q}; "
        "bundle=\"$HOME/$bundle_name\"; "
        f"prepared_root=\"$HOME/.local/state/pulp/local-ci/prepared/{target_name}/{validation}\"; "
        "script=''; "
        "trap 'rm -f \"$bundle\" \"$script\"' EXIT; "
        "export GIT_LFS_SKIP_SMUDGE=1; "
        f"cd {repo_q}; "
        "script=\"$PWD/$script_name\"; "
        "if [ -f \"$bundle\" ]; then "
        "printf '__PULP_PHASE__:bundle-sync\n'; "
        "git fetch \"$bundle\" \"$bundle_ref:refs/remotes/origin/$branch\" >/dev/null 2>&1 || true; "
        "fi; "
        "printf '__PULP_PHASE__:fetch\n'; "
        "git fetch origin >/dev/null 2>&1 || true; "
        "if ! git cat-file -e \"$sha^{commit}\" 2>/dev/null; then "
        "git fetch origin \"refs/heads/$branch:refs/remotes/origin/$branch\" >/dev/null 2>&1 || true; "
        "fi; "
        "if ! git cat-file -e \"$sha^{commit}\" 2>/dev/null; then "
        f"echo {shlex.quote(remote_commit_error(target_name, host, job))} >&2; "
        "exit 2; "
        "fi; "
        "printf '__PULP_PHASE__:validate\n'; "
        "git show \"$sha:validate-build.sh\" > \"$script\"; "
        "chmod +x \"$script\"; "
        "PULP_VALIDATE_ROOT_OVERRIDE=\"$prepared_root\" "
        "PULP_VALIDATE_REUSE_PREPARED=\"$reuse_prepared\" "
        "PULP_EXPECT_SMOKE=0 "
        "bash \"$script\" --quiet --keep-worktree --ref \"$sha\""
    )
    if validation == "smoke":
        remote_cmd = remote_cmd.replace("PULP_EXPECT_SMOKE=0", "PULP_EXPECT_SMOKE=1", 1)
        remote_cmd += " --smoke --no-tests"
    if exclude_tests:
        remote_cmd += f" --exclude-regex {shlex.quote(exclude_tests)}"

    cmd = ["ssh", host, "bash", "-lc", shlex.quote(remote_cmd)]

    run = run_logged_command(cmd, timeout=3600, log_path=log_path, report_progress=report_progress)
    if run["timed_out"]:
        return {
            "target": target_name,
            "status": "timeout",
            "exit_code": -1,
            "duration_secs": run["duration_secs"],
            "stdout_tail": "",
            "stderr_tail": "Validation timed out after 3600s",
            "log_file": str(log_path),
            "transport_mode": "bundle",
        }
    tail = run["output"][-2000:] if run["output"] else ""
    if validation == "smoke":
        if "__PULP_VALIDATION__:smoke" not in run["output"] or "__PULP_TEST_POLICY__:skip" not in run["output"]:
            failed = True
            tail = (
                "Smoke validation contract violated: expected validation=smoke and test_policy=skip markers.\n"
                + tail
            )[-2000:]
        else:
            failed = run["returncode"] != 0
    else:
        failed = run["returncode"] != 0
    return {
        "target": target_name,
        "status": "pass" if not failed else "fail",
        "exit_code": run["returncode"],
        "duration_secs": run["duration_secs"],
        "stdout_tail": "" if failed else tail,
        "stderr_tail": tail if failed else "",
        "log_file": str(log_path),
        "validation": validation,
        "transport_mode": "bundle",
    }


def ps_literal(value: str) -> str:
    return value.replace("'", "''")


def windows_ssh_powershell_command(host: str) -> list[str]:
    # `powershell -Command -` silently no-ops some multi-line try/finally scripts on WinRM/OpenSSH.
    # Read stdin explicitly and invoke it so complex validation scripts execute reliably.
    return [
        "ssh",
        host,
        "powershell",
        "-NoProfile",
        "-NonInteractive",
        "-Command",
        "$script = [Console]::In.ReadToEnd(); Invoke-Expression $script",
    ]


def probe_windows_ssh_cmake_settings(
    host: str,
    cmake_generator: str,
    cmake_platform: str,
    cmake_generator_instance: str,
) -> tuple[str, str]:
    if cmake_platform and cmake_generator_instance:
        return cmake_platform, cmake_generator_instance

    ps_script = f"""
$RequestedPlatform = '{ps_literal(cmake_platform)}'
$RequestedGeneratorInstance = '{ps_literal(cmake_generator_instance)}'
$Generator = '{ps_literal(cmake_generator)}'

function Resolve-CMakePlatform {{
    param([string]$Requested)
    if ($Requested) {{
        return $Requested
    }}
    if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') {{
        return 'ARM64'
    }}
    return 'x64'
}}

function Resolve-VisualStudioInstance {{
    param([string]$Requested, [string]$Generator)
    if ($Requested) {{
        return $Requested
    }}
    if (-not $Generator -or -not $Generator.StartsWith('Visual Studio')) {{
        return ''
    }}
    $vswhere = Join-Path ${{env:ProgramFiles(x86)}} 'Microsoft Visual Studio\\Installer\\vswhere.exe'
    if (-not (Test-Path $vswhere)) {{
        return ''
    }}
    try {{
        $raw = (& $vswhere -latest -products * -format json) -join "`n"
        if (-not $raw) {{
            return ''
        }}
        $instances = $raw | ConvertFrom-Json
        if ($instances -isnot [System.Array]) {{
            $instances = @($instances)
        }}
        $preferred = $instances | Where-Object {{
            $_.productId -and $_.productId -ne 'Microsoft.VisualStudio.Product.BuildTools'
        }} | Select-Object -First 1
        if (-not $preferred) {{
            $preferred = $instances | Select-Object -First 1
        }}
        if ($preferred -and $preferred.installationPath) {{
            return $preferred.installationPath.Replace('\\', '/')
        }}
    }} catch {{
    }}
    return ''
}}

$resolved = @{{
    platform = Resolve-CMakePlatform $RequestedPlatform
    generator_instance = Resolve-VisualStudioInstance $RequestedGeneratorInstance $Generator
}}
$resolved | ConvertTo-Json -Compress
"""

    try:
        run = subprocess.run(
            windows_ssh_powershell_command(host),
            input=ps_script,
            capture_output=True,
            text=True,
            timeout=60,
        )
    except (subprocess.SubprocessError, OSError):
        return cmake_platform, cmake_generator_instance

    if run.returncode != 0:
        return cmake_platform, cmake_generator_instance

    for line in reversed(run.stdout.splitlines()):
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            resolved = json.loads(line)
        except json.JSONDecodeError:
            continue
        return (
            resolved.get("platform") or cmake_platform,
            resolved.get("generator_instance") or cmake_generator_instance,
        )
    return cmake_platform, cmake_generator_instance


def run_windows_ssh_validation(
    target_name: str,
    host: str,
    repo_path: str,
    job: dict,
    exclude_tests: str = "",
    cmake_generator: str = "Visual Studio 17 2022",
    cmake_platform: str = "",
    cmake_generator_instance: str = "",
    report_progress=None,
) -> dict:
    print(f"  [{target_name}] Running validation on {host}:{repo_path} @ {short_sha(job['sha'])}...")
    log_path = prepare_target_log(job["id"], target_name)
    if report_progress:
        report_progress(
            phase="connect",
            host=host,
            log_path=str(log_path),
            last_output_at=now_iso(),
            transport_mode="bundle",
        )

    try:
        bundle_name, bundle_ref = sync_job_bundle_to_ssh_host(host, job, report_progress=report_progress)
    except RuntimeError as exc:
        return {
            "target": target_name,
            "status": "error",
            "exit_code": -1,
            "duration_secs": 0.0,
            "stdout_tail": "",
            "stderr_tail": str(exc),
            "log_file": str(log_path),
            "transport_mode": "bundle",
        }

    resolved_platform, resolved_generator_instance = probe_windows_ssh_cmake_settings(
        host,
        cmake_generator,
        cmake_platform,
        cmake_generator_instance,
    )

    ps_script = f"""
$ErrorActionPreference = 'Stop'

function Invoke-Native {{
    param([string]$File, [string[]]$Arguments)
    & $File @Arguments
    if ($LASTEXITCODE -ne 0) {{
        throw "$File exited with code $LASTEXITCODE"
    }}
}}

function Test-CommitRef {{
    param([string]$Ref)
    & git rev-parse --verify --quiet "$Ref`^{{commit}}" 1> $null 2> $null
    return $LASTEXITCODE -eq 0
}}

function Remove-WorktreeSafe {{
    param([string]$RepoRoot, [string]$Path)
    try {{
        Invoke-Native git @('-C', $RepoRoot, 'worktree', 'remove', '--force', '--force', $Path)
    }} catch {{
    }}
    if (Test-Path $Path) {{
        try {{
            Remove-Item -Recurse -Force -ErrorAction Stop $Path
        }} catch {{
        }}
    }}
    try {{
        Invoke-Native git @('-C', $RepoRoot, 'worktree', 'prune', '--expire', 'now')
    }} catch {{
    }}
}}

function Remove-PreparedRoot {{
    param([string]$RepoRoot, [string]$PreparedRoot)

    $PreparedSrc = Join-Path $PreparedRoot 'src'
    if (Test-Path $PreparedSrc) {{
        Remove-WorktreeSafe $RepoRoot $PreparedSrc
    }}
    if (Test-Path $PreparedRoot) {{
        try {{
            Remove-Item -Recurse -Force -ErrorAction Stop $PreparedRoot
        }} catch {{
        }}
    }}
}}

function Test-PreparedStateMatches {{
    param(
        [string]$StatePath,
        [string]$ExpectedSha,
        [string]$ExpectedValidation,
        [string]$ExpectedGenerator,
        [string]$ExpectedPlatform,
        [string]$ExpectedGeneratorInstance
    )

    if (-not (Test-Path $StatePath)) {{
        return $false
    }}

    try {{
        $state = Get-Content $StatePath -Raw | ConvertFrom-Json
    }} catch {{
        return $false
    }}

    if (
        $state.sha -ne $ExpectedSha -or
        $state.validation -ne $ExpectedValidation -or
        $state.generator -ne $ExpectedGenerator -or
        $state.platform -ne $ExpectedPlatform -or
        $state.generator_instance -ne $ExpectedGeneratorInstance
    ) {{
        return $false
    }}

    $PreparedRoot = Split-Path $StatePath -Parent
    $PreparedSrc = Join-Path $PreparedRoot 'src'
    $PreparedBuild = Join-Path $PreparedRoot 'build'
    $PreparedInstall = Join-Path $PreparedRoot 'install'
    if (-not (Test-Path $PreparedSrc) -or -not (Test-Path $PreparedBuild) -or -not (Test-Path $PreparedInstall)) {{
        return $false
    }}

    $preparedHead = ((& git -C $PreparedSrc rev-parse HEAD 2>$null) | Select-Object -Last 1).Trim()
    if ($LASTEXITCODE -ne 0) {{
        return $false
    }}
    return $preparedHead -eq $ExpectedSha
}}

function Write-PreparedState {{
    param(
        [string]$StatePath,
        [string]$Sha,
        [string]$Validation,
        [string]$Generator,
        [string]$Platform,
        [string]$GeneratorInstance
    )

    $payload = @{{
        sha = $Sha
        validation = $Validation
        generator = $Generator
        platform = $Platform
        generator_instance = $GeneratorInstance
        updated_at = (Get-Date).ToString('o')
    }}
    $payload | ConvertTo-Json | Set-Content -Path $StatePath
}}

function Wait-HostMutex {{
    param(
        [System.Threading.Mutex]$Mutex,
        [bool]$Immediate
    )

    try {{
        if ($Immediate) {{
            return $Mutex.WaitOne(0)
        }}
        $null = $Mutex.WaitOne()
        return $true
    }} catch [System.Threading.AbandonedMutexException] {{
        Write-Host "Recovered abandoned host validation lock: $MutexName"
        return $true
    }}
}}

$Repo = '{ps_literal(repo_path)}'
$RepoDrive = Split-Path -Path $Repo -Qualifier
if (-not $RepoDrive) {{
    $RepoDrive = 'C:'
}}
$env:GIT_LFS_SKIP_SMUDGE = '1'
$CiRoot = Join-Path $RepoDrive 'pulp-ci'
$Branch = '{ps_literal(job['branch'])}'
$Sha = '{ps_literal(job['sha'])}'
$BundleName = '{ps_literal(bundle_name)}'
$BundleRef = '{ps_literal(bundle_ref)}'
$Bundle = Join-Path $HOME $BundleName
$ExcludeRegex = '{ps_literal(exclude_tests)}'
$Generator = '{ps_literal(cmake_generator)}'
$Platform = '{ps_literal(resolved_platform)}'
$GeneratorInstance = '{ps_literal(resolved_generator_instance)}'
$ValidationMode = '{ps_literal(job.get("validation", "full"))}'
$PreparedRoot = Join-Path $CiRoot 'prepared\\{ps_literal(target_name)}'
$PreparedRoot = Join-Path $PreparedRoot $ValidationMode
$PreparedState = Join-Path $PreparedRoot 'state.json'
$Src = Join-Path $PreparedRoot 'src'
$Build = Join-Path $PreparedRoot 'build'
$Install = Join-Path $PreparedRoot 'install'
$Smoke = Join-Path $PreparedRoot 'smoke'
$ReusePrepared = {'$true' if should_reuse_prepared_state(job) else '$false'}
$UsePrepared = $false
$MutexName = 'Global\\PulpLocalCIValidate'
$Mutex = New-Object System.Threading.Mutex($false, $MutexName)
$LockAcquired = $false
$ValidatorStartedAt = (Get-Process -Id $PID).StartTime.ToUniversalTime().ToString('o')

try {{
    Write-Host "__PULP_VALIDATOR_PID__:$PID"
    Write-Host "__PULP_VALIDATOR_STARTED__:$ValidatorStartedAt"
    if (-not (Wait-HostMutex -Mutex $Mutex -Immediate $true)) {{
        Write-Host "__PULP_WAIT__:host-lock"
        Write-Host "__PULP_PHASE__:waiting-lock"
        Write-Host "Waiting for host validation lock: $MutexName"
        $LockAcquired = Wait-HostMutex -Mutex $Mutex -Immediate $false
    }} else {{
        $LockAcquired = $true
    }}

    Write-Host "__PULP_PHASE__:fetch"
    New-Item -ItemType Directory -Force -Path $PreparedRoot | Out-Null
    Set-Location $Repo
    if (Test-Path $Bundle) {{
        Write-Host "__PULP_PHASE__:bundle-sync"
        try {{
            Invoke-Native git @(
                'fetch',
                $Bundle,
                "$BundleRef`:refs/pulp-ci-bundles/{job['id']}"
            )
        }} finally {{
            Remove-Item -Force -ErrorAction SilentlyContinue $Bundle
        }}
    }}
    try {{
        Invoke-Native git @('fetch', 'origin')
    }} catch {{
    }}

    if (-not (Test-CommitRef $Sha)) {{
        try {{
            Invoke-Native git @(
                'fetch',
                'origin',
                "refs/heads/$Branch`:refs/remotes/origin/$Branch"
            )
        }} catch {{
        }}
    }}

    if (-not (Test-CommitRef $Sha)) {{
        throw '{ps_literal(remote_commit_error(target_name, host, job))}'
    }}

    if ($ReusePrepared -and (Test-PreparedStateMatches `
        -StatePath $PreparedState `
        -ExpectedSha $Sha `
        -ExpectedValidation $ValidationMode `
        -ExpectedGenerator $Generator `
        -ExpectedPlatform $Platform `
        -ExpectedGeneratorInstance $GeneratorInstance)) {{
        $UsePrepared = $true
        Write-Host "__PULP_PREPARED__:reused"
    }} else {{
        Write-Host "__PULP_PREPARED__:clean"
        Remove-PreparedRoot $Repo $PreparedRoot
        New-Item -ItemType Directory -Force -Path $PreparedRoot | Out-Null
        Write-Host "__PULP_PHASE__:worktree"
        Invoke-Native git @('worktree', 'add', '--force', '--detach', $Src, $Sha)
    }}

    try {{
        Write-Host "__PULP_PHASE__:configure"
        Write-Host "CMake platform: $Platform"
        if ($GeneratorInstance) {{
            Write-Host "CMake generator instance: $GeneratorInstance"
        }}
        $configureArgs = @('-S', $Src, '-B', $Build)
        if ($Generator) {{
            $configureArgs += @('-G', $Generator)
        }}
        if ($Platform) {{
            $configureArgs += @('-A', $Platform)
        }}
        if ($GeneratorInstance) {{
            $configureArgs += @("-DCMAKE_GENERATOR_INSTANCE=$GeneratorInstance")
        }}
        $configureArgs += @('-DCMAKE_BUILD_TYPE=Release')
        if ($ValidationMode -eq 'smoke') {{
            $configureArgs += @(
                '-DPULP_BUILD_TESTS=OFF',
                '-DPULP_BUILD_EXAMPLES=OFF',
                '-DPULP_ENABLE_GPU=OFF'
            )
        }}
        Invoke-Native cmake $configureArgs
        Write-Host "__PULP_PHASE__:build"
        Invoke-Native cmake @('--build', $Build, '--config', 'Release')
        if ($ValidationMode -eq 'smoke') {{
            Write-Host "__PULP_PHASE__:install"
            Invoke-Native cmake @('--install', $Build, '--prefix', $Install, '--config', 'Release')
            New-Item -ItemType Directory -Force -Path $Smoke | Out-Null
            @"
cmake_minimum_required(VERSION 3.24)
project(PulpSDKSmoke LANGUAGES CXX)

find_package(Pulp REQUIRED CONFIG)

add_library(smoke INTERFACE)
target_link_libraries(smoke INTERFACE Pulp::format Pulp::standalone)
"@ | Set-Content -Path (Join-Path $Smoke 'CMakeLists.txt')
            Write-Host "__PULP_PHASE__:smoke"
            $smokeConfigureArgs = @('-S', $Smoke, '-B', (Join-Path $Smoke 'build'))
            if ($Generator) {{
                $smokeConfigureArgs += @('-G', $Generator)
            }}
            if ($Platform) {{
                $smokeConfigureArgs += @('-A', $Platform)
            }}
            if ($GeneratorInstance) {{
                $smokeConfigureArgs += @("-DCMAKE_GENERATOR_INSTANCE=$GeneratorInstance")
            }}
            $smokeConfigureArgs += @("-DCMAKE_PREFIX_PATH=$Install")
            Invoke-Native cmake $smokeConfigureArgs
            Write-PreparedState `
                -StatePath $PreparedState `
                -Sha $Sha `
                -Validation $ValidationMode `
                -Generator $Generator `
                -Platform $Platform `
                -GeneratorInstance $GeneratorInstance
        }} else {{
            Write-PreparedState `
                -StatePath $PreparedState `
                -Sha $Sha `
                -Validation $ValidationMode `
                -Generator $Generator `
                -Platform $Platform `
                -GeneratorInstance $GeneratorInstance
            Write-Host "__PULP_PHASE__:test"
            $ctestArgs = @('--test-dir', $Build, '--output-on-failure', '-C', 'Release')
            if ($ExcludeRegex) {{
                $ctestArgs += @('--exclude-regex', $ExcludeRegex)
            }}
            Invoke-Native ctest $ctestArgs
        }}
    }} finally {{
        Write-Host "__PULP_PHASE__:cleanup"
        if (-not (Test-Path $PreparedState)) {{
            Remove-PreparedRoot $Repo $PreparedRoot
        }}
    }}
}} finally {{
    if ($LockAcquired) {{
        try {{
            $Mutex.ReleaseMutex() | Out-Null
        }} catch [System.ApplicationException] {{
        }}
    }}
    $Mutex.Dispose()
}}
""".strip()

    cmd = windows_ssh_powershell_command(host)

    run = run_logged_command(
        cmd,
        input_text=ps_script,
        timeout=3600,
        log_path=log_path,
        report_progress=report_progress,
    )
    if run["timed_out"]:
        return {
            "target": target_name,
            "status": "timeout",
            "exit_code": -1,
            "duration_secs": run["duration_secs"],
            "stdout_tail": "",
            "stderr_tail": "Validation timed out after 3600s",
            "log_file": str(log_path),
            "transport_mode": "bundle",
        }
    tail = run["output"][-2000:] if run["output"] else ""
    failed = run["returncode"] != 0
    return {
        "target": target_name,
        "status": "pass" if not failed else "fail",
        "exit_code": run["returncode"],
        "duration_secs": run["duration_secs"],
        "stdout_tail": "" if failed else tail,
        "stderr_tail": tail if failed else "",
        "log_file": str(log_path),
        "validation": job.get("validation", "full"),
        "transport_mode": "bundle",
    }


# ── Job Processing ───────────────────────────────────────────────────────────


def _build_target_tasks(job: dict, config: dict, progress_factory=None) -> list[tuple[str, object]]:
    targets = config["targets"]
    defaults = config.get("defaults", {})
    requested = set(job.get("targets") or enabled_targets(config))
    tasks: list[tuple[str, object]] = []

    mac_cfg = targets.get("mac", {})
    if "mac" in requested and mac_cfg.get("enabled", True):
        reporter = progress_factory("mac") if progress_factory else None
        tasks.append(("mac", lambda r=reporter: run_local_validation(job, mac_cfg.get("exclude_tests", ""), r)))

    ubuntu_cfg = targets.get("ubuntu")
    if "ubuntu" in requested and ubuntu_cfg and ubuntu_cfg.get("enabled", True):
        host = ensure_host_reachable("ubuntu", ubuntu_cfg, defaults)
        if host:
            exc = ubuntu_cfg.get("exclude_tests", "")
            reporter = progress_factory("ubuntu") if progress_factory else None
            tasks.append(
                (
                    "ubuntu",
                    lambda h=host, e=exc, r=reporter: run_posix_ssh_validation(
                        "ubuntu", h, ubuntu_cfg["repo_path"], job, exclude_tests=e, report_progress=r
                    ),
                )
            )
        else:
            tasks.append(
                (
                    "ubuntu",
                    lambda: {
                        "target": "ubuntu",
                        "status": "unreachable",
                        "exit_code": -1,
                        "duration_secs": 0,
                        "stdout_tail": "",
                        "stderr_tail": "Host unreachable",
                    },
                )
            )

    win_cfg = targets.get("windows")
    if "windows" in requested and win_cfg and win_cfg.get("enabled", True):
        host = ensure_host_reachable("windows", win_cfg, defaults)
        if host:
            exc = win_cfg.get("exclude_tests", "")
            reporter = progress_factory("windows") if progress_factory else None
            generator = win_cfg.get("cmake_generator", "Visual Studio 17 2022")
            platform = win_cfg.get("cmake_platform", "")
            generator_instance = win_cfg.get("cmake_generator_instance", "")
            tasks.append(
                (
                    "windows",
                    lambda h=host, e=exc, r=reporter, g=generator, p=platform, i=generator_instance: run_windows_ssh_validation(
                        "windows",
                        h,
                        win_cfg["repo_path"],
                        job,
                        exclude_tests=e,
                        cmake_generator=g,
                        cmake_platform=p,
                        cmake_generator_instance=i,
                        report_progress=r,
                    ),
                )
            )
        else:
            tasks.append(
                (
                    "windows",
                    lambda: {
                        "target": "windows",
                        "status": "unreachable",
                        "exit_code": -1,
                        "duration_secs": 0,
                        "stdout_tail": "",
                        "stderr_tail": "Host unreachable",
                    },
                )
            )

    return tasks


def process_job(job: dict, config: dict) -> dict:
    print(
        f"\n=== Validating [{job['id']}] {job['branch']} @ {short_sha(job['sha'])} "
        f"priority={job['priority']} ===\n"
    )

    target_states: dict[str, dict] = {}
    state_lock = threading.Lock()

    def flush_target_states() -> None:
        with state_lock:
            snapshot = {name: dict(state) for name, state in target_states.items()}
        update_runner_active_targets(job["id"], snapshot or None)
        update_job_active_targets(job["id"], snapshot or None)

    def progress_factory(name: str):
        def report(**fields) -> None:
            with state_lock:
                state = dict(target_states.get(name, {}))
                for key, value in fields.items():
                    if value is None:
                        state.pop(key, None)
                    else:
                        state[key] = value
                target_states[name] = state
            flush_target_states()

        return report

    tasks = _build_target_tasks(job, config, progress_factory=progress_factory)
    if not tasks:
        return {
            "job_id": job["id"],
            "branch": job["branch"],
            "sha": job["sha"],
            "priority": job["priority"],
            "submission": job.get("submission"),
            "provenance": normalize_provenance(job.get("provenance")),
            "targets": job.get("targets", []),
            "queued_at": job.get("queued_at", ""),
            "completed_at": now_iso(),
            "results": [],
            "overall": "pass",
        }

    for name, _fn in tasks:
        target_states[name] = {
            "status": "running",
            "started_at": now_iso(),
            "phase": "starting",
            "log_path": str(target_log_path(job["id"], name)),
        }
    flush_target_states()

    results = []
    with ThreadPoolExecutor(max_workers=len(tasks)) as pool:
        futures = {pool.submit(fn): name for name, fn in tasks}
        for future in as_completed(futures):
            name = futures[future]
            try:
                result = future.result()
            except Exception as exc:
                result = {
                    "target": name,
                    "status": "error",
                    "exit_code": -1,
                    "duration_secs": 0,
                    "stdout_tail": "",
                    "stderr_tail": str(exc),
                }

            results.append(result)
            target_states[name] = {
                "status": result.get("status", "?"),
                "exit_code": result.get("exit_code"),
                "duration_secs": result.get("duration_secs"),
                "completed_at": now_iso(),
                "phase": "done" if result.get("status") == "pass" else target_states.get(name, {}).get("phase", "done"),
                "log_path": result.get("log_file", str(target_log_path(job["id"], name))),
                "last_output_at": target_states.get(name, {}).get("last_output_at"),
                "last_line": target_states.get(name, {}).get("last_line"),
                "host": target_states.get(name, {}).get("host"),
                "transport_mode": result.get("transport_mode", target_states.get(name, {}).get("transport_mode")),
                "wait_reason": target_states.get(name, {}).get("wait_reason"),
            }
            flush_target_states()

    results.sort(key=lambda item: item["target"])
    return {
        "job_id": job["id"],
        "branch": job["branch"],
        "sha": job["sha"],
        "priority": job["priority"],
        "validation": job.get("validation", "full"),
        "submission": job.get("submission"),
        "provenance": normalize_provenance(job.get("provenance")),
        "targets": job.get("targets", []),
        "queued_at": job.get("queued_at", ""),
        "completed_at": now_iso(),
        "results": results,
        "overall": "pass" if all(result["status"] == "pass" for result in results) else "fail",
    }


def save_result(result: dict) -> Path:
    ensure_state_dirs()
    ts = datetime.now().strftime("%Y%m%d-%H%M%S")
    branch_slug = result["branch"].replace("/", "-")
    path = results_dir() / f"{ts}-{result['job_id']}-{branch_slug}.json"
    path.write_text(json.dumps(result, indent=2) + "\n")
    update_evidence_index(result, path)
    return path


def print_result(result: dict, result_path: Path | None = None) -> None:
    result = normalize_result(result)
    print(f"\n--- Result: [{result['job_id']}] {result['branch']} ---")
    if result.get("validation", "full") != "full":
        print(f"  {'validation':10s}  {result['validation']}")
    print(f"  {'execution':10s}  {provenance_summary(result.get('provenance'))}")
    for item in result["results"]:
        icon = "PASS" if item["status"] == "pass" else item["status"].upper()
        print(f"  {item['target']:10s}  {icon:12s}  {item.get('duration_secs', 0)}s")
    print(f"  {'overall':10s}  {result['overall'].upper()}")
    if result_path:
        print(f"  Saved: {result_path}")
    print()


def drain_pending_jobs(config: dict, *, blocking: bool) -> tuple[bool, bool]:
    acquired = False
    try:
        with file_lock(drain_lock_path(), blocking=blocking):
            acquired = True
            runner_info = {
                "pid": os.getpid(),
                "root": str(ROOT),
                "started_at": now_iso(),
                "active_job_id": None,
                "active_branch": None,
            }
            write_runner_info(runner_info)
            any_failure = False

            while True:
                reclaim_stale_remote_validators(config)
                job = claim_next_job()
                if job is None:
                    break

                runner_info.update(
                    {
                        "active_job_id": job["id"],
                        "active_branch": job["branch"],
                        "updated_at": now_iso(),
                    }
                )
                write_runner_info(runner_info)

                try:
                    result = process_job(job, config)
                except Exception as exc:
                    result = {
                        "job_id": job["id"],
                        "branch": job["branch"],
                        "sha": job["sha"],
                        "priority": job["priority"],
                        "validation": job.get("validation", "full"),
                        "targets": job.get("targets", []),
                        "queued_at": job.get("queued_at", ""),
                        "completed_at": now_iso(),
                        "results": [
                            {
                                "target": "scheduler",
                                "status": "error",
                                "exit_code": -1,
                                "duration_secs": 0,
                                "stdout_tail": "",
                                "stderr_tail": str(exc),
                            }
                        ],
                        "overall": "fail",
                    }

                result_path = save_result(result)
                finalize_job(job["id"], result, result_path)
                print_result(result, result_path)
                if result["overall"] != "pass":
                    any_failure = True

            return True, any_failure
    except LockBusyError:
        return False, False
    finally:
        if acquired:
            clear_runner_info()


# ── GitHub Helpers ───────────────────────────────────────────────────────────


def gh_available() -> bool:
    result = subprocess.run(["gh", "auth", "status"], capture_output=True, text=True)
    return result.returncode == 0


def gh_auth_status_text() -> str:
    result = subprocess.run(["gh", "auth", "status", "-t"], capture_output=True, text=True)
    if result.returncode != 0:
        return ""
    return result.stdout


def gh_token_scopes() -> set[str]:
    status_text = gh_auth_status_text()
    if not status_text:
        return set()
    marker = "Token scopes:"
    for raw_line in status_text.splitlines():
        line = raw_line.strip()
        if marker not in line:
            continue
        suffix = line.split(marker, 1)[1].strip()
        if suffix.startswith("'") and suffix.endswith("'"):
            suffix = suffix[1:-1]
        return {item.strip() for item in suffix.split(",") if item.strip()}
    return set()


def gh_api_json(path: str, *, fields: dict[str, str | int] | None = None) -> tuple[dict | list | None, str]:
    cmd = [
        "gh",
        "api",
        "-H",
        "Accept: application/vnd.github+json",
        "-H",
        "X-GitHub-Api-Version: 2026-03-10",
        path,
    ]
    for key, value in (fields or {}).items():
        cmd += ["-F", f"{key}={value}"]
    result = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if result.returncode != 0:
        detail = (result.stderr or result.stdout or "").strip()
        return None, detail or "gh api failed"
    try:
        payload = json.loads(result.stdout)
    except json.JSONDecodeError:
        return None, "gh api returned invalid JSON"
    return payload, ""


def nsc_run(args: list[str], *, capture_output: bool = True) -> subprocess.CompletedProcess | None:
    try:
        return subprocess.run(
            ["nsc", *args],
            cwd=ROOT,
            capture_output=capture_output,
            text=True,
            check=False,
        )
    except FileNotFoundError:
        return None


def nsc_available() -> bool:
    result = nsc_run(["version"])
    return bool(result and result.returncode == 0)


def nsc_version() -> str | None:
    result = nsc_run(["version"])
    if not result or result.returncode != 0:
        return None
    lines = [line.strip() for line in result.stdout.splitlines() if line.strip()]
    return lines[0] if lines else None


def nsc_logged_in() -> bool:
    result = nsc_run(["auth", "check-login"])
    return bool(result and result.returncode == 0)


def parse_colon_separated_fields(text: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line or ":" not in line:
            continue
        key, value = line.split(":", 1)
        fields[key.strip()] = value.strip()
    return fields


def nsc_workspace_info() -> dict[str, str] | None:
    result = nsc_run(["workspace", "describe"])
    if not result or result.returncode != 0:
        return None
    fields = parse_colon_separated_fields(result.stdout)
    return fields or None


def nsc_instance_history(max_entries: int = 100) -> list[dict]:
    result = nsc_run(["instance", "history", "--all", "-o", "json", "--max_entries", str(max_entries)])
    if not result or result.returncode != 0:
        return []
    try:
        payload = json.loads(result.stdout)
    except json.JSONDecodeError:
        return []
    return payload if isinstance(payload, list) else []


def namespace_instances_for_run(repository: str, run_id: int) -> list[dict]:
    matched: list[dict] = []
    for raw_instance in nsc_instance_history():
        github_workflow = raw_instance.get("github_workflow") or {}
        if github_workflow.get("repository") != repository:
            continue
        if str(github_workflow.get("run_id") or "") != str(run_id):
            continue
        matched.append(normalize_namespace_instance(raw_instance))
    matched.sort(key=lambda item: (item.get("created_at", ""), item.get("cluster_id", "")))
    return matched


def print_namespace_setup_help() -> None:
    print("Recommended Namespace setup:")
    print("  1. Install the `nsc` CLI")
    print("  2. Run `nsc login`")
    print("  3. Verify with `nsc workspace describe`")
    print("  4. Configure a Namespace runner selector/profile for the workflow you want to route")


def cmd_cloud_namespace_doctor(_args: argparse.Namespace) -> int:
    version = nsc_version()
    if not version:
        print("Namespace CLI: missing")
        print_namespace_setup_help()
        return 1

    print(f"Namespace CLI: ok ({version})")
    if not nsc_logged_in():
        print("Namespace login: missing")
        print("Run: nsc login")
        return 1

    print("Namespace login: ok")
    workspace = nsc_workspace_info()
    if workspace:
        name = workspace.get("Name")
        if name:
            print(f"Workspace: {name}")
        tenant = workspace.get("Tenant ID")
        if tenant:
            print(f"Tenant ID: {tenant}")
        registry = workspace.get("Registry URL")
        if registry:
            print(f"Registry URL: {registry}")
    else:
        print("Workspace: unavailable")
    return 0


def cmd_cloud_namespace_setup(_args: argparse.Namespace) -> int:
    if not nsc_available():
        print("Namespace CLI: missing")
        print_namespace_setup_help()
        return 1

    if not nsc_logged_in():
        print("Namespace login: starting `nsc login`...")
        login_result = nsc_run(["login"], capture_output=False)
        if not login_result or login_result.returncode != 0:
            print("Namespace login: failed")
            return 1

    return cmd_cloud_namespace_doctor(argparse.Namespace())


def gh_repo_variables(repository: str) -> dict[str, str]:
    result = subprocess.run(
        ["gh", "variable", "list", "--repo", repository, "--json", "name,value"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        return {}
    try:
        payload = json.loads(result.stdout)
    except json.JSONDecodeError:
        return {}
    variables: dict[str, str] = {}
    for item in payload:
        name = item.get("name")
        value = item.get("value")
        if not name or value in (None, ""):
            continue
        variables[str(name)] = str(value)
    return variables


def gh_repo_name() -> str | None:
    result = subprocess.run(
        ["gh", "repo", "view", "--json", "nameWithOwner"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        return None
    try:
        return json.loads(result.stdout).get("nameWithOwner")
    except json.JSONDecodeError:
        return None


def gh_current_login() -> str | None:
    result = subprocess.run(
        ["gh", "api", "user", "--jq", ".login"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        return None
    login = result.stdout.strip()
    return login or None


def resolve_github_repository(settings: dict) -> str:
    repository = settings.get("repository", "").strip()
    if repository:
        return repository
    discovered = gh_repo_name()
    if discovered:
        return discovered
    raise ValueError(
        "Could not determine GitHub repository. Set github_actions.repository in tools/local-ci/config.json "
        "or make sure `gh repo view` works in this checkout."
    )


def gh_workflow_dispatch(repository: str, workflow_file: str, ref: str, fields: dict[str, str]) -> None:
    cmd = ["gh", "workflow", "run", workflow_file, "--repo", repository, "--ref", ref]
    for key, value in fields.items():
        cmd += ["-f", f"{key}={value}"]
    result = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if result.returncode != 0:
        detail = (result.stderr or result.stdout or "").strip()
        raise RuntimeError(f"Failed to dispatch {workflow_file}: {detail or 'gh workflow run failed'}")


def gh_find_dispatched_run(
    repository: str,
    workflow_file: str,
    ref: str,
    dispatched_at: str,
    *,
    timeout_secs: int,
) -> dict | None:
    deadline = time.time() + timeout_secs
    dispatched_dt = parse_iso_datetime(dispatched_at)
    tolerance_secs = 10
    fields = (
        "databaseId,headBranch,headSha,status,conclusion,url,createdAt,updatedAt,workflowName,event"
    )

    while time.time() < deadline:
        result = subprocess.run(
            [
                "gh",
                "run",
                "list",
                "--repo",
                repository,
                "--workflow",
                workflow_file,
                "--branch",
                ref,
                "--event",
                "workflow_dispatch",
                "--json",
                fields,
                "--limit",
                "10",
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        if result.returncode == 0:
            try:
                runs = json.loads(result.stdout)
            except json.JSONDecodeError:
                runs = []
            candidates = []
            for run in runs:
                if run.get("headBranch") != ref or run.get("event") != "workflow_dispatch":
                    continue
                created_dt = parse_iso_datetime(run.get("createdAt"))
                if dispatched_dt and created_dt and created_dt.timestamp() + tolerance_secs < dispatched_dt.timestamp():
                    continue
                candidates.append(run)
            if candidates:
                candidates.sort(key=lambda run: run.get("createdAt", ""), reverse=True)
                matched = dict(candidates[0])
                matched["match_ambiguous"] = len(candidates) > 1
                return matched
        time.sleep(2)

    return None


def gh_run_view(repository: str, run_id: int) -> dict | None:
    result = subprocess.run(
        [
            "gh",
            "run",
            "view",
            str(run_id),
            "--repo",
            repository,
            "--json",
            "databaseId,status,conclusion,url,headSha,headBranch,workflowName,createdAt,updatedAt,jobs",
        ],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        return None
    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError:
        return None


def gh_pr_create(branch: str, base: str = "main") -> int | None:
    result = subprocess.run(
        ["gh", "pr", "create", "--head", branch, "--base", base, "--fill", "--no-maintainer-edit"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        existing = subprocess.run(
            ["gh", "pr", "view", branch, "--json", "number"],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )
        if existing.returncode == 0:
            return json.loads(existing.stdout)["number"]
        print(f"  Failed to create PR: {result.stderr.strip()}")
        return None

    url = result.stdout.strip()
    try:
        return int(url.rstrip("/").split("/")[-1])
    except (ValueError, IndexError):
        return None


def gh_pr_comment(pr_number: int, body: str) -> bool:
    result = subprocess.run(
        ["gh", "pr", "comment", str(pr_number), "--body", body],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    return result.returncode == 0


def gh_pr_merge(pr_number: int, method: str = "squash") -> bool:
    result = subprocess.run(
        ["gh", "pr", "merge", str(pr_number), f"--{method}", "--delete-branch"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    return result.returncode == 0


def gh_pr_list_open() -> list[dict]:
    result = subprocess.run(
        ["gh", "pr", "list", "--json", "number,title,headRefName,author,createdAt,labels"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        return []
    return json.loads(result.stdout)


def gh_pr_head(pr_ref: str) -> tuple[int, str, str] | None:
    if pr_ref == "latest":
        prs = gh_pr_list_open()
        if not prs:
            print("No open PRs found.")
            return None
        pr_ref = str(prs[0]["number"])

    result = subprocess.run(
        ["gh", "pr", "view", pr_ref, "--json", "number,headRefName,headRefOid"],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        print(f"  Could not find PR {pr_ref}: {result.stderr.strip()}")
        return None

    data = json.loads(result.stdout)
    return data["number"], data["headRefName"], data["headRefOid"]


def format_ci_comment(result: dict) -> str:
    result = normalize_result(result)
    validation = result.get("validation", "full")
    title = "Local CI Smoke Results" if validation == "smoke" else "Local CI Results"
    lines = [f"## {title}\n"]
    overall = result["overall"].upper()
    icon = "white_check_mark" if overall == "PASS" else "x"
    lines.append(f":{icon}: **Overall: {overall}**\n")
    lines.append(f"Job: `{result.get('job_id', '?')}`  Commit: `{short_sha(result.get('sha', ''))}`\n")
    lines.append(f"Execution: `{provenance_summary(result.get('provenance'))}`\n")
    if result.get("provenance", {}).get("run_url"):
        lines.append(f"Run URL: {result['provenance']['run_url']}\n")
    if validation != "full":
        lines.append(f"Validation: `{validation}`\n")
        lines.append("_Smoke mode is a fast clean install/export preflight and does not run the full test suite._\n")
    lines.append("| Target | Status | Duration |")
    lines.append("|--------|--------|----------|")
    for item in result["results"]:
        status = item["status"].upper()
        s_icon = "white_check_mark" if status == "PASS" else "x"
        lines.append(f"| {item['target']} | :{s_icon}: {status} | {item.get('duration_secs', 0)}s |")

    if any(item["status"] != "pass" for item in result["results"]):
        lines.append("\n<details><summary>Failure details</summary>\n")
        for item in result["results"]:
            if item["status"] == "pass":
                continue
            lines.append(f"### {item['target']} (exit {item.get('exit_code', '?')})")
            stderr = item.get("stderr_tail", "")
            if stderr:
                lines.append(f"```\n{stderr[-500:]}\n```")
        lines.append("</details>")

    lines.append(f"\n*Run at {result.get('completed_at', 'unknown')}*")
    return "\n".join(lines)


# ── CLI Commands ─────────────────────────────────────────────────────────────


def update_cloud_record_from_run(record: dict, snapshot: dict, *, provider_resolved: str | None = None) -> dict:
    updated = normalize_cloud_record(record)
    snapshot_updated_at = snapshot.get("updatedAt") or now_iso()
    current_updated_at = updated.get("updated_at") or ""
    if current_updated_at and snapshot_updated_at and current_updated_at > snapshot_updated_at:
        return updated

    updated["run_id"] = snapshot.get("databaseId", updated.get("run_id"))
    updated["workflow_name"] = snapshot.get("workflowName", updated.get("workflow_name"))
    updated["head_branch"] = snapshot.get("headBranch", updated.get("head_branch"))
    updated["head_sha"] = snapshot.get("headSha", updated.get("head_sha"))
    updated["status"] = snapshot.get("status", updated.get("status"))
    updated["conclusion"] = snapshot.get("conclusion") or updated.get("conclusion", "")
    updated["url"] = snapshot.get("url", updated.get("url"))
    updated["updated_at"] = snapshot_updated_at
    if provider_resolved:
        updated["provider_resolved"] = provider_resolved
    if snapshot.get("createdAt") and not updated.get("matched_at"):
        updated["matched_at"] = snapshot["createdAt"]

    timing = summarize_cloud_timing(snapshot)
    if timing.get("started_at"):
        updated["started_at"] = timing["started_at"]
    if timing.get("completed_at"):
        updated["completed_at"] = timing["completed_at"]
    elif updated.get("status") == "completed" and not updated.get("completed_at"):
        updated["completed_at"] = snapshot_updated_at
    elif updated.get("status") != "completed":
        updated["completed_at"] = ""
    updated["queue_delay_secs"] = timing.get("queue_delay_secs")
    updated["duration_secs"] = timing.get("duration_secs")

    jobs = []
    for job in snapshot.get("jobs", []) or []:
        jobs.append(
            {
                "name": job.get("name", ""),
                "status": job.get("status", ""),
                "conclusion": job.get("conclusion", ""),
                "started_at": normalize_github_timestamp(job.get("startedAt", "")),
                "completed_at": normalize_github_timestamp(job.get("completedAt", "")),
            }
        )
    if jobs:
        updated["jobs"] = jobs
    return updated


def refresh_cloud_record(record: dict, repository: str, *, require_snapshot: bool = False) -> dict:
    run_id = record.get("run_id")
    if not run_id:
        return normalize_cloud_record(record)
    snapshot = gh_run_view(repository, int(run_id))
    if not snapshot:
        if require_snapshot:
            raise RuntimeError(f"Failed to refresh GitHub run {run_id} from {repository}.")
        return normalize_cloud_record(record)
    refreshed = enrich_cloud_record_provider_metadata(
        update_cloud_record_from_run(record, snapshot)
    )
    save_cloud_record(refreshed)
    return refreshed


def filter_cloud_records(
    records: list[dict],
    *,
    workflow_key: str | None = None,
    provider: str | None = None,
) -> list[dict]:
    filtered = []
    for raw_record in records:
        record = normalize_cloud_record(raw_record)
        if workflow_key and record.get("workflow_key") != workflow_key:
            continue
        resolved_provider = record.get("provider_resolved") or record.get("provider_requested") or "github-hosted"
        if provider and resolved_provider != provider:
            continue
        filtered.append(record)
    return filtered


def median_or_none(values: list[float], *, digits: int = 1) -> float | None:
    cleaned = [float(value) for value in values if value not in (None, "")]
    if not cleaned:
        return None
    return round(float(statistics.median(cleaned)), digits)


def compare_cloud_providers(
    records: list[dict],
    config: dict | None,
    *,
    workflow_key: str,
) -> list[dict]:
    grouped: dict[str, dict] = {}
    for record in filter_cloud_records(records, workflow_key=workflow_key):
        if record.get("status") != "completed":
            continue
        provider = record.get("provider_resolved") or record.get("provider_requested") or "github-hosted"
        group = grouped.setdefault(
            provider,
            {
                "provider": provider,
                "workflow_key": workflow_key,
                "runs": [],
                "durations": [],
                "queue_delays": [],
                "provider_runtimes": [],
                "estimated_costs": [],
                "success_count": 0,
                "completed_count": 0,
                "latest_completed_at": "",
                "latest_success_at": "",
            },
        )
        group["runs"].append(record)
        group["completed_count"] += 1
        completed_at = record.get("completed_at") or record.get("updated_at") or ""
        if completed_at and completed_at > group["latest_completed_at"]:
            group["latest_completed_at"] = completed_at
        if record.get("conclusion") == "success":
            group["success_count"] += 1
            if completed_at and completed_at > group["latest_success_at"]:
                group["latest_success_at"] = completed_at
        if record.get("duration_secs") not in (None, ""):
            group["durations"].append(float(record["duration_secs"]))
        if record.get("queue_delay_secs") not in (None, ""):
            group["queue_delays"].append(float(record["queue_delay_secs"]))
        provider_runtime = (record.get("usage_summary") or {}).get("provider_runtime_secs")
        if provider_runtime not in (None, ""):
            group["provider_runtimes"].append(float(provider_runtime))
        cost = estimate_cloud_record_cost(record, config)
        if cost.get("status") == "estimated":
            group["estimated_costs"].append(float(cost.get("estimated_total") or 0.0))

    summaries = []
    for provider, group in grouped.items():
        period = estimate_billing_period_totals(records, config, provider=provider)
        summaries.append(
            {
                "provider": provider,
                "workflow_key": workflow_key,
                "runs_count": len(group["runs"]),
                "completed_count": group["completed_count"],
                "success_count": group["success_count"],
                "median_duration_secs": median_or_none(group["durations"]),
                "median_queue_delay_secs": median_or_none(group["queue_delays"]),
                "median_provider_runtime_secs": median_or_none(group["provider_runtimes"]),
                "median_estimated_cost": median_or_none(group["estimated_costs"], digits=4),
                "currency": resolve_billing_settings(config).get("currency", "USD"),
                "period": period,
                "latest_completed_at": group["latest_completed_at"],
                "latest_success_at": group["latest_success_at"],
            }
        )

    summaries.sort(key=lambda item: item["provider"])
    return summaries


def recommend_cloud_provider(
    records: list[dict],
    config: dict | None,
    *,
    workflow_key: str,
) -> tuple[str | None, str]:
    summaries = compare_cloud_providers(records, config, workflow_key=workflow_key)
    viable = [item for item in summaries if item.get("success_count")]
    if not viable:
        return None, "no successful runs recorded yet"
    if len(viable) == 1:
        return viable[0]["provider"], "only measured provider"

    viable_with_duration = [item for item in viable if item.get("median_duration_secs") is not None]
    if not viable_with_duration:
        return viable[0]["provider"], "no timing medians available"

    fastest = min(viable_with_duration, key=lambda item: float(item["median_duration_secs"]))
    cheapest_candidates = [item for item in viable if item.get("median_estimated_cost") is not None]
    if len(cheapest_candidates) >= 2:
        cheapest = min(cheapest_candidates, key=lambda item: float(item["median_estimated_cost"]))
        fastest_duration = float(fastest["median_duration_secs"] or 0.0)
        cheapest_duration = float(cheapest.get("median_duration_secs") or fastest_duration or 0.0)
        fastest_cost = float(fastest.get("median_estimated_cost") or 0.0)
        cheapest_cost = float(cheapest.get("median_estimated_cost") or 0.0)
        speedup = 0.0
        if cheapest_duration > 0:
            speedup = max(0.0, (cheapest_duration - fastest_duration) / cheapest_duration)
        if cheapest["provider"] != fastest["provider"] and fastest_cost > 0 and cheapest_cost > 0:
            if fastest_cost > cheapest_cost * 1.2 and speedup < 0.15:
                return cheapest["provider"], "lower estimated cost with similar timing"
        return fastest["provider"], "fastest observed median"

    return fastest["provider"], "fastest observed median"


def cmd_cloud_history(args: argparse.Namespace) -> int:
    config = load_optional_config()
    records = filter_cloud_records(
        list_cloud_records(limit=None),
        workflow_key=getattr(args, "workflow", None),
        provider=getattr(args, "provider", None),
    )
    if not records:
        print("No tracked cloud runs found.")
        return 0

    limit = max(1, int(getattr(args, "limit", 10)))
    print("Cloud history:\n")
    for record in records[:limit]:
        print(f"  {cloud_record_summary(record, config)}")

    print()
    print_billing_period_summary(estimate_billing_period_totals(records, config))
    if getattr(args, "provider", None) in (None, "github-hosted"):
        try:
            repository = resolve_github_repository(resolve_github_actions_settings(config))
        except ValueError as exc:
            print_github_repo_billing_summary({"status": "unavailable", "reason": str(exc)})
        else:
            print_github_repo_billing_summary(fetch_github_repo_actions_billing_summary(repository, config))
    return 0


def cmd_cloud_compare(args: argparse.Namespace) -> int:
    config = load_optional_config()
    workflow_key = args.workflow or resolve_github_actions_settings(config).get("workflow", "build")
    summaries = compare_cloud_providers(list_cloud_records(limit=None), config, workflow_key=workflow_key)
    if not summaries:
        print(f"No tracked cloud runs found for workflow '{workflow_key}'.")
        return 0

    print(f"Cloud compare: workflow={workflow_key}\n")
    for summary in summaries:
        line = (
            f"  {summary['provider']}: runs={summary['runs_count']} "
            f"success={summary['success_count']}/{summary['completed_count'] or summary['runs_count']}"
        )
        duration = format_duration_secs(summary.get("median_duration_secs"))
        if duration:
            line += f" median_elapsed={duration}"
        queue_delay = format_duration_secs(summary.get("median_queue_delay_secs"))
        if queue_delay:
            line += f" median_queue={queue_delay}"
        provider_runtime = format_duration_secs(summary.get("median_provider_runtime_secs"))
        if provider_runtime:
            line += f" median_provider_time={provider_runtime}"
        if summary.get("median_estimated_cost") is not None:
            amount = format_currency_amount(summary.get("median_estimated_cost"), summary.get("currency", "USD"))
            if amount:
                line += f" median_cost=est {amount}"
        latest_success = summary.get("latest_success_at") or ""
        latest_completed = summary.get("latest_completed_at") or ""
        if latest_success:
            line += f" latest_success={latest_success}"
        elif latest_completed:
            line += f" latest={latest_completed}"
        print(line)
        print_billing_period_summary(summary.get("period") or {}, indent="    ")
    print("\n  note: estimated; verify provider pricing")
    return 0


def cmd_cloud_recommend(args: argparse.Namespace) -> int:
    config = load_optional_config()
    workflow_key = args.workflow or resolve_github_actions_settings(config).get("workflow", "build")
    provider, reason = recommend_cloud_provider(list_cloud_records(limit=None), config, workflow_key=workflow_key)
    if not provider:
        print(f"No recommendation for workflow '{workflow_key}': {reason}.")
        return 0
    print(f"Recommended provider for {workflow_key}: {provider} ({reason})")
    print(f"  note: {billing_note_text()}")
    return 0


def cmd_cloud_workflows(_args: argparse.Namespace) -> int:
    print("GitHub Actions workflows:\n")
    for key, info in BUILTIN_GITHUB_WORKFLOWS.items():
        providers = ", ".join(info.get("providers", [])) or "github-hosted"
        print(f"  {key:12s} {info['display_name']} ({info['file']})")
        print(f"               providers: {providers}")
    return 0


def cmd_cloud_defaults(_args: argparse.Namespace) -> int:
    config = load_optional_config()
    settings = github_actions_settings_for_display(config)
    repository = ""
    repository_note = ""
    repository_variables: dict[str, str] = {}
    try:
        resolved_settings = resolve_github_actions_settings(config)
        settings = resolved_settings
        repository = resolve_github_repository(resolved_settings)
    except ValueError as exc:
        repository_note = str(exc)
        try:
            repository = resolve_github_repository(settings)
        except ValueError:
            repository = ""
    else:
        if gh_available():
            repository_variables = gh_repo_variables(repository)
        else:
            repository_note = "gh CLI unavailable; repo-variable fallbacks not inspected"

    print("Cloud defaults:\n")
    if repository:
        print(f"  repository: {repository}")
    else:
        print("  repository: unresolved")
    if repository_note:
        print(f"  note: {repository_note}")
    print(f"  configured default workflow: {settings.get('workflow', 'build')}")
    print(f"  configured default provider: {settings.get('provider', 'github-hosted')}")
    billing = resolve_billing_settings(config)
    print(
        f"  billing estimates: {billing.get('currency', 'USD')} period-day={billing.get('billing_period_start_day', 1)} "
        f"({billing_note_text()})"
    )
    provider_truth_state = "enabled (local opt-in)" if billing.get("enable_provider_reported_totals") else "disabled (opt-in; off by default)"
    print(f"  provider billing truth: {provider_truth_state}")

    for workflow_key, workflow in BUILTIN_GITHUB_WORKFLOWS.items():
        summary = summarize_workflow_provider_defaults(
            config,
            repository_variables,
            settings,
            workflow_key,
        )
        print(f"\n  {workflow_key}: {workflow['display_name']} ({workflow['file']})")
        print(f"    supported providers: {', '.join(workflow.get('providers', ['github-hosted']))}")
        print(
            f"    default provider: {summary['provider']} ({summary['provider_source']})"
        )
        selector_input = summary.get("selector_input") or ""
        if selector_input:
            print_cloud_field_detail(
                selector_input,
                summary.get("selector_value", ""),
                summary.get("selector_source", ""),
            )
        for field_name in workflow.get("dispatch_fields") or []:
            unset_note = ""
            if workflow_key == "build" and field_name == "macos_runner_selector_json":
                unset_note = "macOS stays local-first unless a config default or one-off override is set"
            print_cloud_field_detail(
                field_name,
                (summary.get("dispatch_fields") or {}).get(field_name, ""),
                (summary.get("dispatch_sources") or {}).get(field_name, ""),
                unset_note=unset_note,
            )
    return 0


def cmd_cloud_run(args: argparse.Namespace) -> int:
    if not gh_available():
        print("Error: gh CLI not available or not authenticated. Run: gh auth login")
        return 1

    config = load_optional_config()
    try:
        settings = resolve_github_actions_settings(config)
        repository = resolve_github_repository(settings)
    except ValueError as exc:
        print(f"Error: {exc}")
        return 1

    workflow_key = args.workflow or settings.get("workflow", "build")
    workflow = BUILTIN_GITHUB_WORKFLOWS.get(workflow_key)
    if workflow is None:
        print(
            f"Error: Unknown workflow '{workflow_key}'. Use `pulp ci-local cloud workflows` to list supported workflows."
        )
        return 1

    branch = args.branch or current_branch()
    try:
        provider, _provider_source = resolve_default_provider_for_workflow(
            settings,
            workflow_key,
            explicit_provider=getattr(args, "provider", None),
        )
        repository_variables = gh_repo_variables(repository)
        config_dispatch_fields, _config_dispatch_sources = resolve_workflow_dispatch_defaults(
            config,
            repository_variables,
            workflow_key,
            provider,
            workflow.get("dispatch_fields"),
        )
        cli_dispatch_fields = resolve_cli_dispatch_field_values(
            args, workflow.get("dispatch_fields")
        )
        selector_input = workflow.get("selector_input")
        if getattr(args, "runner_selector_json", None):
            selector_json = normalize_runs_on_json(
                args.runner_selector_json,
                setting_name="--runner-selector-json",
            )
        elif selector_input:
            selector_json, _selector_source = resolve_workflow_field_value_and_source(
                config,
                repository_variables,
                workflow_key,
                provider,
                selector_input,
            )
        else:
            selector_json = ""
        config_dispatch_fields.update(cli_dispatch_fields)
    except ValueError as exc:
        print(f"Error: {exc}")
        return 1

    selector_input = workflow.get("selector_input")
    if selector_json and not selector_input:
        print(f"Error: workflow '{workflow_key}' does not accept an explicit runner selector.")
        return 1

    dispatch_id = uuid.uuid4().hex[:12]
    dispatch_time = now_iso()
    record = normalize_cloud_record(
        {
            "dispatch_id": dispatch_id,
            "repository": repository,
            "workflow_key": workflow_key,
            "workflow_file": workflow["file"],
            "workflow_name": workflow["display_name"],
            "requested_ref": branch,
            "requested_by": gh_current_login() or "",
            "provider_requested": provider,
            "runner_selector_json": selector_json,
            "dispatch_fields": config_dispatch_fields,
            "status": "unresolved",
            "dispatched_at": dispatch_time,
            "updated_at": dispatch_time,
            "match_strategy": "workflow+branch+created_at",
        }
    )
    save_cloud_record(record)

    fields: dict[str, str] = {}
    provider_input = workflow.get("provider_input")
    if provider_input:
        fields[provider_input] = provider
    fields.update(config_dispatch_fields)
    if selector_input and selector_json:
        fields[selector_input] = selector_json

    try:
        gh_workflow_dispatch(repository, workflow["file"], branch, fields)
    except RuntimeError as exc:
        print(f"Error: {exc}")
        return 1

    matched = gh_find_dispatched_run(
        repository,
        workflow["file"],
        branch,
        dispatch_time,
        timeout_secs=int(settings["match_timeout_secs"]),
    )

    if matched:
        record = enrich_cloud_record_provider_metadata(
            update_cloud_record_from_run(record, matched, provider_resolved=provider)
        )
        record["match_ambiguous"] = bool(matched.get("match_ambiguous"))
        save_cloud_record(record)

    print(f"Dispatched: {workflow_key} ref={branch} provider={provider}")
    print(f"  dispatch id: {dispatch_id}")
    if record.get("run_id"):
        print(f"  GitHub run: {record['run_id']}")
        if record.get("url"):
            print(f"  URL: {record['url']}")
    else:
        print("  warning: dispatched workflow could not be matched to a GitHub run yet")

    if not args.wait:
        return 0

    if not record.get("run_id"):
        print("Error: blocking wait requested, but the dispatched GitHub run could not be matched.")
        return 1

    while record.get("status") != "completed":
        time.sleep(int(settings["wait_poll_secs"]))
        try:
            record = refresh_cloud_record(record, repository, require_snapshot=True)
        except RuntimeError as exc:
            print(f"Error: {exc}")
            return 1

    print(f"  final: {record.get('status', '?')}/{(record.get('conclusion') or 'unknown').upper()}")
    return 0 if record.get("conclusion") == "success" else 1


def cmd_cloud_status(args: argparse.Namespace) -> int:
    config = load_optional_config()
    if args.identifier is None:
        records = list_cloud_records(limit=args.limit)
        if not records:
            print("No tracked cloud runs yet.")
            return 0
        print("Recent cloud runs:\n")
        for item in records:
            print(f"  {cloud_record_summary(item, config)}")
        print()
        print_billing_period_summary(estimate_billing_period_totals(list_cloud_records(limit=None), config))
        return 0

    try:
        record = find_cloud_record(list_cloud_records(), args.identifier)
    except ValueError as exc:
        print(f"Error: {exc}")
        return 1

    if record is None:
        print("No matching cloud runs found.")
        return 1

    if args.refresh:
        if not gh_available():
            print("Error: gh CLI not available or not authenticated. Run: gh auth login")
            return 1
        try:
            repository = record.get("repository") or resolve_github_repository(
                resolve_github_actions_settings(load_optional_config())
            )
        except ValueError as exc:
            print(f"Error: {exc}")
            return 1
        try:
            record = refresh_cloud_record(record, repository, require_snapshot=True)
        except RuntimeError as exc:
            print(f"Error: {exc}")
            return 1

    rendered_record = normalize_cloud_record(record)
    rendered_record["cost_summary"] = estimate_cloud_record_cost(rendered_record, config)
    print(cloud_record_summary(rendered_record, config))
    print(f"  workflow: {record.get('workflow_name')} ({record.get('workflow_file')})")
    print(f"  repo: {record.get('repository')}")
    print(f"  requested ref: {record.get('requested_ref')}")
    selector = summarize_runner_selector(record.get("runner_selector_json", ""))
    if selector:
        print(f"  runner selector: {selector}")
    dispatch_fields = record.get("dispatch_fields") or {}
    if isinstance(dispatch_fields, dict):
        for key in sorted(dispatch_fields):
            value = dispatch_fields.get(key)
            if not value:
                continue
            rendered = (
                summarize_runner_selector(value)
                if key.endswith("_selector_json")
                else str(value)
            )
            print(f"  {key}: {rendered}")
    if record.get("head_sha"):
        print(f"  sha: {short_sha(record['head_sha'])}")
    if record.get("url"):
        print(f"  url: {record['url']}")
    if record.get("matched_at"):
        print(f"  matched: {record['matched_at']}")
    if record.get("started_at"):
        print(f"  started: {record['started_at']}")
    if record.get("queue_delay_secs") is not None:
        print(f"  queue delay: {format_duration_secs(record.get('queue_delay_secs'))}")
    if record.get("duration_secs") is not None:
        print(f"  elapsed: {format_duration_secs(record.get('duration_secs'))}")
    if record.get("updated_at"):
        print(f"  updated: {record['updated_at']}")
    if record.get("completed_at"):
        print(f"  completed: {record['completed_at']}")
    print_namespace_usage_summary(rendered_record)
    print_billing_period_summary(estimate_billing_period_totals(list_cloud_records(limit=None), config))
    if record.get("jobs"):
        print("  jobs:")
        for job in record["jobs"]:
            status = job.get("status", "?")
            conclusion = job.get("conclusion", "")
            suffix = f"/{conclusion}" if conclusion else ""
            job_duration = format_duration_secs(
                duration_between(job.get("started_at"), job.get("completed_at"))
            )
            detail = f" duration={job_duration}" if job_duration else ""
            print(f"    {job.get('name', '?')}: {status}{suffix}{detail}")
    return 0


def print_local_ci_state_footprint(*, indent: str = "") -> None:
    footprint = local_ci_state_footprint()
    print(f"{indent}Local CI footprint: total={format_size_bytes(footprint.get('total_bytes', 0))}")
    for label in ("bundles", "prepared", "logs", "results", "cloud-runs"):
        entry = (footprint.get("entries") or {}).get(label) or {}
        print(
            f"{indent}  {label}: {format_size_bytes(entry.get('size_bytes', 0))} "
            f"({describe_path_for_cleanup(entry.get('path', state_dir()))})"
        )


def print_local_ci_cleanup_plan(plan: dict, *, dry_run: bool) -> None:
    print("Local CI cleanup:\n")
    print(
        f"  reclaimable: {format_size_bytes(plan.get('total_bytes', 0))} "
        f"across {plan.get('total_paths', 0)} path(s)"
    )
    for category in ("bundles", "logs", "results", "prepared"):
        entries = (plan.get("categories") or {}).get(category) or []
        if not entries:
            continue
        category_bytes = sum(int(entry.get("size_bytes", 0)) for entry in entries)
        print(f"\n  {category}: {format_size_bytes(category_bytes)} across {len(entries)} path(s)")
        for entry in entries[:10]:
            print(f"    {describe_path_for_cleanup(Path(entry['path']))} ({format_size_bytes(entry.get('size_bytes', 0))})")
        if len(entries) > 10:
            print(f"    ... {len(entries) - 10} more")

    if dry_run:
        print("\n  dry run only; re-run with --apply to delete these paths")
    else:
        print("\n  applying cleanup now")


def cmd_cleanup(args: argparse.Namespace) -> int:
    queue = load_queue()
    running = [job for job in queue if job.get("status") == "running"]
    if args.apply and running:
        print("Error: cleanup --apply is blocked while local CI jobs are running.")
        return 1

    plan = collect_local_ci_cleanup_plan(
        queue,
        keep_results=args.keep_results,
        keep_logs=args.keep_logs,
        keep_bundles=args.keep_bundles,
        include_prepared=args.include_prepared,
    )
    print_local_ci_cleanup_plan(plan, dry_run=not args.apply)

    if not args.apply:
        print_local_ci_state_footprint(indent="  ")
        if args.include_prepared:
            print("  note: prepared cleanup removes cached build/install state and later reruns will rebuild it.")
        return 0

    result = apply_local_ci_cleanup_plan(plan)
    print(
        f"\n  removed: {len(result.get('removed', []))} path(s), "
        f"{format_size_bytes(result.get('removed_bytes', 0))}"
    )
    if result.get("failed"):
        print(f"  failed: {len(result['failed'])} path(s)")
        for failure in result["failed"][:10]:
            print(f"    {describe_path_for_cleanup(Path(failure['path']))}: {failure['error']}")
        return 1
    print_local_ci_state_footprint(indent="  ")
    if args.include_prepared:
        print("  note: prepared cleanup removes cached build/install state and later reruns will rebuild it.")
    return 0


def resolve_submission_options(
    args: argparse.Namespace, command: str
) -> tuple[dict, str, str, list[str], str, str, dict]:
    config = load_config()
    branch = args.branch or current_branch()
    if args.sha:
        sha = args.sha
    elif args.branch:
        sha = resolve_git_ref_sha(branch)
    else:
        sha = current_sha()
    targets = resolve_targets(config, parse_targets_arg(getattr(args, "targets", None)))
    priority = normalize_priority(getattr(args, "priority", None) or default_priority_for(command, config))
    validation = normalize_validation_mode("smoke" if getattr(args, "smoke", False) else "full")
    submission = build_submission_metadata(
        config,
        branch,
        sha,
        targets,
        priority,
        validation,
        allow_root_mismatch=bool(getattr(args, "allow_root_mismatch", False)),
        allow_unreachable_targets=bool(getattr(args, "allow_unreachable_targets", False)),
    )
    return config, branch, sha, targets, priority, validation, submission


def cmd_enqueue(args: argparse.Namespace) -> int:
    try:
        _config, branch, sha, targets, priority, validation, submission = resolve_submission_options(args, "enqueue")
    except (FileNotFoundError, ValueError) as exc:
        print(f"Error: {exc}")
        return 1

    print_submission_metadata(submission)
    job, created = enqueue_job(branch, sha, priority, targets, "enqueue", validation, submission=submission)
    if created:
        print(f"Enqueued: {summarize_job(job)}")
    else:
        print(f"Already queued/running: {summarize_job(job)}")
    return 0


def cmd_drain(_args: argparse.Namespace) -> int:
    try:
        config = load_config()
    except FileNotFoundError as exc:
        print(f"Error: {exc}")
        return 1

    acquired, any_failure = drain_pending_jobs(config, blocking=False)
    if not acquired:
        runner = current_runner_info()
        if runner and runner.get("active_job_id"):
            print(
                f"Another local CI runner is active [{runner['active_job_id']}] {runner.get('active_branch', '?')}."
            )
        else:
            print("Another local CI runner is active.")
        return 0

    notify("CI complete" + (" - PASSED" if not any_failure else " - FAILED"))
    return 1 if any_failure else 0


def cmd_run(args: argparse.Namespace) -> int:
    try:
        config, branch, sha, targets, priority, validation, submission = resolve_submission_options(args, "run")
    except (FileNotFoundError, ValueError) as exc:
        print(f"Error: {exc}")
        return 1

    print_submission_metadata(submission)
    job, created = enqueue_job(branch, sha, priority, targets, "run", validation, submission=submission)
    print(("Enqueued" if created else "Already queued/running") + f": {summarize_job(job)}")

    result, exit_code = wait_for_job(job["id"], config)
    if result is not None:
        print_result(result, Path(load_job(job["id"])["result_file"]))
    notify("CI run complete" + (" - PASSED" if exit_code == 0 else " - FAILED"))
    return exit_code


def cmd_ship(args: argparse.Namespace) -> int:
    try:
        config, branch, sha, targets, priority, validation, submission = resolve_submission_options(args, "ship")
    except (FileNotFoundError, ValueError) as exc:
        print(f"Error: {exc}")
        return 1
    if validation != "full":
        print("Error: ship only supports full validation. Use `run --smoke` or `check --smoke` for preflight.")
        return 1

    base = args.base or "main"
    if branch == base:
        print(f"Error: cannot ship {base} to itself. Checkout a feature branch first.")
        return 1

    if not gh_available():
        print("Error: gh CLI not available or not authenticated. Run: gh auth login")
        return 1

    print(f"\n=== Shipping {branch} -> {base} ===\n")
    print_submission_metadata(submission)
    print(f"  Pushing {branch}...")
    push = subprocess.run(
        ["git", "push", "-u", "origin", branch],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if push.returncode != 0:
        print(f"  Push failed: {push.stderr.strip()}")
        return 1

    print("  Creating PR...")
    pr_number = gh_pr_create(branch, base)
    if pr_number is None:
        print("  Failed to create or find PR.")
        return 1
    print(f"  PR #{pr_number} ready")

    job, _created = enqueue_job(branch, sha, priority, targets, "ship", validation, submission=submission)
    print(f"  Queueing CI: {summarize_job(job)}")
    result, exit_code = wait_for_job(job["id"], config)
    if result is None:
        return 1

    gh_pr_comment(pr_number, format_ci_comment(result))
    if result["overall"] == "pass":
        print(f"  All targets passed. Merging PR #{pr_number}...")
        if gh_pr_merge(pr_number):
            print(f"  PR #{pr_number} merged and branch deleted.")
            notify(f"PR #{pr_number} shipped to {base}!")
            return 0
        print(f"  Merge failed. PR #{pr_number} is still open.")
        notify(f"PR #{pr_number} CI passed but merge failed")
        return 1

    print(f"  CI failed. PR #{pr_number} left open for review.")
    notify(f"PR #{pr_number} CI failed")
    return exit_code


def cmd_check(args: argparse.Namespace) -> int:
    if not gh_available():
        print("Error: gh CLI not available. Run: gh auth login")
        return 1

    pr_info = gh_pr_head(args.pr)
    if pr_info is None:
        return 1

    pr_number, branch, sha = pr_info
    print(f"  PR #{pr_number} -> branch: {branch} @ {short_sha(sha)}")

    try:
        config = load_config()
        targets = resolve_targets(config, parse_targets_arg(args.targets))
        priority = normalize_priority(args.priority or default_priority_for("check", config))
        validation = normalize_validation_mode("smoke" if getattr(args, "smoke", False) else "full")
        submission = build_submission_metadata(
            config,
            branch,
            sha,
            targets,
            priority,
            validation,
            allow_root_mismatch=bool(getattr(args, "allow_root_mismatch", False)),
            allow_unreachable_targets=bool(getattr(args, "allow_unreachable_targets", False)),
        )
    except (FileNotFoundError, ValueError) as exc:
        print(f"Error: {exc}")
        return 1

    print_submission_metadata(submission)
    job, _created = enqueue_job(branch, sha, priority, targets, "check", validation, submission=submission)
    print(f"  Queueing CI: {summarize_job(job)}")
    result, exit_code = wait_for_job(job["id"], config)
    if result is None:
        return 1

    gh_pr_comment(pr_number, format_ci_comment(result))
    notify("CI check complete" + (" - PASSED" if exit_code == 0 else " - FAILED"))
    return exit_code


def cmd_bump(args: argparse.Namespace) -> int:
    try:
        requested_priority = normalize_priority(args.priority)
    except ValueError as exc:
        print(f"Error: {exc}")
        return 1

    try:
        with file_lock(queue_lock_path(), blocking=True):
            queue = load_queue_unlocked()
            job = find_job_unlocked(queue, args.job, statuses={"pending", "running"})
            if job is None:
                print(f"No active job matches '{args.job}'.")
                return 1
            if job["status"] != "pending":
                print(f"Job is already {job['status']}; only pending jobs can be reprioritized.")
                return 1
            job["priority"] = requested_priority
            job["bumped_at"] = now_iso()
            save_queue_unlocked(queue)
            print(f"Updated priority: {summarize_job(job)}")
            return 0
    except ValueError as exc:
        print(f"Error: {exc}")
        return 1


def cmd_cancel(args: argparse.Namespace) -> int:
    try:
        with file_lock(queue_lock_path(), blocking=True):
            queue = load_queue_unlocked()
            job = find_job_unlocked(queue, args.job, statuses={"pending", "running"})
            if job is None:
                print(f"No active job matches '{args.job}'.")
                return 1
            if job["status"] != "pending":
                print(f"Job is already {job['status']}; only pending jobs can be canceled safely.")
                return 1
            cancel_job_unlocked(job)
            save_queue_unlocked(trim_completed_jobs(queue))
            print(f"Canceled: {summarize_job(job)}")
            return 0
    except ValueError as exc:
        print(f"Error: {exc}")
        return 1


def cmd_list(_args: argparse.Namespace) -> int:
    if not gh_available():
        print("Error: gh CLI not available. Run: gh auth login")
        return 1

    prs = gh_pr_list_open()
    if not prs:
        print("No open PRs.")
        return 0

    print(f"Open PRs ({len(prs)}):\n")
    for pr in prs:
        author = pr.get("author", {}).get("login", "?")
        labels = ", ".join(label.get("name", "") for label in pr.get("labels", []))
        label_str = f" [{labels}]" if labels else ""
        print(f"  #{pr['number']:4d}  {pr['title']}")
        print(f"         {pr['headRefName']} by {author}{label_str}")
    return 0


def resolve_job_for_logs(job_ref: str | None) -> dict | None:
    queue = load_queue()
    runner = current_runner_info()

    if job_ref:
        return find_job_unlocked(queue, job_ref)

    if runner and runner.get("active_job_id"):
        return find_job_unlocked(queue, runner["active_job_id"])

    completed = [job for job in queue if job.get("status") == "completed"]
    if completed:
        return completed[-1]
    return None


def cmd_logs(args: argparse.Namespace) -> int:
    try:
        job = resolve_job_for_logs(args.job)
    except ValueError as exc:
        print(f"Error: {exc}")
        return 1

    if job is None:
        print("No matching job logs found.")
        return 1

    paths: list[Path]
    if args.target:
        path = target_log_path(job["id"], args.target)
        paths = [path]
    else:
        log_dir = job_logs_dir(job["id"])
        paths = sorted(log_dir.glob("*.log"))

    if not paths:
        print(f"No logs found for job [{job['id']}] {job['branch']}.")
        return 1

    print(f"Logs for [{job['id']}] {job['branch']} @ {short_sha(job.get('sha', ''))}\n")
    for path in paths:
        print(f"== {path.stem} ==")
        lines = tail_lines(path, args.lines)
        if lines:
            print("".join(lines).rstrip())
        else:
            print("(empty)")
        print()
    return 0


def cmd_evidence(args: argparse.Namespace) -> int:
    branch = args.branch or current_branch()
    printed_header = False

    if branch:
        print(f"Evidence for branch `{branch}`:")
        printed_header = True
    elif args.sha:
        print(f"Evidence for sha `{short_sha(args.sha)}`:")
        printed_header = True

    found = print_evidence_summary(branch=branch, sha=args.sha, limit=args.limit)
    if not found:
        if printed_header:
            print("  (none)")
        else:
            print("No local CI evidence recorded.")
        return 1
    return 0


def cmd_status(_args: argparse.Namespace) -> int:
    try:
        config = load_config()
    except FileNotFoundError as exc:
        print(f"Error: {exc}")
        return 1

    queue = load_queue()
    pending = sorted([job for job in queue if job.get("status") == "pending"], key=job_sort_key)
    running = [job for job in queue if job.get("status") == "running"]
    completed = [job for job in queue if job.get("status") == "completed"]
    runner = current_runner_info()

    print(f"State: {state_dir()}")
    print(f"Config: {config_path()}")

    if runner:
        active_job = runner.get("active_job_id") or "?"
        active_branch = runner.get("active_branch") or "?"
        print(f"\nRunner: pid={runner.get('pid', '?')} active=[{active_job}] {active_branch}")
    else:
        print("\nRunner: idle")

    if running:
        print(f"\nRunning ({len(running)}):")
        for job in running:
            print(f"  {summarize_job(job)} started {job.get('started_at', '?')}")
            submission = job.get("submission") or {}
            if submission.get("config_path"):
                print(
                    "    submission: "
                    f"root={submission.get('submitted_root', '?')} "
                    f"config={submission.get('config_path')} "
                    f"({submission.get('config_source', '?')})"
                )
            if submission.get("provenance"):
                print(f"    provenance: {provenance_summary(submission.get('provenance'))}")
            active_targets = job.get("active_targets") or (
                runner.get("active_targets") if runner and runner.get("active_job_id") == job["id"] else None
            )
            target_summary = summarize_active_targets(active_targets, job.get("targets"))
            if target_summary:
                print(f"    live targets: {target_summary}")
            for name in job.get("targets") or []:
                state = (active_targets or {}).get(name)
                if not state:
                    continue
                details = []
                if state.get("phase"):
                    details.append(f"phase={state['phase']}")
                if state.get("validation_mode"):
                    details.append(f"mode={state['validation_mode']}")
                if state.get("transport_mode"):
                    details.append(f"transport={state['transport_mode']}")
                if state.get("test_policy"):
                    details.append(f"tests={state['test_policy']}")
                if state.get("prepared_state"):
                    details.append(f"prepared={state['prepared_state']}")
                if state.get("wait_reason"):
                    details.append(f"wait={state['wait_reason']}")
                if state.get("cleanup_status"):
                    details.append(f"cleanup={state['cleanup_status']}")
                if state.get("last_output_at"):
                    details.append(f"output={state['last_output_at']}")
                if state.get("last_heartbeat_at"):
                    details.append(f"heartbeat={state['last_heartbeat_at']}")
                if state.get("quiet_for_secs") is not None:
                    details.append(f"idle={state['quiet_for_secs']}s")
                if state.get("liveness"):
                    details.append(f"liveness={state['liveness']}")
                if state.get("log_path"):
                    details.append(f"log={Path(state['log_path']).name}")
                if details:
                    print(f"    {name}: " + ", ".join(details))
                if state.get("last_line"):
                    print(f"      {state['last_line']}")
                if state.get("cleanup_result"):
                    print(f"      cleanup: {state['cleanup_result']}")
    else:
        print("\nNo running jobs.")

    if pending:
        print(f"\nPending ({len(pending)}):")
        for job in pending:
            print(f"  {summarize_job(job)} queued {job.get('queued_at', '?')}")
            submission = job.get("submission") or {}
            if submission.get("config_path"):
                print(
                    "    submission: "
                    f"root={submission.get('submitted_root', '?')} "
                    f"config={submission.get('config_path')} "
                    f"({submission.get('config_source', '?')})"
                )
            if submission.get("provenance"):
                print(f"    provenance: {provenance_summary(submission.get('provenance'))}")
            active_targets = job.get("active_targets")
            target_summary = summarize_active_targets(active_targets, job.get("targets"))
            if target_summary:
                progress_at = job.get("last_progress_at") or job.get("requeued_at") or "?"
                print(f"    last known targets: {target_summary} (updated {progress_at})")
            for name in job.get("targets") or []:
                state = (active_targets or {}).get(name)
                if not state:
                    continue
                details = []
                if state.get("phase"):
                    details.append(f"phase={state['phase']}")
                if state.get("validation_mode"):
                    details.append(f"mode={state['validation_mode']}")
                if state.get("transport_mode"):
                    details.append(f"transport={state['transport_mode']}")
                if state.get("test_policy"):
                    details.append(f"tests={state['test_policy']}")
                if state.get("prepared_state"):
                    details.append(f"prepared={state['prepared_state']}")
                if state.get("wait_reason"):
                    details.append(f"wait={state['wait_reason']}")
                if state.get("cleanup_status"):
                    details.append(f"cleanup={state['cleanup_status']}")
                if state.get("last_output_at"):
                    details.append(f"output={state['last_output_at']}")
                if state.get("last_heartbeat_at"):
                    details.append(f"heartbeat={state['last_heartbeat_at']}")
                if state.get("quiet_for_secs") is not None:
                    details.append(f"idle={state['quiet_for_secs']}s")
                if state.get("liveness"):
                    details.append(f"liveness={state['liveness']}")
                if state.get("log_path"):
                    details.append(f"log={Path(state['log_path']).name}")
                if details:
                    print(f"    {name}: " + ", ".join(details))
                if state.get("last_line"):
                    print(f"      {state['last_line']}")
                if state.get("cleanup_result"):
                    print(f"      cleanup: {state['cleanup_result']}")
    else:
        print("\nNo pending jobs.")

    if completed:
        print(f"\nRecent ({min(len(completed), 5)}):")
        for job in completed[-5:]:
            result_file = job.get("result_file")
            if result_file and Path(result_file).exists():
                result = load_result(Path(result_file))
                targets = ", ".join(
                    f"{item['target']}={item['status']}" for item in result.get("results", [])
                )
                print(
                    f"  [{job['id']}] {job['branch']} @ {short_sha(job.get('sha', ''))} "
                    f"{result.get('overall', '?').upper()} [{targets}] "
                    f"via {provenance_summary(result.get('provenance'))}"
                )
            else:
                print(f"  {summarize_job(job)} (result file missing)")

    branch = current_branch()
    if branch:
        print(f"\nEvidence ({branch}):")
        if not print_evidence_summary(branch=branch, limit=2, indent="  "):
            print("  (none)")

    cloud_records = list_cloud_records(limit=5)
    all_cloud_records = list_cloud_records(limit=None)
    cloud_config = load_optional_config()
    cloud_settings_note = ""
    cloud_settings = github_actions_settings_for_display(cloud_config)
    try:
        resolved_cloud_settings = resolve_github_actions_settings(cloud_config)
        cloud_settings = resolved_cloud_settings
    except ValueError as exc:
        cloud_settings_note = str(exc)
    default_workflow_key = cloud_settings.get("workflow", "build")
    try:
        default_provider, _default_provider_source = resolve_default_provider_for_workflow(
            cloud_settings,
            default_workflow_key,
        )
    except ValueError:
        default_provider = cloud_settings.get("provider", "github-hosted")

    print(
        f"\nCloud defaults: workflow={default_workflow_key} provider={default_provider} "
        "(`pulp ci-local cloud defaults` for selectors and sources)"
    )
    if cloud_settings_note:
        print(f"  note: {cloud_settings_note}")

    if cloud_records:
        print_billing_period_summary(estimate_billing_period_totals(all_cloud_records, cloud_config), indent="  ")
        print("\nCloud (latest 5 known to this machine):")
        for record in cloud_records:
            print(f"  {cloud_record_summary(record, cloud_config)}")

    print()
    print_local_ci_state_footprint(indent="  ")

    print("\nVM Status:")
    for vm_name in ["Ubuntu 24.04 desktop", "Windows"]:
        print(f"  {vm_name}: {utmctl_vm_status(vm_name) or 'not found'}")

    for host in [target_cfg.get("host") for target_cfg in config.get("targets", {}).values() if target_cfg.get("type") == "ssh"]:
        if host:
            print(f"  ssh {host}: {'up' if ssh_reachable(host, 3) else 'down'}")

    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Local CI runner for Pulp",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    sub = parser.add_subparsers(dest="command")

    def add_submission_args(
        command_parser: argparse.ArgumentParser,
        *,
        include_sha: bool = False,
        allow_smoke: bool = False,
    ) -> None:
        command_parser.add_argument("branch", nargs="?", help="Branch name (default: current)")
        command_parser.add_argument(
            "--priority",
            choices=sorted(PRIORITY_VALUES),
            help="Queue priority (default from config; ship/check default to high)",
        )
        command_parser.add_argument(
            "--targets",
            help="Comma-separated target list (for example: mac or mac,ubuntu)",
        )
        command_parser.add_argument(
            "--allow-root-mismatch",
            action="store_true",
            help="Queue even if the current cwd belongs to a different git root than this local_ci.py checkout",
        )
        command_parser.add_argument(
            "--allow-unreachable-targets",
            action="store_true",
            help="Queue even if preflight finds a selected SSH target currently unreachable with no fallback",
        )
        if include_sha:
            command_parser.add_argument("--sha", help="Exact commit SHA to validate (default: current HEAD)")
        if allow_smoke:
            command_parser.add_argument(
                "--smoke",
                action="store_true",
                help="Run the fast clean install/export preflight instead of full validation",
            )

    p_enqueue = sub.add_parser("enqueue", help="Queue a branch for validation")
    add_submission_args(p_enqueue, include_sha=True, allow_smoke=True)

    sub.add_parser("drain", help="Process pending jobs if no other runner is active")

    p_run = sub.add_parser("run", help="Queue validation and wait for completion")
    add_submission_args(p_run, include_sha=True, allow_smoke=True)

    p_ship = sub.add_parser("ship", help="PR -> queued CI -> merge on green")
    add_submission_args(p_ship, include_sha=True)
    p_ship.add_argument("--base", default="main", help="Base branch (default: main)")

    p_check = sub.add_parser("check", help="Validate an existing PR")
    p_check.add_argument("pr", help="PR number, GitHub URL, or 'latest'")
    p_check.add_argument(
        "--priority",
        choices=sorted(PRIORITY_VALUES),
        help="Queue priority (default: high)",
    )
    p_check.add_argument(
        "--targets",
        help="Comma-separated target list (for example: mac or mac,ubuntu)",
    )
    p_check.add_argument(
        "--allow-root-mismatch",
        action="store_true",
        help="Queue even if the current cwd belongs to a different git root than this local_ci.py checkout",
    )
    p_check.add_argument(
        "--allow-unreachable-targets",
        action="store_true",
        help="Queue even if preflight finds a selected SSH target currently unreachable with no fallback",
    )
    p_check.add_argument(
        "--smoke",
        action="store_true",
        help="Run the fast clean install/export preflight instead of full validation",
    )

    p_cloud = sub.add_parser("cloud", help="Operate GitHub Actions workflows through the local CI control plane")
    cloud_sub = p_cloud.add_subparsers(dest="cloud_command")

    cloud_sub.add_parser("workflows", help="List supported GitHub workflows and providers")
    cloud_sub.add_parser("defaults", help="Show effective cloud workflow/provider defaults")

    p_cloud_history = cloud_sub.add_parser("history", help="Show recent tracked cloud run history")
    p_cloud_history.add_argument(
        "--workflow",
        help="Optional workflow key filter (for example: build or docs-check)",
    )
    p_cloud_history.add_argument(
        "--provider",
        help="Optional provider filter (for example: github-hosted or namespace)",
    )
    p_cloud_history.add_argument(
        "--limit",
        type=int,
        default=10,
        help="Runs to show (default: 10)",
    )

    p_cloud_compare = cloud_sub.add_parser("compare", help="Compare observed cloud providers for a workflow")
    p_cloud_compare.add_argument(
        "workflow",
        nargs="?",
        help="Workflow key (default: configured cloud default workflow)",
    )

    p_cloud_recommend = cloud_sub.add_parser("recommend", help="Recommend a cloud provider from recorded history")
    p_cloud_recommend.add_argument(
        "workflow",
        nargs="?",
        help="Workflow key (default: configured cloud default workflow)",
    )

    p_cloud_run = cloud_sub.add_parser("run", help="Dispatch a GitHub Actions workflow")
    p_cloud_run.add_argument(
        "workflow",
        nargs="?",
        help="Workflow key (for example: build, validate, docs-check)",
    )
    p_cloud_run.add_argument("branch", nargs="?", help="Branch name (default: current)")
    p_cloud_run.add_argument(
        "--provider",
        help="Runner provider (for example: github-hosted or namespace)",
    )
    p_cloud_run.add_argument(
        "--runner-selector-json",
        help="Optional JSON string/array passed through to the workflow runs-on selector",
    )
    p_cloud_run.add_argument(
        "--linux-runner-selector-json",
        help="Optional JSON string/array override for the Linux build leg runs-on selector",
    )
    p_cloud_run.add_argument(
        "--windows-runner-selector-json",
        help="Optional JSON string/array override for the Windows build leg runs-on selector",
    )
    p_cloud_run.add_argument(
        "--macos-runner-selector-json",
        help="Optional JSON string/array override for the macOS build leg runs-on selector",
    )
    p_cloud_run.add_argument(
        "--wait",
        action="store_true",
        help="Block until the matched GitHub run completes",
    )

    p_cloud_status = cloud_sub.add_parser("status", help="Show tracked GitHub workflow state")
    p_cloud_status.add_argument(
        "identifier",
        nargs="?",
        help="Dispatch id, GitHub run id, or 'latest' (default: list recent tracked runs)",
    )
    p_cloud_status.add_argument(
        "--refresh",
        action="store_true",
        help="Refresh the selected matched run from GitHub before rendering",
    )
    p_cloud_status.add_argument(
        "--limit",
        type=int,
        default=5,
        help="Runs to show when listing recent tracked runs (default: 5)",
    )

    p_cloud_namespace = cloud_sub.add_parser(
        "namespace",
        help="Check Namespace CLI/login/workspace setup without replacing the upstream nsc tool",
    )
    cloud_namespace_sub = p_cloud_namespace.add_subparsers(dest="cloud_namespace_command")
    cloud_namespace_sub.add_parser("doctor", help="Show Namespace CLI, login, and workspace status")
    cloud_namespace_sub.add_parser("setup", help="Run the thin Namespace setup flow (`nsc login` if needed)")

    sub.add_parser("list", help="Show open PRs")

    p_bump = sub.add_parser("bump", help="Reprioritize a pending job")
    p_bump.add_argument("job", help="Job id, unique id prefix, or exact branch name")
    p_bump.add_argument("priority", choices=sorted(PRIORITY_VALUES), help="New priority")

    p_cancel = sub.add_parser("cancel", help="Cancel a pending job")
    p_cancel.add_argument("job", help="Job id, unique id prefix, or exact branch name")

    p_logs = sub.add_parser("logs", help="Tail saved logs for a running or completed job")
    p_logs.add_argument("job", nargs="?", help="Job id, unique id prefix, or exact branch name (default: active/latest)")
    p_logs.add_argument("--target", help="Target name to show (mac, ubuntu, windows)")
    p_logs.add_argument("--lines", type=int, default=80, help="Number of log lines to show (default: 80)")

    p_cleanup = sub.add_parser("cleanup", help="Inspect or prune retained local CI artifacts")
    cleanup_mode = p_cleanup.add_mutually_exclusive_group()
    cleanup_mode.add_argument(
        "--dry-run",
        action="store_true",
        help="Show the cleanup plan without deleting anything (default)",
    )
    cleanup_mode.add_argument(
        "--apply",
        action="store_true",
        help="Delete the reported stale artifacts instead of only showing a dry-run plan",
    )
    p_cleanup.add_argument(
        "--include-prepared",
        action="store_true",
        help="Also include prepared build/install trees; later reruns will rebuild them",
    )
    p_cleanup.add_argument(
        "--keep-results",
        type=int,
        default=KEEP_COMPLETED_JOBS,
        help=f"Keep this many orphaned result files outside retained queue history (default: {KEEP_COMPLETED_JOBS})",
    )
    p_cleanup.add_argument(
        "--keep-logs",
        type=int,
        default=KEEP_COMPLETED_JOBS,
        help=f"Keep this many orphaned log directories outside retained queue history (default: {KEEP_COMPLETED_JOBS})",
    )
    p_cleanup.add_argument(
        "--keep-bundles",
        type=int,
        default=0,
        help="Keep this many non-live git bundles instead of deleting all completed-job bundles (default: 0)",
    )

    p_evidence = sub.add_parser("evidence", help="Show accumulated last-good target results by exact SHA")
    p_evidence.add_argument("branch", nargs="?", help="Branch name (default: current)")
    p_evidence.add_argument("--sha", help="Filter to one exact SHA")
    p_evidence.add_argument("--limit", type=int, default=5, help="Shas to show per validation mode (default: 5)")

    sub.add_parser("status", help="Show queue, runner, results, and VM status")
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()

    commands = {
        "enqueue": cmd_enqueue,
        "drain": cmd_drain,
        "run": cmd_run,
        "ship": cmd_ship,
        "check": cmd_check,
        "list": cmd_list,
        "bump": cmd_bump,
        "cancel": cmd_cancel,
        "logs": cmd_logs,
        "cleanup": cmd_cleanup,
        "evidence": cmd_evidence,
        "status": cmd_status,
    }

    if args.command == "cloud":
        if args.cloud_command == "workflows":
            return cmd_cloud_workflows(args)
        if args.cloud_command == "defaults":
            return cmd_cloud_defaults(args)
        if args.cloud_command == "history":
            return cmd_cloud_history(args)
        if args.cloud_command == "compare":
            return cmd_cloud_compare(args)
        if args.cloud_command == "recommend":
            return cmd_cloud_recommend(args)
        if args.cloud_command == "run":
            return cmd_cloud_run(args)
        if args.cloud_command == "status":
            return cmd_cloud_status(args)
        if args.cloud_command == "namespace":
            if args.cloud_namespace_command == "doctor":
                return cmd_cloud_namespace_doctor(args)
            if args.cloud_namespace_command == "setup":
                return cmd_cloud_namespace_setup(args)
            print("Error: missing cloud namespace subcommand. Use `pulp ci-local cloud namespace doctor`.")
            return 1
        print("Error: missing cloud subcommand. Use `pulp ci-local cloud workflows`.")
        return 1

    if args.command in commands:
        return commands[args.command](args)

    parser.print_help()
    return 1


if __name__ == "__main__":
    sys.exit(main())

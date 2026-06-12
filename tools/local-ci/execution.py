"""Validation command execution helpers for local CI.

This module owns subprocess output capture, progress marker parsing, heartbeat
updates, optional command log writing, target-neutral command result assembly,
and local/POSIX/Windows validation runner orchestration.
"""

from __future__ import annotations

from collections.abc import Callable
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime
import json
import queue as queue_module
import shlex
import subprocess
import threading
import time
from pathlib import Path

from git_helpers import now_iso
from io_utils import trim_line
from state_paths import state_dir
from validation_commands import (
    local_validation_command,
    posix_ssh_validation_command,
    prepared_state_root,
    remote_commit_error,
    should_reuse_prepared_state,
    windows_validation_script,
)
from validation_results import (
    completed_job_result,
    sorted_target_results,
    target_exception_result,
    unreachable_target_result,
    validation_error_result,
    validation_result_from_run,
)


HEARTBEAT_INTERVAL_SECS = 15.0
STUCK_IDLE_SECS = 90.0


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


def config_for_job_execution(
    job: dict,
    config: dict,
    *,
    load_config_file_fn: Callable[[str], dict],
    warn_fn: Callable[[str], None],
) -> dict:
    submission = job.get("submission") or {}
    config_file = submission.get("config_path")
    if not config_file:
        return config
    try:
        return load_config_file_fn(config_file)
    except (FileNotFoundError, json.JSONDecodeError) as exc:
        warn_fn(f"  [scheduler] Warning: failed to load submission config {config_file}: {exc}")
        return config


def submission_target_state(job: dict, target_name: str) -> dict:
    submission = job.get("submission") or {}
    target_hosts = submission.get("target_hosts") or {}
    state = target_hosts.get(target_name)
    return state if isinstance(state, dict) else {}


def resolve_ssh_target_execution(
    job: dict,
    target_name: str,
    target_cfg: dict,
    defaults: dict,
    *,
    ensure_host_reachable_fn: Callable[[str, dict, dict], str | None],
) -> tuple[str | None, str | None]:
    state = submission_target_state(job, target_name)
    repo_path = state.get("repo_path") or target_cfg.get("repo_path")
    status = state.get("status")
    resolved_host = (state.get("resolved_host") or "").strip()
    configured_host = (state.get("configured_host") or target_cfg.get("host") or "").strip()

    if status in {"primary-up", "fallback-up"} and resolved_host:
        return resolved_host, repo_path

    if status == "unreachable":
        return None, repo_path

    if status == "utm-fallback-pending" and configured_host:
        queued_cfg = dict(target_cfg)
        queued_cfg["host"] = configured_host
        return ensure_host_reachable_fn(target_name, queued_cfg, defaults), repo_path

    return ensure_host_reachable_fn(target_name, target_cfg, defaults), repo_path


def build_target_tasks(
    job: dict,
    config: dict,
    *,
    enabled_targets_fn: Callable[[dict], list[str]],
    resolve_ssh_target_execution_fn: Callable[[dict, str, dict, dict], tuple[str | None, str | None]],
    run_local_validation_fn: Callable[..., dict],
    run_posix_ssh_validation_fn: Callable[..., dict],
    run_windows_ssh_validation_fn: Callable[..., dict],
    progress_factory: Callable[[str], object] | None = None,
) -> list[tuple[str, Callable[[], dict]]]:
    targets = config["targets"]
    defaults = config.get("defaults", {})
    requested = set(job.get("targets") or enabled_targets_fn(config))
    tasks: list[tuple[str, Callable[[], dict]]] = []

    mac_cfg = targets.get("mac", {})
    if "mac" in requested and mac_cfg.get("enabled", True):
        reporter = progress_factory("mac") if progress_factory else None
        tasks.append(("mac", lambda r=reporter: run_local_validation_fn(job, mac_cfg.get("exclude_tests", ""), r)))

    ubuntu_cfg = targets.get("ubuntu")
    if "ubuntu" in requested and ubuntu_cfg and ubuntu_cfg.get("enabled", True):
        host, repo_path = resolve_ssh_target_execution_fn(job, "ubuntu", ubuntu_cfg, defaults)
        if host:
            exc = ubuntu_cfg.get("exclude_tests", "")
            reporter = progress_factory("ubuntu") if progress_factory else None
            tasks.append(
                (
                    "ubuntu",
                    lambda h=host, repo=repo_path, e=exc, cfg=config, r=reporter: run_posix_ssh_validation_fn(
                        "ubuntu", h, repo, job, exclude_tests=e, config=cfg, report_progress=r
                    ),
                )
            )
        else:
            tasks.append(("ubuntu", lambda: unreachable_target_result("ubuntu")))

    win_cfg = targets.get("windows")
    if "windows" in requested and win_cfg and win_cfg.get("enabled", True):
        host, repo_path = resolve_ssh_target_execution_fn(job, "windows", win_cfg, defaults)
        if host:
            exc = win_cfg.get("exclude_tests", "")
            reporter = progress_factory("windows") if progress_factory else None
            generator = win_cfg.get("cmake_generator", "Visual Studio 17 2022")
            platform = win_cfg.get("cmake_platform", "")
            generator_instance = win_cfg.get("cmake_generator_instance", "")
            tasks.append(
                (
                    "windows",
                    lambda h=host, repo=repo_path, e=exc, cfg=config, r=reporter, g=generator, p=platform, i=generator_instance: run_windows_ssh_validation_fn(
                        "windows",
                        h,
                        repo,
                        job,
                        exclude_tests=e,
                        cmake_generator=g,
                        cmake_platform=p,
                        cmake_generator_instance=i,
                        config=cfg,
                        report_progress=r,
                    ),
                )
            )
        else:
            tasks.append(("windows", lambda: unreachable_target_result("windows")))

    return tasks


def run_local_validation(
    job: dict,
    exclude_tests: str = "",
    report_progress=None,
    *,
    root: Path,
    print_fn: Callable[[str], None],
    short_sha_fn: Callable[[str], str],
    prepare_target_log_fn: Callable[[str, str], Path],
    now_iso_fn: Callable[[], str],
    local_validation_command_fn: Callable[[dict, str], tuple[list[str], str]],
    run_logged_command_fn: Callable[..., dict],
    validation_result_from_run_fn: Callable[..., dict],
) -> dict:
    print_fn(f"  [mac] Running local validation on {job['branch']} @ {short_sha_fn(job['sha'])}...")
    log_path = prepare_target_log_fn(job["id"], "mac")
    if report_progress:
        report_progress(
            phase="validate",
            log_path=str(log_path),
            last_output_at=now_iso_fn(),
            transport_mode="local",
        )

    cmd, validation = local_validation_command_fn(job, exclude_tests)

    run = run_logged_command_fn(cmd, cwd=root, timeout=3600, log_path=log_path, report_progress=report_progress)
    return validation_result_from_run_fn(
        "mac",
        run,
        log_path=log_path,
        validation=validation,
        transport_mode="local",
    )


def run_posix_ssh_validation(
    target_name: str,
    host: str,
    repo_path: str,
    job: dict,
    exclude_tests: str = "",
    config: dict | None = None,
    report_progress=None,
    *,
    print_fn: Callable[[str], None],
    short_sha_fn: Callable[[str], str],
    prepare_target_log_fn: Callable[[str, str], Path],
    now_iso_fn: Callable[[], str],
    sync_job_bundle_to_ssh_host_fn: Callable[..., tuple[str, str]],
    posix_ssh_validation_command_fn: Callable[..., tuple[list[str], str]],
    run_logged_command_fn: Callable[..., dict],
    validation_result_from_run_fn: Callable[..., dict],
    validation_error_result_fn: Callable[..., dict],
) -> dict:
    print_fn(f"  [{target_name}] Running validation on {host}:{repo_path} @ {short_sha_fn(job['sha'])}...")
    log_path = prepare_target_log_fn(job["id"], target_name)
    if report_progress:
        report_progress(
            phase="connect",
            host=host,
            log_path=str(log_path),
            last_output_at=now_iso_fn(),
            transport_mode="bundle",
        )

    try:
        bundle_name, bundle_ref = sync_job_bundle_to_ssh_host_fn(
            host,
            job,
            report_progress=report_progress,
            config=config,
        )
    except RuntimeError as exc:
        return validation_error_result_fn(target_name, str(exc), log_path=log_path, transport_mode="bundle")

    cmd, validation = posix_ssh_validation_command_fn(
        target_name,
        host,
        repo_path,
        job,
        bundle_name=bundle_name,
        bundle_ref=bundle_ref,
        exclude_tests=exclude_tests,
    )

    run = run_logged_command_fn(cmd, timeout=3600, log_path=log_path, report_progress=report_progress)
    return validation_result_from_run_fn(
        target_name,
        run,
        log_path=log_path,
        validation=validation,
        transport_mode="bundle",
    )


def run_windows_ssh_validation(
    target_name: str,
    host: str,
    repo_path: str,
    job: dict,
    exclude_tests: str = "",
    cmake_generator: str = "Visual Studio 17 2022",
    cmake_platform: str = "",
    cmake_generator_instance: str = "",
    config: dict | None = None,
    report_progress=None,
    *,
    root: Path,
    prepare_target_log_fn: Callable[[str, str], Path],
    sync_job_bundle_to_ssh_host_fn: Callable[..., tuple[str, str]],
    validation_error_result_fn: Callable[..., dict],
    ensure_windows_remote_repo_checkout_fn: Callable[..., dict | None],
    git_origin_clone_url_fn: Callable[[Path], str | None],
    print_fn: Callable[[str], None],
    short_sha_fn: Callable[[str], str],
    now_iso_fn: Callable[[], str],
    probe_windows_ssh_cmake_settings_fn: Callable[[str, str, str, str], tuple[str, str]],
    windows_validation_script_fn: Callable[..., tuple[str, str]],
    windows_ssh_powershell_command_fn: Callable[[str], list[str]],
    run_logged_command_fn: Callable[..., dict],
    validation_result_from_run_fn: Callable[..., dict],
) -> dict:
    log_path = prepare_target_log_fn(job["id"], target_name)
    try:
        bundle_name, bundle_ref = sync_job_bundle_to_ssh_host_fn(
            host,
            job,
            report_progress=report_progress,
            config=config,
        )
    except RuntimeError as exc:
        return validation_error_result_fn(target_name, str(exc), log_path=log_path, transport_mode="bundle")
    try:
        repo_probe = ensure_windows_remote_repo_checkout_fn(
            host,
            repo_path,
            remote_url=git_origin_clone_url_fn(root),
            bundle_name=bundle_name,
            bundle_ref=bundle_ref,
        )
    except RuntimeError as exc:
        return validation_error_result_fn(target_name, str(exc), log_path=log_path, transport_mode="bundle")

    if not isinstance(repo_probe, dict):
        return validation_error_result_fn(
            target_name,
            "Windows repo checkout probe returned no structured payload",
            log_path=log_path,
            transport_mode="bundle",
        )

    effective_repo_path = repo_probe.get("repo_path") or repo_path
    print_fn(f"  [{target_name}] Running validation on {host}:{effective_repo_path} @ {short_sha_fn(job['sha'])}...")
    if report_progress:
        report_progress(
            phase="connect",
            host=host,
            log_path=str(log_path),
            last_output_at=now_iso_fn(),
            transport_mode="bundle",
        )

    resolved_platform, resolved_generator_instance = probe_windows_ssh_cmake_settings_fn(
        host,
        cmake_generator,
        cmake_platform,
        cmake_generator_instance,
    )

    ps_script, validation = windows_validation_script_fn(
        target_name,
        host,
        effective_repo_path,
        job,
        bundle_name=bundle_name,
        bundle_ref=bundle_ref,
        exclude_tests=exclude_tests,
        cmake_generator=cmake_generator,
        resolved_platform=resolved_platform,
        resolved_generator_instance=resolved_generator_instance,
    )

    cmd = windows_ssh_powershell_command_fn(host)

    run = run_logged_command_fn(
        cmd,
        input_text=ps_script,
        timeout=3600,
        log_path=log_path,
        report_progress=report_progress,
    )
    return validation_result_from_run_fn(
        target_name,
        run,
        log_path=log_path,
        validation=validation,
        transport_mode="bundle",
    )


def process_job(
    job: dict,
    config: dict,
    *,
    print_fn: Callable[[str], None],
    short_sha_fn: Callable[[str], str],
    config_for_job_execution_fn: Callable[[dict, dict], dict],
    build_target_tasks_fn: Callable[..., list[tuple[str, Callable[[], dict]]]],
    target_state_snapshot_fn: Callable[[dict[str, dict]], dict[str, dict]],
    update_runner_active_targets_fn: Callable[[str, dict[str, dict]], None],
    update_job_active_targets_fn: Callable[[str, dict[str, dict]], None],
    updated_target_state_fn: Callable[[dict | None, dict], dict],
    initial_target_state_fn: Callable[..., dict],
    completed_target_state_fn: Callable[..., dict],
    now_iso_fn: Callable[[], str],
    run_target_tasks_fn: Callable[..., list[dict]],
    completed_job_result_fn: Callable[[dict, list[dict]], dict],
    sorted_target_results_fn: Callable[[list[dict]], list[dict]],
) -> dict:
    print_fn(
        f"\n=== Validating [{job['id']}] {job['branch']} @ {short_sha_fn(job['sha'])} "
        f"priority={job['priority']} ===\n"
    )
    config = config_for_job_execution_fn(job, config)

    target_states: dict[str, dict] = {}
    state_lock = threading.Lock()

    def flush_target_states() -> None:
        with state_lock:
            snapshot = target_state_snapshot_fn(target_states)
        update_runner_active_targets_fn(job["id"], snapshot)
        update_job_active_targets_fn(job["id"], snapshot)

    def progress_factory(name: str):
        def report(**fields) -> None:
            with state_lock:
                target_states[name] = updated_target_state_fn(target_states.get(name), fields)
            flush_target_states()

        return report

    tasks = build_target_tasks_fn(job, config, progress_factory=progress_factory)
    if not tasks:
        return completed_job_result_fn(job, [])

    for name, _fn in tasks:
        target_states[name] = initial_target_state_fn(job["id"], name, started_at=now_iso_fn())
    flush_target_states()

    def record_target_completion(name: str, result: dict) -> None:
        target_states[name] = completed_target_state_fn(
            job["id"],
            name,
            result,
            target_states.get(name, {}),
            completed_at=now_iso_fn(),
        )
        flush_target_states()

    results = run_target_tasks_fn(tasks, on_target_complete=record_target_completion)
    return completed_job_result_fn(job, sorted_target_results_fn(results))


def save_result(
    result: dict,
    *,
    ensure_state_dirs_fn: Callable[[], None],
    results_dir_fn: Callable[[], Path],
    update_evidence_index_fn: Callable[[dict, Path], None],
    now_fn: Callable[[], datetime] = datetime.now,
) -> Path:
    ensure_state_dirs_fn()
    ts = now_fn().strftime("%Y%m%d-%H%M%S")
    branch_slug = result["branch"].replace("/", "-")
    path = results_dir_fn() / f"{ts}-{result['job_id']}-{branch_slug}.json"
    path.write_text(json.dumps(result, indent=2) + "\n")
    update_evidence_index_fn(result, path)
    return path


def print_result(
    result: dict,
    result_path: Path | None = None,
    *,
    normalize_result_fn: Callable[[dict], dict],
    result_validation_line_fn: Callable[[dict], str | None],
    result_execution_line_fn: Callable[[dict], str],
    result_target_lines_fn: Callable[[dict], list[str]],
    result_overall_line_fn: Callable[[dict], str],
    print_fn: Callable[[str], None] = print,
) -> None:
    result = normalize_result_fn(result)
    print_fn(f"\n--- Result: [{result['job_id']}] {result['branch']} ---")
    validation_line = result_validation_line_fn(result)
    if validation_line:
        print_fn(validation_line)
    print_fn(result_execution_line_fn(result))
    for line in result_target_lines_fn(result):
        print_fn(line)
    print_fn(result_overall_line_fn(result))
    if result_path:
        print_fn(f"  Saved: {result_path}")
    print_fn("")


def run_target_tasks(
    tasks: list[tuple[str, Callable[[], dict]]],
    *,
    exception_result_fn: Callable[[str, Exception], dict],
    on_target_complete: Callable[[str, dict], None],
) -> list[dict]:
    if not tasks:
        return []

    results = []
    with ThreadPoolExecutor(max_workers=len(tasks)) as pool:
        futures = {pool.submit(fn): name for name, fn in tasks}
        for future in as_completed(futures):
            name = futures[future]
            try:
                result = future.result()
            except Exception as exc:
                result = exception_result_fn(name, exc)

            results.append(result)
            on_target_complete(name, result)
    return results


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

"""Validation command execution helpers for local CI.

This module owns target-neutral command result assembly and local/POSIX/Windows
validation runner orchestration.
"""

from __future__ import annotations

from collections.abc import Callable
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime
import json
import shlex
import subprocess
import threading
from pathlib import Path

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
from validation_logging import (
    HEARTBEAT_INTERVAL_SECS,
    STUCK_IDLE_SECS,
    parse_progress_marker,
    run_logged_command,
)
from validation_planning import (
    build_target_tasks,
    config_for_job_execution,
    resolve_ssh_target_execution,
    submission_target_state,
)


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

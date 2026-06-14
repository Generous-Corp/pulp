"""POSIX SSH validation runner orchestration."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path


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

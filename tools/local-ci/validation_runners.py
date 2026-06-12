"""Local and SSH validation runner helpers for local CI."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path


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

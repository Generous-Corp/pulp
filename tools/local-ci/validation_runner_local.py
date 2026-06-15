"""Local validation runner orchestration."""

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

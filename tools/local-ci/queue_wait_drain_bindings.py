"""Bindings from the local_ci facade to queue wait/drain helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr


def wait_for_job(bindings: Mapping[str, Any], job_id: str, config: dict) -> tuple[dict | None, int]:
    return _binding(bindings, "_queue_lifecycle").wait_for_job_completion(
        job_id,
        config,
        load_job_fn=_binding(bindings, "load_job"),
        load_result_fn=_binding(bindings, "load_result"),
        drain_pending_jobs_fn=_binding(bindings, "drain_pending_jobs"),
        current_runner_info_fn=_binding(bindings, "current_runner_info"),
        sleep_fn=_binding_attr(bindings, "time", "sleep"),
        poll_secs=_binding(bindings, "WAIT_POLL_SECS"),
    )


def drain_pending_jobs(bindings: Mapping[str, Any], config: dict, *, blocking: bool) -> tuple[bool, bool]:
    return _binding(bindings, "_queue_lifecycle").drain_pending_jobs_locked(
        config,
        blocking=blocking,
        root=_binding(bindings, "ROOT"),
        drain_lock_path_fn=_binding(bindings, "drain_lock_path"),
        file_lock_fn=_binding(bindings, "file_lock"),
        lock_busy_error_cls=_binding(bindings, "LockBusyError"),
        write_runner_info_fn=_binding(bindings, "write_runner_info"),
        clear_runner_info_fn=_binding(bindings, "clear_runner_info"),
        reclaim_stale_remote_validators_fn=_binding(bindings, "reclaim_stale_remote_validators"),
        claim_next_job_fn=_binding(bindings, "claim_next_job"),
        process_job_fn=_binding(bindings, "process_job"),
        save_result_fn=_binding(bindings, "save_result"),
        finalize_job_fn=_binding(bindings, "finalize_job"),
        print_result_fn=_binding(bindings, "print_result"),
        now_fn=_binding(bindings, "now_iso"),
        pid_fn=_binding(bindings, "os").getpid,
    )

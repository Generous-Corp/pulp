"""Cross-target validation job orchestration helpers for local CI."""

from __future__ import annotations

from collections.abc import Callable
from concurrent.futures import ThreadPoolExecutor, as_completed
import threading


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

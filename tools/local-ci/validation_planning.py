"""Validation target planning helpers for local CI."""

from __future__ import annotations

from collections.abc import Callable
import json

from validation_results import unreachable_target_result


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

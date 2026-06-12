"""Facade dependency bindings for SSH bundle creation and sync helpers."""

from __future__ import annotations

from pathlib import Path

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr


def bundle_ref_name(bindings: dict, job_id: str) -> str:
    return _binding(bindings, "_ssh_bundle").bundle_ref_name(job_id)


def remote_bundle_name(bindings: dict, job_id: str) -> str:
    return _binding(bindings, "_ssh_bundle").remote_bundle_name(job_id)


def create_job_bundle(bindings: dict, job: dict) -> Path:
    return _binding(bindings, "_ssh_bundle").create_job_bundle(
        job,
        ensure_state_dirs_fn=_binding(bindings, "ensure_state_dirs"),
        bundles_dir_fn=_binding(bindings, "bundles_dir"),
        bundle_build_lock=_binding(bindings, "_BUNDLE_BUILD_LOCK"),
        root=_binding(bindings, "ROOT"),
        run_fn=_binding_attr(bindings, "subprocess", "run"),
    )


def config_for_bundle_probe(bindings: dict, job: dict, config: dict | None = None) -> dict:
    return _binding(bindings, "_ssh_bundle").config_for_bundle_probe(
        job,
        config,
        load_config_file_fn=_binding(bindings, "load_config_file"),
        load_optional_config_fn=_binding(bindings, "load_optional_config"),
    )


def sync_job_bundle_to_ssh_host(
    bindings: dict,
    host: str,
    job: dict,
    report_progress=None,
    config: dict | None = None,
) -> tuple[str, str]:
    subprocess_module = _binding(bindings, "subprocess")
    return _binding(bindings, "_ssh_bundle").sync_job_bundle_to_ssh_host(
        host,
        job,
        report_progress=report_progress,
        config=config,
        create_job_bundle_fn=_binding(bindings, "create_job_bundle"),
        remote_bundle_name_fn=_binding(bindings, "remote_bundle_name"),
        bundle_ref_name_fn=_binding(bindings, "bundle_ref_name"),
        config_for_bundle_probe_fn=_binding(bindings, "config_for_bundle_probe"),
        probe_uploaded_bundle_size_fn=_binding(bindings, "probe_uploaded_bundle_size"),
        now_iso_fn=_binding(bindings, "now_iso"),
        popen_fn=subprocess_module.Popen,
        stdout_pipe=subprocess_module.PIPE,
        stderr_pipe=subprocess_module.PIPE,
        timeout_expired_type=subprocess_module.TimeoutExpired,
        time_fn=_binding_attr(bindings, "time", "time"),
    )

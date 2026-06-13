"""Dependency bindings for SSH bundle upload/sync helpers."""

from __future__ import annotations

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr


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

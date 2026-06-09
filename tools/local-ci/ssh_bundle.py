"""SSH git-bundle transport helpers for local-ci.

This module owns the pure bundle naming/build/upload mechanics. The
`local_ci.py` entrypoint keeps thin wrappers around these helpers so existing
tests and callers can still monkey-patch the historical `local_ci.*` names.
"""

from __future__ import annotations

import json
import subprocess
import time
from pathlib import Path
from typing import Callable, TextIO


def bundle_ref_name(job_id: str) -> str:
    return f"refs/pulp-ci-bundles/{job_id}"


def remote_bundle_name(job_id: str) -> str:
    return f"pulp-ci-{job_id}.bundle"


def create_job_bundle(
    job: dict,
    *,
    ensure_state_dirs_fn: Callable[[], None],
    bundles_dir_fn: Callable[[], Path],
    bundle_build_lock,
    root: Path,
    run_fn: Callable[..., subprocess.CompletedProcess],
) -> Path:
    ensure_state_dirs_fn()
    bundle_path = bundles_dir_fn() / f"{job['id']}.bundle"
    bundle_lock_path = Path(f"{bundle_path}.lock")

    with bundle_build_lock:
        if bundle_path.exists() and bundle_path.stat().st_size > 0:
            return bundle_path

        bundle_lock_path.unlink(missing_ok=True)
        bundle_path.unlink(missing_ok=True)

        temp_ref = bundle_ref_name(job["id"])
        run_fn(["git", "update-ref", temp_ref, job["sha"]], cwd=root, check=True)
        try:
            run_fn(["git", "bundle", "create", str(bundle_path), temp_ref], cwd=root, check=True)
        finally:
            run_fn(["git", "update-ref", "-d", temp_ref], cwd=root, check=True)
    return bundle_path


def config_for_bundle_probe(
    job: dict,
    config: dict | None = None,
    *,
    load_config_file_fn: Callable[[str], dict],
    load_optional_config_fn: Callable[[], dict | None],
) -> dict:
    if config:
        return config
    submission = job.get("submission") or {}
    config_file = submission.get("config_path")
    if config_file:
        try:
            return load_config_file_fn(config_file)
        except (FileNotFoundError, json.JSONDecodeError):
            pass
    optional = load_optional_config_fn()
    return optional or {"targets": {}}


def sync_job_bundle_to_ssh_host(
    host: str,
    job: dict,
    *,
    report_progress=None,
    config: dict | None = None,
    create_job_bundle_fn: Callable[[dict], Path],
    remote_bundle_name_fn: Callable[[str], str],
    bundle_ref_name_fn: Callable[[str], str],
    config_for_bundle_probe_fn: Callable[[dict, dict | None], dict],
    probe_uploaded_bundle_size_fn: Callable[..., int | None],
    now_iso_fn: Callable[[], str],
    popen_fn: Callable[..., subprocess.Popen],
    stdout_pipe: int | TextIO,
    stderr_pipe: int | TextIO,
    timeout_expired_type: type[Exception],
    time_fn: Callable[[], float] = time.time,
) -> tuple[str, str]:
    bundle_path = create_job_bundle_fn(job)
    remote_name = remote_bundle_name_fn(job["id"])
    probe_config = config_for_bundle_probe_fn(job, config)
    try:
        if report_progress:
            report_progress(
                phase="bundle-upload",
                host=host,
                bundle=remote_name,
                last_output_at=now_iso_fn(),
                transport_mode="bundle",
            )
        upload = popen_fn(
            ["scp", str(bundle_path), f"{host}:{remote_name}"],
            stdout=stdout_pipe,
            stderr=stderr_pipe,
            text=True,
        )
        bundle_size = bundle_path.stat().st_size
        deadline = time_fn() + 300
        stdout = ""
        stderr = ""
        while True:
            remaining = deadline - time_fn()
            if remaining <= 0:
                upload.kill()
                stdout, stderr = upload.communicate()
                raise RuntimeError(
                    f"failed to upload git bundle to {host}: timed out waiting for scp to finish"
                )
            try:
                stdout, stderr = upload.communicate(timeout=min(5.0, max(1.0, remaining)))
            except timeout_expired_type:
                remote_size = probe_uploaded_bundle_size_fn(host, remote_name, config=probe_config)
                if remote_size is not None and remote_size >= bundle_size:
                    upload.terminate()
                    try:
                        upload.communicate(timeout=2.0)
                    except timeout_expired_type:
                        upload.kill()
                        upload.communicate()
                    break
                continue
            if upload.returncode != 0:
                detail = (stderr or stdout or "").strip()
                raise RuntimeError(f"failed to upload git bundle to {host}: {detail or f'scp exited {upload.returncode}'}")
            break
    except OSError as exc:
        raise RuntimeError(f"failed to upload git bundle to {host}: {exc}") from exc
    return remote_name, bundle_ref_name_fn(job["id"])

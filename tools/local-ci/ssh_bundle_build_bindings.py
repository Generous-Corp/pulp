"""Dependency bindings for SSH bundle creation and probe config helpers."""

from __future__ import annotations

from pathlib import Path

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr


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

"""Bindings from the local_ci facade to desktop install command helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import install_local_helpers
from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr


DESKTOP_INSTALL_COMMAND_EXPORTS = ("cmd_desktop_install",)


def cmd_desktop_install(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_setup_commands_cli").cmd_desktop_install(
        args,
        load_config_fn=_binding(bindings, "load_config"),
        resolve_desktop_target_fn=_binding(bindings, "resolve_desktop_target"),
        check_writable_dir_fn=_binding(bindings, "_check_writable_dir"),
        desktop_target_contract_fn=_binding(bindings, "desktop_target_contract"),
        ensure_host_reachable_fn=_binding(bindings, "ensure_host_reachable"),
        bootstrap_windows_session_agent_fn=_binding(bindings, "bootstrap_windows_session_agent"),
        probe_windows_session_agent_fn=_binding(bindings, "probe_windows_session_agent"),
        subprocess_run_fn=_binding_attr(bindings, "subprocess", "run"),
        root_path=_binding(bindings, "ROOT"),
        new_install_job_id_fn=lambda: _binding_attr(bindings, "uuid", "uuid4")().hex[:12],
        sync_job_bundle_to_ssh_host_fn=_binding(bindings, "sync_job_bundle_to_ssh_host"),
        ensure_windows_remote_tooling_fn=_binding(bindings, "ensure_windows_remote_tooling"),
        windows_remote_tooling_ready_fn=_binding(bindings, "windows_remote_tooling_ready"),
        ensure_windows_remote_repo_checkout_fn=_binding(bindings, "ensure_windows_remote_repo_checkout"),
        git_origin_clone_url_fn=_binding(bindings, "git_origin_clone_url"),
        windows_repo_checkout_ready_fn=_binding(bindings, "windows_repo_checkout_ready"),
        update_target_repo_path_fn=_binding(bindings, "update_target_repo_path"),
        save_config_fn=_binding(bindings, "save_config"),
        now_iso_fn=_binding(bindings, "now_iso"),
        desktop_target_receipt_path_fn=_binding(bindings, "desktop_target_receipt_path"),
        atomic_write_text_fn=_binding(bindings, "atomic_write_text"),
        windows_tooling_detail_fn=_binding(bindings, "windows_tooling_detail"),
    )


def install_desktop_install_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_INSTALL_COMMAND_EXPORTS,
) -> None:
    known_names = set(DESKTOP_INSTALL_COMMAND_EXPORTS)
    command_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), command_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)

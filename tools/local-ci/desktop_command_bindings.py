"""Bindings from the local_ci facade to desktop command helpers."""

from __future__ import annotations

import json
from collections.abc import Mapping
from typing import Any


def _binding(bindings: Mapping[str, Any], name: str) -> Any:
    return bindings[name]


def _terminal_stdout(result: Mapping[str, Any]) -> str:
    stdout = str(result.get("stdout") or "")
    cleanup = result.get("terminal_cleanup")
    title = result.get("terminal_title")
    if not stdout or not (cleanup or title):
        return stdout
    try:
        payload = json.loads(stdout)
    except json.JSONDecodeError:
        return stdout
    if not isinstance(payload, dict):
        return stdout
    payload["terminal_reinvoke"] = {
        "title": title,
        "cleanup": cleanup,
        "timed_out": bool(result.get("timed_out")),
    }
    return json.dumps(payload, indent=2) + "\n"


def _maybe_run_local_ci_in_terminal(bindings: Mapping[str, Any], args: Any) -> int | None:
    if not getattr(args, "run_in_terminal", False):
        return None
    sys_mod = _binding(bindings, "sys")
    os_mod = _binding(bindings, "os")
    terminal_runner = _binding(bindings, "_macos_terminal_runner")
    if not terminal_runner.should_reinvoke_in_terminal(
        requested=True,
        sys_platform=sys_mod.platform,
        environ=os_mod.environ,
    ):
        return None
    result = terminal_runner.run_local_ci_in_terminal(
        sys_mod.argv[1:],
        cwd=_binding(bindings, "ROOT"),
        python_executable=sys_mod.executable,
        script_path=_binding(bindings, "ROOT") / "tools" / "local-ci" / "local_ci.py",
    )
    if result.get("stdout"):
        sys_mod.stdout.write(_terminal_stdout(result))
    if result.get("stderr"):
        sys_mod.stderr.write(result["stderr"])
    terminal_title = result.get("terminal_title")
    if terminal_title and not result.get("terminal_cleanup"):
        terminal_runner.close_terminal_windows_with_title(terminal_title)
    return int(result["returncode"])


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
        subprocess_run_fn=_binding(bindings, "subprocess").run,
        root_path=_binding(bindings, "ROOT"),
        new_install_job_id_fn=lambda: _binding(bindings, "uuid").uuid4().hex[:12],
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


def cmd_desktop_doctor(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_setup_commands_cli").cmd_desktop_doctor(
        args,
        load_config_fn=_binding(bindings, "load_config"),
        resolve_desktop_target_fn=_binding(bindings, "resolve_desktop_target"),
        desktop_doctor_checks_fn=_binding(bindings, "desktop_doctor_checks"),
    )


def cmd_desktop_video_doctor(bindings: Mapping[str, Any], args: Any) -> int:
    terminal_result = _maybe_run_local_ci_in_terminal(bindings, args)
    if terminal_result is not None:
        return terminal_result
    return _binding(bindings, "_desktop_setup_commands_cli").cmd_desktop_video_doctor(
        args,
        load_config_fn=_binding(bindings, "load_config"),
        resolve_desktop_target_fn=_binding(bindings, "resolve_desktop_target"),
        desktop_doctor_checks_fn=_binding(bindings, "desktop_doctor_checks"),
        normalize_desktop_optional_config_fn=_binding(bindings, "normalize_desktop_optional_config"),
        video_proof_smoke_fn=_binding(bindings, "video_proof_smoke"),
        probe_macos_avfoundation_audio_fn=_binding(bindings, "probe_macos_avfoundation_audio"),
    )


def cmd_desktop_video_setup(bindings: Mapping[str, Any], args: Any) -> int:
    terminal_result = _maybe_run_local_ci_in_terminal(bindings, args)
    if terminal_result is not None:
        return terminal_result
    return _binding(bindings, "_desktop_setup_commands_cli").cmd_desktop_video_setup(
        args,
        load_config_fn=_binding(bindings, "load_config"),
        resolve_desktop_target_fn=_binding(bindings, "resolve_desktop_target"),
        desktop_doctor_checks_fn=_binding(bindings, "desktop_doctor_checks"),
        normalize_desktop_optional_config_fn=_binding(bindings, "normalize_desktop_optional_config"),
        video_proof_smoke_fn=_binding(bindings, "video_proof_smoke"),
        probe_macos_avfoundation_audio_fn=_binding(bindings, "probe_macos_avfoundation_audio"),
        desktop_video_matrix_payload_fn=_binding(bindings, "desktop_video_matrix_payload"),
    )


def cmd_desktop_status(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_commands_cli").cmd_desktop_status(
        args,
        load_config_fn=_binding(bindings, "load_config"),
        desktop_receipt_for_fn=_binding(bindings, "desktop_receipt_for"),
        desktop_capabilities_for_fn=_binding(bindings, "desktop_capabilities_for"),
        desktop_optional_capabilities_fn=_binding(bindings, "desktop_optional_capabilities"),
        desktop_run_manifests_fn=_binding(bindings, "desktop_run_manifests"),
        desktop_run_summary_fn=_binding(bindings, "desktop_run_summary"),
        desktop_proof_summaries_fn=_binding(bindings, "desktop_proof_summaries"),
        normalize_desktop_optional_config_fn=_binding(bindings, "normalize_desktop_optional_config"),
        desktop_target_contract_fn=_binding(bindings, "desktop_target_contract"),
        desktop_publish_reports_fn=_binding(bindings, "desktop_publish_reports"),
        desktop_status_lines_fn=_binding(bindings, "_desktop_cli").desktop_status_lines,
        short_sha_fn=_binding(bindings, "short_sha"),
        windows_tooling_detail_fn=_binding(bindings, "windows_tooling_detail"),
        windows_repo_checkout_detail_fn=_binding(bindings, "windows_repo_checkout_detail"),
    )


def cmd_desktop_config_show(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_commands_cli").cmd_desktop_config_show(
        args,
        load_config_fn=_binding(bindings, "load_config"),
        desktop_config_show_lines_fn=_binding(bindings, "_desktop_cli").desktop_config_show_lines,
    )


def cmd_desktop_config_set(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_commands_cli").cmd_desktop_config_set(
        args,
        load_config_fn=_binding(bindings, "load_config"),
        save_config_fn=_binding(bindings, "save_config"),
        config_path_fn=_binding(bindings, "config_path"),
        normalize_publish_mode_fn=_binding(bindings, "normalize_publish_mode"),
        parse_config_bool_fn=_binding(bindings, "parse_config_bool"),
        normalize_desktop_config_fn=_binding(bindings, "normalize_desktop_config"),
        desktop_config_update_lines_fn=_binding(bindings, "_desktop_cli").desktop_config_update_lines,
    )


def cmd_desktop_recent(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_commands_cli").cmd_desktop_recent(
        args,
        load_config_fn=_binding(bindings, "load_config"),
        desktop_run_manifests_fn=_binding(bindings, "desktop_run_manifests"),
        desktop_run_summary_fn=_binding(bindings, "desktop_run_summary"),
        desktop_recent_lines_fn=_binding(bindings, "_desktop_cli").desktop_recent_lines,
        short_sha_fn=_binding(bindings, "short_sha"),
    )


def cmd_desktop_proof(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_commands_cli").cmd_desktop_proof(
        args,
        load_config_fn=_binding(bindings, "load_config"),
        desktop_proof_summaries_fn=_binding(bindings, "desktop_proof_summaries"),
        desktop_proof_empty_line_fn=_binding(bindings, "_desktop_cli").desktop_proof_empty_line,
        desktop_proof_lines_fn=_binding(bindings, "_desktop_cli").desktop_proof_lines,
        short_sha_fn=_binding(bindings, "short_sha"),
    )


def cmd_desktop_publish(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_commands_cli").cmd_desktop_publish(
        args,
        load_config_fn=_binding(bindings, "load_config"),
        desktop_run_manifests_fn=_binding(bindings, "desktop_run_manifests"),
        stage_desktop_publish_report_fn=_binding(bindings, "stage_desktop_publish_report"),
        desktop_publish_lines_fn=_binding(bindings, "_desktop_cli").desktop_publish_lines,
        desktop_serve_candidate_urls_fn=_binding(bindings, "desktop_serve_candidate_urls"),
    )


def cmd_desktop_verdict(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_commands_cli").cmd_desktop_verdict(
        args,
        now_iso_fn=_binding(bindings, "now_iso"),
        atomic_write_text_fn=_binding(bindings, "atomic_write_text"),
    )


def cmd_desktop_review_issue(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_commands_cli").cmd_desktop_review_issue(
        args,
        desktop_review_issue_draft_fn=_binding(bindings, "desktop_review_issue_draft"),
        atomic_write_text_fn=_binding(bindings, "atomic_write_text"),
    )


def cmd_desktop_review_status(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_commands_cli").cmd_desktop_review_status(args)


def cmd_desktop_compose_video(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_commands_cli").cmd_desktop_compose_video(
        args,
        compose_desktop_video_proof_fn=_binding(bindings, "compose_desktop_video_proof"),
        create_issue_video_variant_fn=_binding(bindings, "create_issue_video_variant"),
        atomic_write_text_fn=_binding(bindings, "atomic_write_text"),
    )


def cmd_desktop_design_diff(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_commands_cli").cmd_desktop_design_diff(
        args,
        design_parity_diff_summary_fn=_binding(bindings, "design_parity_diff_summary"),
        atomic_write_text_fn=_binding(bindings, "atomic_write_text"),
    )


def cmd_desktop_video_matrix(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_commands_cli").cmd_desktop_video_matrix(
        args,
        load_config_fn=_binding(bindings, "load_config"),
    )


def cmd_desktop_serve(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_commands_cli").cmd_desktop_serve(
        args,
        load_config_fn=_binding(bindings, "load_config"),
        desktop_publish_reports_fn=_binding(bindings, "desktop_publish_reports"),
    )


def cmd_desktop_cleanup(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_commands_cli").cmd_desktop_cleanup(
        args,
        load_config_fn=_binding(bindings, "load_config"),
        prune_desktop_run_manifests_fn=_binding(bindings, "prune_desktop_run_manifests"),
        write_desktop_run_rollups_fn=_binding(bindings, "write_desktop_run_rollups"),
        desktop_cleanup_empty_line_fn=_binding(bindings, "_desktop_cli").desktop_cleanup_empty_line,
        desktop_cleanup_lines_fn=_binding(bindings, "_desktop_cli").desktop_cleanup_lines,
        desktop_publish_reports_fn=_binding(bindings, "desktop_publish_reports"),
        write_desktop_publish_rollups_fn=_binding(bindings, "write_desktop_publish_rollups"),
    )


def cmd_desktop_video(bindings: Mapping[str, Any], args: Any) -> int:
    terminal_result = _maybe_run_local_ci_in_terminal(bindings, args)
    if terminal_result is not None:
        return terminal_result
    return _binding(bindings, "_desktop_action_commands_cli").cmd_desktop_video(
        args,
        cmd_desktop_smoke_fn=lambda video_args: cmd_desktop_smoke(bindings, video_args),
        cmd_desktop_click_fn=lambda video_args: cmd_desktop_click(bindings, video_args),
        cmd_desktop_inspect_fn=lambda video_args: cmd_desktop_inspect(bindings, video_args),
    )


def windows_requires_pulp_app_selectors(bindings: Mapping[str, Any], args: Any) -> bool:
    return _binding(bindings, "_desktop_action_commands_cli").windows_requires_pulp_app_selectors(args)


def _desktop_action_kwargs(bindings: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "load_config_fn": _binding(bindings, "load_config"),
        "resolve_desktop_target_fn": _binding(bindings, "resolve_desktop_target"),
        "make_desktop_source_request_fn": _binding(bindings, "make_desktop_source_request"),
        "run_macos_local_smoke_fn": _binding(bindings, "run_macos_local_smoke"),
        "run_linux_xvfb_remote_action_fn": _binding(bindings, "run_linux_xvfb_remote_action"),
        "run_windows_session_agent_action_fn": _binding(bindings, "run_windows_session_agent_action"),
        "desktop_action_success_lines_fn": _binding(bindings, "_desktop_cli").desktop_action_success_lines,
        "sys_platform": _binding(bindings, "sys").platform,
    }


def cmd_desktop_smoke(bindings: Mapping[str, Any], args: Any) -> int:
    terminal_result = _maybe_run_local_ci_in_terminal(bindings, args)
    if terminal_result is not None:
        return terminal_result
    return _binding(bindings, "_desktop_action_commands_cli").cmd_desktop_smoke(
        args,
        **_desktop_action_kwargs(bindings),
    )


def cmd_desktop_click(bindings: Mapping[str, Any], args: Any) -> int:
    terminal_result = _maybe_run_local_ci_in_terminal(bindings, args)
    if terminal_result is not None:
        return terminal_result
    return _binding(bindings, "_desktop_action_commands_cli").cmd_desktop_click(
        args,
        **_desktop_action_kwargs(bindings),
    )


def cmd_desktop_inspect(bindings: Mapping[str, Any], args: Any) -> int:
    terminal_result = _maybe_run_local_ci_in_terminal(bindings, args)
    if terminal_result is not None:
        return terminal_result
    return _binding(bindings, "_desktop_action_commands_cli").cmd_desktop_inspect(
        args,
        **_desktop_action_kwargs(bindings),
    )

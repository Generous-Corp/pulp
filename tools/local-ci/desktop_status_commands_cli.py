"""Desktop automation status command orchestration."""

from __future__ import annotations

import argparse
from collections.abc import Callable

from desktop_command_flow import emit_desktop_command_result, load_desktop_command_config, run_desktop_command_step
from desktop_status_payload import desktop_status_payload


def cmd_desktop_status(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict],
    desktop_receipt_for_fn: Callable[[str], dict],
    desktop_capabilities_for_fn: Callable[[str, str, dict | None], list[str]],
    desktop_optional_capabilities_fn: Callable[[dict | None], list[str]],
    desktop_run_manifests_fn: Callable[..., list[dict]],
    desktop_run_summary_fn: Callable[[dict, dict], dict],
    desktop_proof_summaries_fn: Callable[..., list[dict]],
    normalize_desktop_optional_config_fn: Callable[[dict | None], dict],
    desktop_target_contract_fn: Callable[[str, dict], dict],
    desktop_publish_reports_fn: Callable[..., list[dict]],
    desktop_status_lines_fn: Callable[..., list[str]],
    short_sha_fn: Callable[[str], str],
    windows_tooling_detail_fn: Callable[..., str],
    windows_repo_checkout_detail_fn: Callable[..., str],
    print_fn: Callable[[str], None] = print,
) -> int:
    config, status = load_desktop_command_config(load_config_fn=load_config_fn, print_fn=print_fn)
    if status is not None:
        return status

    payload, status = run_desktop_command_step(
        lambda: desktop_status_payload(
            config,
            target_name=args.target,
            desktop_receipt_for_fn=desktop_receipt_for_fn,
            desktop_capabilities_for_fn=desktop_capabilities_for_fn,
            desktop_optional_capabilities_fn=desktop_optional_capabilities_fn,
            desktop_run_manifests_fn=desktop_run_manifests_fn,
            desktop_run_summary_fn=desktop_run_summary_fn,
            desktop_proof_summaries_fn=desktop_proof_summaries_fn,
            normalize_desktop_optional_config_fn=normalize_desktop_optional_config_fn,
            desktop_target_contract_fn=desktop_target_contract_fn,
            desktop_publish_reports_fn=desktop_publish_reports_fn,
        ),
        print_fn=print_fn,
        error_prefix="\nError: ",
        handled_exceptions=(ValueError,),
    )
    if status is not None:
        return status

    return emit_desktop_command_result(
        payload=payload,
        json_output=getattr(args, "json", False),
        text_lines=desktop_status_lines_fn(
            config["desktop_automation"],
            payload["targets"],
            latest_publish=payload["latest_publish"],
            short_sha_fn=short_sha_fn,
            windows_tooling_detail_fn=windows_tooling_detail_fn,
            windows_repo_checkout_detail_fn=windows_repo_checkout_detail_fn,
        ),
        print_fn=print_fn,
    )

"""Desktop automation management command orchestration for pulp ci-local."""

from __future__ import annotations

import argparse
from collections.abc import Callable
from pathlib import Path
import shutil

from desktop_command_flow import (
    emit_desktop_command_result,
    load_desktop_command_config,
    require_desktop_run_manifests,
    run_desktop_command_step,
)
from desktop_config_commands_cli import (
    cmd_desktop_config,
    cmd_desktop_config_set,
    cmd_desktop_config_show,
)
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


def cmd_desktop_recent(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict],
    desktop_run_manifests_fn: Callable[..., list[dict]],
    desktop_run_summary_fn: Callable[[dict, dict], dict],
    desktop_recent_lines_fn: Callable[..., list[str]],
    short_sha_fn: Callable[[str], str],
    print_fn: Callable[[str], None] = print,
) -> int:
    config, status = load_desktop_command_config(load_config_fn=load_config_fn, print_fn=print_fn)
    if status is not None:
        return status

    manifests = desktop_run_manifests_fn(config, target_name=args.target, action=args.action)
    if not require_desktop_run_manifests(
        manifests,
        empty_line="No desktop automation runs found.",
        print_fn=print_fn,
    ):
        return 0
    manifests = manifests[: args.limit]

    run_summaries = [desktop_run_summary_fn(config, manifest) for manifest in manifests]
    return emit_desktop_command_result(
        payload={"runs": manifests},
        json_output=getattr(args, "json", False),
        text_lines=desktop_recent_lines_fn(run_summaries, short_sha_fn=short_sha_fn),
        print_fn=print_fn,
    )


def cmd_desktop_proof(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict],
    desktop_proof_summaries_fn: Callable[..., list[dict]],
    desktop_proof_empty_line_fn: Callable[..., str],
    desktop_proof_lines_fn: Callable[..., list[str]],
    short_sha_fn: Callable[[str], str],
    print_fn: Callable[[str], None] = print,
) -> int:
    config, status = load_desktop_command_config(load_config_fn=load_config_fn, print_fn=print_fn)
    if status is not None:
        return status

    proofs, status = run_desktop_command_step(
        lambda: desktop_proof_summaries_fn(
            config,
            target_name=args.target,
            action=args.action,
            source_mode=args.source_mode,
            sha=args.sha,
            branch=args.branch,
            limit=args.limit,
        ),
        print_fn=print_fn,
    )
    if status is not None:
        return status

    if not proofs:
        print_fn(
            desktop_proof_empty_line_fn(
                target=args.target,
                action=args.action,
                source_mode=args.source_mode,
                sha=args.sha,
                branch=args.branch,
                short_sha_fn=short_sha_fn,
            )
        )
        return 0

    return emit_desktop_command_result(
        payload={"proofs": proofs},
        json_output=getattr(args, "json", False),
        text_lines=desktop_proof_lines_fn(proofs, short_sha_fn=short_sha_fn),
        print_fn=print_fn,
    )


def cmd_desktop_publish(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict],
    desktop_run_manifests_fn: Callable[..., list[dict]],
    stage_desktop_publish_report_fn: Callable[..., dict],
    desktop_publish_lines_fn: Callable[[dict], list[str]],
    print_fn: Callable[[str], None] = print,
) -> int:
    config, status = load_desktop_command_config(load_config_fn=load_config_fn, print_fn=print_fn)
    if status is not None:
        return status

    manifests = desktop_run_manifests_fn(config, target_name=args.target, action=args.action)
    if not require_desktop_run_manifests(
        manifests,
        empty_line="No desktop automation runs found.",
        print_fn=print_fn,
    ):
        return 0

    manifests = manifests[: args.limit]
    output_dir = Path(args.output).expanduser() if args.output else None

    report, status = run_desktop_command_step(
        lambda: stage_desktop_publish_report_fn(config, manifests, output_dir=output_dir, label=args.label),
        print_fn=print_fn,
    )
    if status is not None:
        return status

    return emit_desktop_command_result(
        payload=report,
        json_output=getattr(args, "json", False),
        text_lines=desktop_publish_lines_fn(report),
        print_fn=print_fn,
    )


def cmd_desktop_cleanup(
    args: argparse.Namespace,
    *,
    load_config_fn: Callable[[], dict],
    prune_desktop_run_manifests_fn: Callable[..., list[Path]],
    write_desktop_run_rollups_fn: Callable[..., None],
    desktop_cleanup_empty_line_fn: Callable[[], str],
    desktop_cleanup_lines_fn: Callable[[list[Path]], list[str]],
    remove_tree_fn: Callable[..., None] = shutil.rmtree,
    print_fn: Callable[[str], None] = print,
) -> int:
    config, status = load_desktop_command_config(load_config_fn=load_config_fn, print_fn=print_fn)
    if status is not None:
        return status

    older_than = args.older_than_days if args.older_than_days is not None else config["desktop_automation"]["retention_days"]
    paths = prune_desktop_run_manifests_fn(
        config,
        target_name=args.target,
        older_than_days=older_than,
        keep_last=args.keep_last,
    )
    if not paths:
        print_fn(desktop_cleanup_empty_line_fn())
        return 0

    for path in paths:
        remove_tree_fn(path, ignore_errors=False)

    write_desktop_run_rollups_fn(config, target_name=args.target if args.target else None)
    if args.target is not None:
        write_desktop_run_rollups_fn(config)

    return emit_desktop_command_result(
        payload={"removed": [str(path) for path in paths]},
        json_output=getattr(args, "json", False),
        text_lines=desktop_cleanup_lines_fn(paths),
        print_fn=print_fn,
    )

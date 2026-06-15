"""Desktop automation proof-report command orchestration."""

from __future__ import annotations

import argparse
from collections.abc import Callable

from desktop_command_flow import (
    emit_desktop_command_result,
    load_desktop_command_config,
    run_desktop_command_step,
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


__all__ = ["cmd_desktop_proof"]

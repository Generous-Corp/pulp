"""Submission metadata printing helpers for local CI target preflight."""

from __future__ import annotations

from collections.abc import Callable


def print_submission_metadata(
    metadata: dict,
    *,
    short_sha_fn: Callable[[str], str],
    provenance_summary_fn: Callable[[dict | None], str],
    print_fn: Callable[..., None],
) -> None:
    print_fn(
        "Submitting: "
        f"{metadata['branch']} @ {short_sha_fn(metadata['sha'])} "
        f"priority={metadata['priority']} targets={','.join(metadata['targets']) or 'none'}"
    )
    print_fn(f"  root: {metadata['submitted_root']}")
    print_fn(f"  cwd: {metadata['cwd']}")
    if metadata.get("cwd_git_root"):
        print_fn(f"  cwd git root: {metadata['cwd_git_root']}")
    print_fn(f"  config: {metadata['config_path']} ({metadata['config_source']})")
    if metadata.get("provenance"):
        print_fn(f"  provenance: {provenance_summary_fn(metadata.get('provenance'))}")
    for drift in metadata.get("config_drift", []):
        print_fn(f"  config drift: {drift}")
    for target_name in metadata.get("targets", []):
        state = metadata.get("target_hosts", {}).get(target_name, {})
        transport = state.get("transport_mode", "local")
        if transport == "local":
            print_fn(f"  {target_name}: local transport")
            continue
        resolved = state.get("resolved_host") or state.get("configured_host") or "?"
        status = state.get("status", "unknown")
        repo_path = state.get("repo_path") or "?"
        print_fn(f"  {target_name}: host={resolved} status={status} transport={transport} repo={repo_path}")
    for warning in metadata.get("warnings", []):
        print_fn(f"  warning: {warning}")

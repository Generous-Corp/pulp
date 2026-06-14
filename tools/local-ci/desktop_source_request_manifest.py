"""Desktop source context manifest attachment helpers."""

from __future__ import annotations


def attach_desktop_source_to_manifest(manifest: dict, source_context: dict | None) -> None:
    if not source_context:
        return
    source_manifest = {
        "mode": source_context.get("mode", "live"),
        "branch": source_context.get("branch"),
        "sha": source_context.get("sha"),
        "prepare_command": source_context.get("prepare_command"),
        "prepare_timeout_secs": source_context.get("prepare_timeout_secs"),
        "prepared_root": source_context.get("prepared_root_display", source_context.get("prepared_root")),
        "launch_cwd": source_context.get("launch_cwd_display", source_context.get("launch_cwd")),
    }
    manifest["source"] = source_manifest
    prepare_log = source_context.get("prepare_log")
    if prepare_log:
        manifest.setdefault("artifacts", {})["prepare_log"] = str(prepare_log)

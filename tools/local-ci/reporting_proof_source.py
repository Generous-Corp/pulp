"""Desktop automation proof source normalization."""

from __future__ import annotations


def normalize_desktop_proof_source_mode(mode: str | None) -> str:
    value = (mode or "legacy").strip().lower().replace("_", "-")
    if value not in {"live", "exact-sha", "legacy"}:
        raise ValueError(f"Invalid desktop proof source mode '{mode}'. Use one of: live, exact-sha, legacy.")
    return value


def desktop_manifest_source(manifest: dict) -> dict:
    raw = manifest.get("source")
    if not isinstance(raw, dict):
        return {
            "mode": "legacy",
            "branch": None,
            "sha": None,
            "prepare_command": None,
            "prepare_timeout_secs": None,
            "prepared_root": None,
            "launch_cwd": None,
        }
    mode = raw.get("mode")
    try:
        normalized_mode = normalize_desktop_proof_source_mode(mode)
    except ValueError:
        normalized_mode = "legacy"
    return {
        "mode": normalized_mode,
        "branch": raw.get("branch"),
        "sha": raw.get("sha"),
        "prepare_command": raw.get("prepare_command"),
        "prepare_timeout_secs": raw.get("prepare_timeout_secs"),
        "prepared_root": raw.get("prepared_root"),
        "launch_cwd": raw.get("launch_cwd"),
    }


__all__ = ["desktop_manifest_source", "normalize_desktop_proof_source_mode"]

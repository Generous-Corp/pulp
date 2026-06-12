"""Desktop automation proof and run-summary helpers for local CI."""

from __future__ import annotations

from typing import Callable


def normalize_desktop_proof_source_mode(mode: str | None) -> str:
    value = (mode or "legacy").strip().lower().replace("_", "-")
    if value not in {"live", "exact-sha", "legacy"}:
        raise ValueError(f"Invalid desktop proof source mode '{mode}'. Use one of: live, exact-sha, legacy.")
    return value


def desktop_manifest_adapter(config: dict, manifest: dict) -> str:
    adapter = str(manifest.get("adapter") or "").strip()
    if adapter:
        return adapter
    target_name = manifest.get("target")
    targets = config.get("desktop_automation", {}).get("targets", {})
    target_cfg = targets.get(target_name) if isinstance(targets, dict) else None
    if isinstance(target_cfg, dict):
        return str(target_cfg.get("adapter") or "unknown")
    return "unknown"


def desktop_manifest_run_status(manifest: dict) -> str:
    for key in ("agent_status", "status"):
        value = str(manifest.get(key) or "").strip()
        if value:
            return value.lower()
    return "pass"


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


def desktop_proof_scope_for_adapter(adapter: str) -> str:
    if adapter in {"linux-xvfb", "windows-session-agent"}:
        return "live-host"
    if adapter == "macos-local":
        return "local-session"
    return "unknown"


def desktop_run_summary(config: dict, manifest: dict) -> dict:
    artifacts = manifest.get("artifacts", {})
    source = desktop_manifest_source(manifest)
    adapter = desktop_manifest_adapter(config, manifest)
    return {
        "target": manifest.get("target"),
        "action": manifest.get("action", "run"),
        "label": manifest.get("label", manifest.get("action", "run")),
        "adapter": adapter,
        "proof_scope": desktop_proof_scope_for_adapter(adapter),
        "run_status": desktop_manifest_run_status(manifest),
        "completed_at": manifest.get("completed_at") or manifest.get("started_at") or "?",
        "interaction_mode": (manifest.get("interaction") or {}).get("mode"),
        "host": manifest.get("host"),
        "source": source,
        "artifacts": {
            "bundle_dir": artifacts.get("bundle_dir"),
            "screenshot": artifacts.get("screenshot"),
            "before_screenshot": artifacts.get("before_screenshot"),
            "diff_screenshot": artifacts.get("diff_screenshot"),
            "ui_snapshot": artifacts.get("ui_snapshot"),
            "stdout": artifacts.get("stdout"),
            "stderr": artifacts.get("stderr"),
            "agent_manifest": artifacts.get("agent_manifest"),
            "image_change": artifacts.get("image_change"),
        },
    }


def desktop_proof_summaries(
    config: dict,
    *,
    target_name: str | None = None,
    action: str | None = None,
    source_mode: str | None = None,
    sha: str | None = None,
    branch: str | None = None,
    limit: int | None = None,
    desktop_run_manifests_fn: Callable[..., list[dict]],
    desktop_run_summary_fn: Callable[[dict, dict], dict],
) -> list[dict]:
    manifests = desktop_run_manifests_fn(config, target_name=target_name, action=action)
    summaries: dict[tuple[str | None, str, str, str | None], dict] = {}
    requested_mode = normalize_desktop_proof_source_mode(source_mode) if source_mode else None
    for manifest in manifests:
        run_summary = desktop_run_summary_fn(config, manifest)
        if run_summary["run_status"] != "pass":
            continue
        source = run_summary["source"]
        if requested_mode and source["mode"] != requested_mode:
            continue
        if sha and source.get("sha") != sha:
            continue
        if branch and source.get("branch") != branch:
            continue
        key = (
            run_summary.get("target"),
            run_summary.get("action"),
            source.get("mode", "legacy"),
            source.get("sha"),
        )
        existing = summaries.get(key)
        if existing is None:
            summaries[key] = {
                "key": {
                    "target": run_summary.get("target"),
                    "action": run_summary.get("action"),
                    "source_mode": source.get("mode", "legacy"),
                    "sha": source.get("sha"),
                },
                "target": run_summary.get("target"),
                "action": run_summary.get("action"),
                "adapter": run_summary.get("adapter"),
                "proof_scope": run_summary.get("proof_scope"),
                "host": run_summary.get("host"),
                "source": source,
                "interaction_mode": run_summary.get("interaction_mode"),
                "run_count": 1,
                "latest_run": run_summary,
            }
            continue
        existing["run_count"] += 1
    ordered = sorted(
        summaries.values(),
        key=lambda item: item.get("latest_run", {}).get("completed_at") or "",
        reverse=True,
    )
    if limit is not None:
        ordered = ordered[:limit]
    return ordered

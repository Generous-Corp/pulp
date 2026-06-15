"""Desktop automation run-summary helpers."""

from __future__ import annotations

from reporting_proof_source import desktop_manifest_source


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


__all__ = [
    "desktop_manifest_adapter",
    "desktop_manifest_run_status",
    "desktop_proof_scope_for_adapter",
    "desktop_run_summary",
]

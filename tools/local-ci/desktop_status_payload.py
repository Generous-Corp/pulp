"""Desktop automation status payload assembly."""

from __future__ import annotations

from collections.abc import Callable


def desktop_status_target_names(*, targets: dict, target_name: str | None) -> list[str]:
    if target_name:
        if target_name not in targets:
            raise ValueError(f"unknown desktop target `{target_name}`")
        return [target_name]
    return sorted(targets)


def desktop_latest_run_payload(latest_run: dict | None) -> dict | None:
    if not latest_run:
        return None
    return {
        "label": latest_run["label"],
        "completed_at": latest_run["completed_at"],
        "interaction_mode": latest_run["interaction_mode"],
        "run_status": latest_run["run_status"],
        "source_mode": latest_run["source"]["mode"],
        "source_branch": latest_run["source"]["branch"],
        "source_sha": latest_run["source"]["sha"],
        "proof_scope": latest_run["proof_scope"],
        "host": latest_run["host"],
        "screenshot": latest_run["artifacts"]["screenshot"],
        "before_screenshot": latest_run["artifacts"]["before_screenshot"],
        "diff_screenshot": latest_run["artifacts"]["diff_screenshot"],
        "image_change": latest_run["artifacts"]["image_change"],
        "ui_snapshot": latest_run["artifacts"]["ui_snapshot"],
        "bundle_dir": latest_run["artifacts"]["bundle_dir"],
    }


def desktop_status_target_payload(
    config: dict,
    name: str,
    target: dict,
    *,
    desktop_receipt_for_fn: Callable[[str], dict],
    desktop_capabilities_for_fn: Callable[[str, str, dict | None], list[str]],
    desktop_optional_capabilities_fn: Callable[[dict | None], list[str]],
    desktop_run_manifests_fn: Callable[..., list[dict]],
    desktop_run_summary_fn: Callable[[dict, dict], dict],
    desktop_proof_summaries_fn: Callable[..., list[dict]],
    normalize_desktop_optional_config_fn: Callable[[dict | None], dict],
    desktop_target_contract_fn: Callable[[str, dict], dict],
) -> dict:
    receipt = desktop_receipt_for_fn(name)
    capabilities = desktop_capabilities_for_fn(target["adapter"], target["capability_tier"], target.get("optional"))
    latest = desktop_run_manifests_fn(config, target_name=name)[:1]
    latest_manifest = latest[0] if latest else None
    latest_run = desktop_run_summary_fn(config, latest_manifest) if latest_manifest else None
    latest_proof_matches = desktop_proof_summaries_fn(config, target_name=name, limit=1)
    latest_proof = latest_proof_matches[0] if latest_proof_matches else None

    return {
        "name": name,
        "enabled": target["enabled"],
        "adapter": target["adapter"],
        "bootstrap": target["bootstrap"],
        "type": target["target_type"],
        "host": target.get("host"),
        "repo_path": target.get("repo_path"),
        "capability_tier": target["capability_tier"],
        "capabilities": capabilities,
        "capabilities_text": ", ".join(capabilities),
        "optional_features": normalize_desktop_optional_config_fn(target.get("optional")),
        "optional_capabilities": desktop_optional_capabilities_fn(target.get("optional")),
        "installed": bool(receipt),
        "installed_at": receipt.get("installed_at", "?") if receipt else None,
        "contract": receipt.get("contract") if receipt else desktop_target_contract_fn(name, target),
        "remote_bootstrap_ready": receipt.get("remote_bootstrap_ready") if receipt else None,
        "remote_tooling_ready": receipt.get("remote_tooling_ready") if receipt else None,
        "remote_repo_checkout_ready": receipt.get("remote_repo_checkout_ready") if receipt else None,
        "tooling_probe": receipt.get("tooling_probe") if receipt else None,
        "repo_checkout_probe": receipt.get("repo_checkout_probe") if receipt else None,
        "latest_run": desktop_latest_run_payload(latest_run),
        "latest_proof": latest_proof,
    }


def desktop_status_payload(
    config: dict,
    *,
    target_name: str | None,
    desktop_receipt_for_fn: Callable[[str], dict],
    desktop_capabilities_for_fn: Callable[[str, str, dict | None], list[str]],
    desktop_optional_capabilities_fn: Callable[[dict | None], list[str]],
    desktop_run_manifests_fn: Callable[..., list[dict]],
    desktop_run_summary_fn: Callable[[dict, dict], dict],
    desktop_proof_summaries_fn: Callable[..., list[dict]],
    normalize_desktop_optional_config_fn: Callable[[dict | None], dict],
    desktop_target_contract_fn: Callable[[str, dict], dict],
    desktop_publish_reports_fn: Callable[..., list[dict]],
) -> dict:
    desktop_cfg = config["desktop_automation"]
    targets = desktop_cfg.get("targets", {})
    target_names = desktop_status_target_names(targets=targets, target_name=target_name)
    target_payloads = [
        desktop_status_target_payload(
            config,
            name,
            targets[name],
            desktop_receipt_for_fn=desktop_receipt_for_fn,
            desktop_capabilities_for_fn=desktop_capabilities_for_fn,
            desktop_optional_capabilities_fn=desktop_optional_capabilities_fn,
            desktop_run_manifests_fn=desktop_run_manifests_fn,
            desktop_run_summary_fn=desktop_run_summary_fn,
            desktop_proof_summaries_fn=desktop_proof_summaries_fn,
            normalize_desktop_optional_config_fn=normalize_desktop_optional_config_fn,
            desktop_target_contract_fn=desktop_target_contract_fn,
        )
        for name in target_names
    ]
    latest_publish_matches = desktop_publish_reports_fn(config, limit=1)
    return {
        "artifact_root": desktop_cfg["artifact_root"],
        "publish_mode": desktop_cfg["publish_mode"],
        "publish_branch": desktop_cfg["publish_branch"],
        "retention_days": desktop_cfg["retention_days"],
        "latest_publish": latest_publish_matches[0] if latest_publish_matches else None,
        "targets": target_payloads,
    }

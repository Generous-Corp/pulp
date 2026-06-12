"""Desktop automation CLI line helpers for local CI."""

from __future__ import annotations

import json

from desktop_reporting_lines import (
    desktop_cleanup_empty_line,
    desktop_cleanup_lines,
    desktop_proof_empty_line,
    desktop_proof_lines,
    desktop_publish_lines,
    desktop_recent_lines,
)


def _append_image_change_lines(lines: list[str], image_change: dict) -> None:
    lines.append(f"  image_change: changed={image_change.get('changed')} method={image_change.get('method')}")
    bbox = image_change.get("bbox")
    if bbox:
        lines.append(f"  image_change_bbox: {bbox['left']},{bbox['top']} -> {bbox['right']},{bbox['bottom']}")


def desktop_action_success_lines(action: str, target_name: str, manifest: dict) -> list[str]:
    lines = [
        f"Desktop {action} PASS for `{target_name}`",
        f"  label: {manifest['label']}",
        f"  pid: {manifest['pid']}",
    ]
    artifacts = manifest["artifacts"]
    if action in {"smoke", "click"}:
        if artifacts.get("before_screenshot"):
            lines.append(f"  before_screenshot: {artifacts['before_screenshot']}")
        if artifacts.get("diff_screenshot"):
            lines.append(f"  diff_screenshot: {artifacts['diff_screenshot']}")
        if artifacts.get("image_change"):
            _append_image_change_lines(lines, artifacts["image_change"])
    lines.append(f"  screenshot: {artifacts['screenshot']}")
    if artifacts.get("ui_snapshot"):
        lines.append(f"  ui_snapshot: {artifacts['ui_snapshot']}")
    if action in {"smoke", "click"} and manifest.get("interaction"):
        interaction = manifest["interaction"]
        if interaction.get("mode"):
            lines.append(f"  interaction_mode: {interaction['mode']}")
        click = interaction.get("click", {})
        screen_point = click.get("screen_point") or {}
        if "x" in screen_point and "y" in screen_point:
            lines.append(f"  click_screen_point: {screen_point.get('x')},{screen_point.get('y')}")
    lines.append(f"  bundle: {artifacts['bundle_dir']}")
    return lines


def desktop_config_show_lines(desktop_config: dict) -> list[str]:
    return [
        "Desktop automation config:",
        f"  artifact_root: {desktop_config['artifact_root']}",
        f"  publish_mode: {desktop_config['publish_mode']}",
        f"  publish_branch: {desktop_config['publish_branch']}",
        f"  retention_days: {desktop_config['retention_days']}",
        "  target optional keys: target.<name>.(webview_driver|webdriver_url|debug_attach|debugger_command|video_capture|frame_stats)",
    ]


def desktop_config_update_lines(payload: dict) -> list[str]:
    return [
        f"Desktop automation config updated: {payload['key']} = {payload['value']}",
        f"  config: {payload['config_path']}",
    ]


def desktop_status_lines(
    desktop_config: dict,
    target_payloads: list[dict],
    *,
    latest_publish: dict | None,
    short_sha_fn,
    windows_tooling_detail_fn,
    windows_repo_checkout_detail_fn,
) -> list[str]:
    lines = [
        "Desktop automation:",
        f"  artifact_root: {desktop_config['artifact_root']}",
        f"  publish_mode: {desktop_config['publish_mode']}",
        f"  publish_branch: {desktop_config['publish_branch']}",
        f"  retention_days: {desktop_config['retention_days']}",
    ]
    if latest_publish:
        lines.append(f"  latest_publish: {latest_publish.get('label') or '?'} @ {latest_publish.get('generated_at') or '?'}")
        if latest_publish.get("output_dir"):
            lines.append(f"  latest_publish_dir: {latest_publish['output_dir']}")
        if latest_publish.get("index_html"):
            lines.append(f"  latest_publish_html: {latest_publish['index_html']}")
    lines.extend(["", "Targets:"])

    for target_info in target_payloads:
        lines.append(f"  {target_info['name']}:")
        lines.append(f"    enabled: {target_info['enabled']}")
        lines.append(f"    adapter: {target_info['adapter']}")
        lines.append(f"    bootstrap: {target_info['bootstrap']}")
        lines.append(f"    type: {target_info['type']}")
        if target_info.get("host"):
            lines.append(f"    host: {target_info['host']}")
        if target_info.get("repo_path"):
            lines.append(f"    repo_path: {target_info['repo_path']}")
        lines.append(f"    capability_tier: {target_info['capability_tier']}")
        lines.append(f"    capabilities: {target_info['capabilities_text']}")
        if target_info.get("optional_capabilities"):
            lines.append(f"    optional_capabilities: {', '.join(target_info['optional_capabilities'])}")
        optional_features = target_info.get("optional_features") or {}
        if any(optional_features.values()):
            lines.append(f"    optional_features: {json.dumps(optional_features, sort_keys=True)}")
        lines.append(f"    installed: {'yes' if target_info['installed'] else 'no'}")
        if target_info["installed_at"]:
            lines.append(f"    installed_at: {target_info['installed_at']}")
        if target_info.get("remote_bootstrap_ready") is not None:
            lines.append(f"    remote_bootstrap_ready: {target_info['remote_bootstrap_ready']}")
        if target_info.get("remote_tooling_ready") is not None:
            lines.append(f"    remote_tooling_ready: {target_info['remote_tooling_ready']}")
        if target_info.get("remote_repo_checkout_ready") is not None:
            lines.append(f"    remote_repo_checkout_ready: {target_info['remote_repo_checkout_ready']}")
        contract = target_info.get("contract") or {}
        if contract.get("task_name"):
            lines.append(f"    task_name: {contract['task_name']}")
        if contract.get("remote_root"):
            lines.append(f"    remote_root: {contract['remote_root']}")
        tooling_probe = target_info.get("tooling_probe") or {}
        if tooling_probe.get("git_found"):
            lines.append(f"    remote_git: {windows_tooling_detail_fn(tooling_probe, 'git')}")
        elif target_info.get("remote_tooling_ready") is not None:
            lines.append("    remote_git: missing")
        if tooling_probe.get("gh_found"):
            lines.append(f"    remote_gh: {windows_tooling_detail_fn(tooling_probe, 'gh')}")
        repo_checkout_probe = target_info.get("repo_checkout_probe") or {}
        if repo_checkout_probe.get("repo_path"):
            lines.append(
                "    remote_repo_checkout: "
                f"{windows_repo_checkout_detail_fn(repo_checkout_probe, fallback_path=target_info.get('repo_path'))}"
            )
        latest_run = target_info.get("latest_run")
        if latest_run:
            lines.append(f"    latest_run: {latest_run['label']} @ {latest_run['completed_at']}")
            lines.append(f"    latest_run_status: {latest_run['run_status']}")
            lines.append(
                f"    latest_run_source: mode={latest_run['source_mode']} "
                f"sha={short_sha_fn(latest_run['source_sha'])} branch={latest_run['source_branch'] or '?'}"
            )
            if latest_run.get("host"):
                lines.append(f"    latest_run_host: {latest_run['host']}")
            if latest_run.get("proof_scope") and latest_run["proof_scope"] != "unknown":
                lines.append(f"    latest_run_proof_scope: {latest_run['proof_scope']}")
            interaction_mode = latest_run.get("interaction_mode")
            if interaction_mode:
                lines.append(f"    latest_interaction_mode: {interaction_mode}")
            before_screenshot = latest_run.get("before_screenshot")
            if before_screenshot:
                lines.append(f"    latest_before_screenshot: {before_screenshot}")
            diff_screenshot = latest_run.get("diff_screenshot")
            if diff_screenshot:
                lines.append(f"    latest_diff_screenshot: {diff_screenshot}")
            image_change = latest_run.get("image_change")
            if image_change:
                lines.append(f"    latest_image_change: changed={image_change.get('changed')} method={image_change.get('method')}")
            screenshot = latest_run.get("screenshot")
            if screenshot:
                lines.append(f"    latest_screenshot: {screenshot}")
            ui_snapshot = latest_run.get("ui_snapshot")
            if ui_snapshot:
                lines.append(f"    latest_ui_snapshot: {ui_snapshot}")
            bundle_dir = latest_run.get("bundle_dir")
            if bundle_dir:
                lines.append(f"    latest_bundle: {bundle_dir}")
        latest_proof = target_info.get("latest_proof")
        if latest_proof:
            latest_proof_run = latest_proof["latest_run"]
            lines.append(
                "    latest_proof: "
                f"{latest_proof['action']} mode={latest_proof['source']['mode']} "
                f"sha={short_sha_fn(latest_proof['source']['sha'])} @ {latest_proof_run['completed_at']}"
            )
            if latest_proof.get("proof_scope") and latest_proof["proof_scope"] != "unknown":
                host_detail = f" host={latest_proof['host']}" if latest_proof.get("host") else ""
                lines.append(
                    f"    latest_proof_scope: {latest_proof['proof_scope']}{host_detail} "
                    f"runs={latest_proof['run_count']}"
                )
            proof_bundle = latest_proof_run.get("artifacts", {}).get("bundle_dir")
            if proof_bundle:
                lines.append(f"    latest_proof_bundle: {proof_bundle}")
    return lines


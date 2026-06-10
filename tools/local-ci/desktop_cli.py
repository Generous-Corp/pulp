"""Desktop automation CLI line helpers for local CI."""

from __future__ import annotations


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


def desktop_recent_lines(run_summaries: list[dict], *, short_sha_fn) -> list[str]:
    lines = ["Desktop automation recent runs:"]
    for run_summary in run_summaries:
        action = run_summary.get("action", "run")
        target = run_summary.get("target", "?")
        label = run_summary.get("label", action)
        completed = run_summary.get("completed_at") or "?"
        artifacts = run_summary.get("artifacts", {})
        bundle_dir = artifacts.get("bundle_dir", "?")
        lines.append(f"  {target}/{action}: {label} @ {completed}")
        lines.append(f"    status: {run_summary['run_status']}")
        source = run_summary["source"]
        lines.append(f"    source: mode={source['mode']} sha={short_sha_fn(source['sha'])} branch={source['branch'] or '?'}")
        if run_summary.get("proof_scope") and run_summary["proof_scope"] != "unknown":
            host_detail = f" host={run_summary['host']}" if run_summary.get("host") else ""
            lines.append(f"    proof_scope: {run_summary['proof_scope']}{host_detail}")
        lines.append(f"    bundle: {bundle_dir}")
        before_screenshot = artifacts.get("before_screenshot")
        if before_screenshot:
            lines.append(f"    before_screenshot: {before_screenshot}")
        diff_screenshot = artifacts.get("diff_screenshot")
        if diff_screenshot:
            lines.append(f"    diff_screenshot: {diff_screenshot}")
        interaction_mode = run_summary.get("interaction_mode")
        if interaction_mode:
            lines.append(f"    interaction_mode: {interaction_mode}")
        image_change = artifacts.get("image_change")
        if image_change:
            lines.append(f"    image_change: changed={image_change.get('changed')} method={image_change.get('method')}")
        screenshot = artifacts.get("screenshot")
        if screenshot:
            lines.append(f"    screenshot: {screenshot}")
        ui_snapshot = artifacts.get("ui_snapshot")
        if ui_snapshot:
            lines.append(f"    ui_snapshot: {ui_snapshot}")
    return lines


def desktop_publish_lines(report: dict) -> list[str]:
    return [
        "Desktop publish report ready:",
        f"  runs: {report['run_count']}",
        f"  output_dir: {report['output_dir']}",
        f"  index_html: {report['index_html']}",
        f"  index_json: {report['index_json']}",
    ]


def desktop_cleanup_empty_line() -> str:
    return "Desktop cleanup: nothing to remove."


def desktop_cleanup_lines(paths: list) -> list[str]:
    lines = [f"Desktop cleanup removed {len(paths)} bundle(s)."]
    lines.extend(f"  {path}" for path in paths[:10])
    return lines


def desktop_proof_empty_line(
    *,
    target: str | None,
    action: str | None,
    source_mode: str | None,
    sha: str | None,
    branch: str | None,
    short_sha_fn,
) -> str:
    filters = []
    if target:
        filters.append(f"target={target}")
    if action:
        filters.append(f"action={action}")
    if source_mode:
        filters.append(f"source_mode={source_mode}")
    if sha:
        filters.append(f"sha={short_sha_fn(sha)}")
    if branch:
        filters.append(f"branch={branch}")
    suffix = f" ({', '.join(filters)})" if filters else ""
    return f"No desktop proofs found{suffix}."


def desktop_proof_lines(proofs: list[dict], *, short_sha_fn) -> list[str]:
    lines = ["Desktop automation proofs:"]
    for proof in proofs:
        latest_run = proof["latest_run"]
        source = proof["source"]
        lines.append(
            f"  {proof['target']}/{proof['action']}: mode={source['mode']} "
            f"sha={short_sha_fn(source['sha'])} @ {latest_run['completed_at']}"
        )
        host_detail = f" host={proof['host']}" if proof.get("host") else ""
        lines.append(
            f"    proof_scope: {proof['proof_scope']} adapter={proof['adapter']}{host_detail} "
            f"runs={proof['run_count']}"
        )
        if source.get("branch"):
            lines.append(f"    branch: {source['branch']}")
        if latest_run.get("label"):
            lines.append(f"    label: {latest_run['label']}")
        if latest_run.get("interaction_mode"):
            lines.append(f"    interaction_mode: {latest_run['interaction_mode']}")
        bundle_dir = latest_run.get("artifacts", {}).get("bundle_dir")
        if bundle_dir:
            lines.append(f"    bundle: {bundle_dir}")
        screenshot = latest_run.get("artifacts", {}).get("screenshot")
        if screenshot:
            lines.append(f"    screenshot: {screenshot}")
        ui_snapshot = latest_run.get("artifacts", {}).get("ui_snapshot")
        if ui_snapshot:
            lines.append(f"    ui_snapshot: {ui_snapshot}")
        agent_manifest = latest_run.get("artifacts", {}).get("agent_manifest")
        if agent_manifest:
            lines.append(f"    agent_manifest: {agent_manifest}")
    return lines

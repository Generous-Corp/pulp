"""Argument parser construction for pulp ci-local."""

from __future__ import annotations

import argparse
from collections.abc import Iterable


def build_local_ci_parser(
    *,
    priority_values: Iterable[str],
    keep_completed_jobs: int,
    epilog: str | None = None,
) -> argparse.ArgumentParser:
    priority_choices = sorted(priority_values)
    parser = argparse.ArgumentParser(
        description="Local CI runner for Pulp",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=epilog,
    )
    sub = parser.add_subparsers(dest="command")

    def add_submission_args(
        command_parser: argparse.ArgumentParser,
        *,
        include_sha: bool = False,
        allow_smoke: bool = False,
    ) -> None:
        command_parser.add_argument("branch", nargs="?", help="Branch name (default: current)")
        command_parser.add_argument(
            "--priority",
            choices=priority_choices,
            help="Queue priority (default from config; ship/check default to high)",
        )
        command_parser.add_argument(
            "--targets",
            help="Comma-separated target list (for example: mac or mac,ubuntu)",
        )
        command_parser.add_argument(
            "--allow-root-mismatch",
            action="store_true",
            help="Queue even if the current cwd belongs to a different git root than this local_ci.py checkout",
        )
        command_parser.add_argument(
            "--allow-unreachable-targets",
            action="store_true",
            help="Queue even if preflight finds a selected SSH target currently unreachable with no fallback",
        )
        if include_sha:
            command_parser.add_argument("--sha", help="Exact commit SHA to validate (default: current HEAD)")
        if allow_smoke:
            command_parser.add_argument(
                "--smoke",
                action="store_true",
                help="Run the fast clean install/export preflight instead of full validation",
            )

    def add_desktop_source_args(command_parser: argparse.ArgumentParser) -> None:
        command_parser.add_argument(
            "--source-mode",
            choices=["live", "exact-sha"],
            default="live",
            help="Launch from the live checkout (default) or from an exact-SHA prepared source root.",
        )
        command_parser.add_argument("--branch", help="Branch label to record for desktop source provenance (default: current branch)")
        command_parser.add_argument("--sha", help="Exact commit SHA to prepare for desktop source mode (default: current HEAD)")
        command_parser.add_argument("--prepare-command", help="Optional shell command to run in the prepared source root before launch")
        command_parser.add_argument(
            "--prepare-timeout",
            type=float,
            default=900.0,
            help="Seconds to allow the optional prepare command to run (default: 900)",
        )

    def add_desktop_video_args(command_parser: argparse.ArgumentParser) -> None:
        command_parser.add_argument(
            "--run-in-terminal",
            action="store_true",
            help="On macOS, re-run this local-ci command inside Terminal.app so Screen Recording permission follows Terminal's TCC grant.",
        )
        command_parser.add_argument(
            "--record-video",
            action="store_true",
            help="Record a short window-region MP4 proof while the desktop action runs.",
        )
        command_parser.add_argument(
            "--duration",
            "--video-duration",
            dest="video_duration",
            type=float,
            default=8.0,
            help="Seconds of video to record when --record-video is set (default: 8)",
        )
        command_parser.add_argument(
            "--video-fps",
            type=float,
            default=30.0,
            help="Frames per second for --record-video captures (default: 30)",
        )
        command_parser.add_argument(
            "--video-attachment-budget-mb",
            type=float,
            default=100.0,
            help="Attachment budget in decimal MB for issue-ready video metadata (default: 100)",
        )
        command_parser.add_argument(
            "--compose-video-proof",
            action="store_true",
            help="Use Remotion to render an annotated proof video from the raw recording.",
        )
        command_parser.add_argument(
            "--video-template",
            choices=["validation-proof", "design-parity"],
            help="Remotion proof template for direct --record-video composition.",
        )
        command_parser.add_argument("--source-image", help="Optional source/reference image for design-parity proof composition")
        command_parser.add_argument("--source-label", help="Label for --source-image (default: Source reference)")
        command_parser.add_argument("--video-title", help="Override the composed proof video title")
        command_parser.add_argument(
            "--video-audio",
            choices=["none", "plugin", "system"],
            default="none",
            help="Audio source for the proof video. Only 'none' is implemented in the MVP; plugin/system are reserved.",
        )

    p_enqueue = sub.add_parser("enqueue", help="Queue a branch for validation")
    add_submission_args(p_enqueue, include_sha=True, allow_smoke=True)

    sub.add_parser("drain", help="Process pending jobs if no other runner is active")

    p_run = sub.add_parser("run", help="Queue validation and wait for completion")
    add_submission_args(p_run, include_sha=True, allow_smoke=True)

    p_ship = sub.add_parser("ship", help="PR -> queued CI -> merge on green")
    add_submission_args(p_ship, include_sha=True)
    p_ship.add_argument("--base", default="main", help="Base branch (default: main)")

    p_check = sub.add_parser("check", help="Validate an existing PR")
    p_check.add_argument("pr", help="PR number, GitHub URL, or 'latest'")
    p_check.add_argument(
        "--priority",
        choices=priority_choices,
        help="Queue priority (default: high)",
    )
    p_check.add_argument(
        "--targets",
        help="Comma-separated target list (for example: mac or mac,ubuntu)",
    )
    p_check.add_argument(
        "--allow-root-mismatch",
        action="store_true",
        help="Queue even if the current cwd belongs to a different git root than this local_ci.py checkout",
    )
    p_check.add_argument(
        "--allow-unreachable-targets",
        action="store_true",
        help="Queue even if preflight finds a selected SSH target currently unreachable with no fallback",
    )
    p_check.add_argument(
        "--smoke",
        action="store_true",
        help="Run the fast clean install/export preflight instead of full validation",
    )

    p_cloud = sub.add_parser("cloud", help="Operate GitHub Actions workflows through the local CI control plane")
    cloud_sub = p_cloud.add_subparsers(dest="cloud_command")

    cloud_sub.add_parser("workflows", help="List supported GitHub workflows and providers")
    cloud_sub.add_parser("defaults", help="Show effective cloud workflow/provider defaults")

    p_cloud_history = cloud_sub.add_parser("history", help="Show recent tracked cloud run history")
    p_cloud_history.add_argument(
        "--workflow",
        help="Optional workflow key filter (for example: build or docs-check)",
    )
    p_cloud_history.add_argument(
        "--provider",
        help="Optional provider filter (for example: github-hosted or namespace)",
    )
    p_cloud_history.add_argument(
        "--limit",
        type=int,
        default=10,
        help="Runs to show (default: 10)",
    )

    p_cloud_compare = cloud_sub.add_parser("compare", help="Compare observed cloud providers for a workflow")
    p_cloud_compare.add_argument(
        "workflow",
        nargs="?",
        help="Workflow key (default: configured cloud default workflow)",
    )

    p_cloud_recommend = cloud_sub.add_parser("recommend", help="Recommend a cloud provider from recorded history")
    p_cloud_recommend.add_argument(
        "workflow",
        nargs="?",
        help="Workflow key (default: configured cloud default workflow)",
    )

    p_cloud_run = cloud_sub.add_parser("run", help="Dispatch a GitHub Actions workflow")
    p_cloud_run.add_argument(
        "workflow",
        nargs="?",
        help="Workflow key (for example: build, validate, docs-check)",
    )
    p_cloud_run.add_argument("branch", nargs="?", help="Branch name (default: current)")
    p_cloud_run.add_argument(
        "--provider",
        help="Runner provider (for example: github-hosted or namespace)",
    )
    p_cloud_run.add_argument(
        "--runner-selector-json",
        help="Optional JSON string/array passed through to the workflow runs-on selector",
    )
    p_cloud_run.add_argument(
        "--linux-runner-selector-json",
        help="Optional JSON string/array override for the Linux build leg runs-on selector",
    )
    p_cloud_run.add_argument(
        "--windows-runner-selector-json",
        help="Optional JSON string/array override for the Windows build leg runs-on selector",
    )
    p_cloud_run.add_argument(
        "--macos-runner-selector-json",
        help="Optional JSON string/array override for the macOS build leg runs-on selector",
    )
    p_cloud_run.add_argument(
        "--wait",
        action="store_true",
        help="Block until the matched GitHub run completes",
    )

    p_cloud_status = cloud_sub.add_parser("status", help="Show tracked GitHub workflow state")
    p_cloud_status.add_argument(
        "identifier",
        nargs="?",
        help="Dispatch id, GitHub run id, or 'latest' (default: list recent tracked runs)",
    )
    p_cloud_status.add_argument(
        "--refresh",
        action="store_true",
        help="Refresh the selected matched run from GitHub before rendering",
    )
    p_cloud_status.add_argument(
        "--limit",
        type=int,
        default=5,
        help="Runs to show when listing recent tracked runs (default: 5)",
    )

    p_cloud_namespace = cloud_sub.add_parser(
        "namespace",
        help="Check Namespace CLI/login/workspace setup without replacing the upstream nsc tool",
    )
    cloud_namespace_sub = p_cloud_namespace.add_subparsers(dest="cloud_namespace_command")
    cloud_namespace_sub.add_parser("doctor", help="Show Namespace CLI, login, and workspace status")
    cloud_namespace_sub.add_parser("setup", help="Run the thin Namespace setup flow (`nsc login` if needed)")

    sub.add_parser("list", help="Show open PRs")

    p_bump = sub.add_parser("bump", help="Reprioritize a pending job")
    p_bump.add_argument("job", help="Job id, unique id prefix, or exact branch name")
    p_bump.add_argument("priority", choices=priority_choices, help="New priority")

    p_cancel = sub.add_parser("cancel", help="Cancel a pending job")
    p_cancel.add_argument("job", help="Job id, unique id prefix, or exact branch name")

    p_logs = sub.add_parser("logs", help="Tail saved logs for a running or completed job")
    p_logs.add_argument("job", nargs="?", help="Job id, unique id prefix, or exact branch name (default: active/latest)")
    p_logs.add_argument("--target", help="Target name to show (mac, ubuntu, windows)")
    p_logs.add_argument("--lines", type=int, default=80, help="Number of log lines to show (default: 80)")

    p_cleanup = sub.add_parser("cleanup", help="Inspect or prune retained local CI artifacts")
    cleanup_mode = p_cleanup.add_mutually_exclusive_group()
    cleanup_mode.add_argument(
        "--dry-run",
        action="store_true",
        help="Show the cleanup plan without deleting anything (default)",
    )
    cleanup_mode.add_argument(
        "--apply",
        action="store_true",
        help="Delete the reported stale artifacts instead of only showing a dry-run plan",
    )
    p_cleanup.add_argument(
        "--include-prepared",
        action="store_true",
        help="Also include prepared build/install trees; later reruns will rebuild them",
    )
    p_cleanup.add_argument(
        "--keep-results",
        type=int,
        default=keep_completed_jobs,
        help=f"Keep this many orphaned result files outside retained queue history (default: {keep_completed_jobs})",
    )
    p_cleanup.add_argument(
        "--keep-logs",
        type=int,
        default=keep_completed_jobs,
        help=f"Keep this many orphaned log directories outside retained queue history (default: {keep_completed_jobs})",
    )
    p_cleanup.add_argument(
        "--keep-bundles",
        type=int,
        default=0,
        help="Keep this many non-live git bundles instead of deleting all completed-job bundles (default: 0)",
    )

    p_evidence = sub.add_parser("evidence", help="Show accumulated last-good target results by exact SHA")
    p_evidence.add_argument("branch", nargs="?", help="Branch name (default: current)")
    p_evidence.add_argument("--sha", help="Filter to one exact SHA")
    p_evidence.add_argument("--limit", type=int, default=5, help="Shas to show per validation mode (default: 5)")

    sub.add_parser("status", help="Show queue, runner, results, and VM status")

    p_desktop = sub.add_parser("desktop", help="Desktop automation setup, health, and status")
    desktop_sub = p_desktop.add_subparsers(dest="desktop_command")

    p_desktop_install = desktop_sub.add_parser("install", help="Prepare one desktop automation target")
    p_desktop_install.add_argument("target", help="Desktop target name (for example: mac, ubuntu, windows)")

    p_desktop_doctor = desktop_sub.add_parser("doctor", help="Run health checks for one desktop automation target")
    p_desktop_doctor.add_argument("target", help="Desktop target name (for example: mac, ubuntu, windows)")
    p_desktop_doctor.add_argument("--json", action="store_true", help="Emit machine-readable JSON")

    p_desktop_status = desktop_sub.add_parser("status", help="Show desktop automation config and target state")
    p_desktop_status.add_argument("target", nargs="?", help="Optional one-target filter")
    p_desktop_status.add_argument("--json", action="store_true", help="Emit machine-readable JSON")

    p_desktop_config = desktop_sub.add_parser("config", help="Show or update desktop automation config")
    desktop_config_sub = p_desktop_config.add_subparsers(dest="desktop_config_command")

    p_desktop_config_show = desktop_config_sub.add_parser("show", help="Show desktop automation config")
    p_desktop_config_show.add_argument("--json", action="store_true", help="Emit machine-readable JSON")

    p_desktop_config_set = desktop_config_sub.add_parser("set", help="Set a desktop automation config value")
    p_desktop_config_set.add_argument("key", help="Config key (artifact_root, publish_mode, publish_branch, retention_days, or target.<name>.<field>)")
    p_desktop_config_set.add_argument("value", help="New config value")
    p_desktop_config_set.add_argument("--json", action="store_true", help="Emit machine-readable JSON")

    p_desktop_recent = desktop_sub.add_parser("recent", help="Show recent desktop automation runs")
    p_desktop_recent.add_argument("target", nargs="?", help="Optional one-target filter")
    p_desktop_recent.add_argument("--action", help="Optional action filter (for example: smoke)")
    p_desktop_recent.add_argument("--limit", type=int, default=5, help="Number of runs to show (default: 5)")
    p_desktop_recent.add_argument("--json", action="store_true", help="Emit machine-readable JSON")

    p_desktop_proof = desktop_sub.add_parser("proof", help="Show successful desktop proofs grouped by target/action/source/SHA")
    p_desktop_proof.add_argument("target", nargs="?", help="Optional one-target filter")
    p_desktop_proof.add_argument("--action", help="Optional action filter (for example: inspect)")
    p_desktop_proof.add_argument(
        "--source-mode",
        choices=["live", "exact-sha", "legacy"],
        help="Optional source-mode filter for desktop proof summaries",
    )
    p_desktop_proof.add_argument("--sha", help="Optional exact full SHA filter")
    p_desktop_proof.add_argument("--branch", help="Optional branch filter")
    p_desktop_proof.add_argument("--limit", type=int, default=10, help="Number of proofs to show (default: 10)")
    p_desktop_proof.add_argument("--json", action="store_true", help="Emit machine-readable JSON")

    p_desktop_publish = desktop_sub.add_parser("publish", help="Stage a local HTML/JSON report for recent desktop automation runs")
    p_desktop_publish.add_argument("target", nargs="?", help="Optional one-target filter")
    p_desktop_publish.add_argument("--action", help="Optional action filter (for example: click)")
    p_desktop_publish.add_argument("--limit", type=int, default=5, help="Number of runs to include (default: 5)")
    p_desktop_publish.add_argument("--label", help="Optional report label")
    p_desktop_publish.add_argument("--output", help="Optional report output directory")
    p_desktop_publish.add_argument("--manifest", action="append", help="Explicit run manifest.json to publish; may be passed more than once")
    p_desktop_publish.add_argument("--json", action="store_true", help="Emit machine-readable JSON")

    p_desktop_verdict = desktop_sub.add_parser("verdict", help="Record a human review verdict on a desktop run manifest")
    p_desktop_verdict.add_argument("manifest", help="Path to the run manifest.json to update")
    verdict_group = p_desktop_verdict.add_mutually_exclusive_group(required=True)
    verdict_group.add_argument("--approved", action="store_true", help="Mark the proof as approved")
    verdict_group.add_argument("--needs-work", action="store_true", help="Mark the proof as needing work")
    p_desktop_verdict.add_argument("--notes", default="", help="Optional reviewer notes")
    p_desktop_verdict.add_argument("--reviewer", default="", help="Optional reviewer identity")
    p_desktop_verdict.add_argument("--issue-url", default="", help="Optional GitHub review issue URL")
    p_desktop_verdict.add_argument("--json", action="store_true", help="Emit machine-readable JSON")

    p_desktop_compose_video = desktop_sub.add_parser("compose-video", help="Render or rerender a Remotion proof for an existing desktop run manifest")
    p_desktop_compose_video.add_argument("manifest", help="Path to the run manifest.json to compose")
    p_desktop_compose_video.add_argument("--output", help="Output composed MP4 path (default: <run>/video/proof-composed.mp4)")
    p_desktop_compose_video.add_argument("--metadata", help="Output composed metadata JSON path (default: <run>/video/composed-metadata.json)")
    p_desktop_compose_video.add_argument("--issue-output", help="Output issue-ready MP4 path (default: <run>/video/proof.issue.mp4)")
    p_desktop_compose_video.add_argument("--issue-metadata", help="Output issue metadata JSON path (default: <run>/video/issue-metadata.json)")
    p_desktop_compose_video.add_argument("--small-video", action="store_true", help="Also create a <=10 MB convenience encode when possible")
    p_desktop_compose_video.add_argument("--small-output", help="Output small MP4 path (default: <run>/video/proof.small.mp4)")
    p_desktop_compose_video.add_argument("--small-metadata", help="Output small metadata JSON path (default: <run>/video/small-metadata.json)")
    p_desktop_compose_video.add_argument("--small-video-budget-mb", type=float, default=10.0, help="Attachment budget in decimal MB for the small video (default: 10)")
    p_desktop_compose_video.add_argument("--template", choices=["validation-proof", "design-parity"], help="Remotion proof template variant")
    p_desktop_compose_video.add_argument("--source-image", help="Optional source/reference image for design-parity proofs")
    p_desktop_compose_video.add_argument("--source-label", help="Label for --source-image (default: Source reference)")
    p_desktop_compose_video.add_argument("--title", help="Override the composed video title")
    p_desktop_compose_video.add_argument("--video-attachment-budget-mb", type=float, default=100.0, help="Attachment budget in decimal MB for the issue video (default: 100)")
    p_desktop_compose_video.add_argument("--json", action="store_true", help="Emit machine-readable JSON")

    p_desktop_video_doctor = desktop_sub.add_parser("video-doctor", help="Run video proof setup/readiness checks for one desktop target")
    p_desktop_video_doctor.add_argument("target", nargs="?", default="mac", help="Desktop target name (default: mac)")
    p_desktop_video_doctor.add_argument(
        "--skip-remotion-smoke",
        action="store_true",
        help="Skip the Remotion smoke render check and only report local tooling/config readiness.",
    )
    p_desktop_video_doctor.add_argument(
        "--run-in-terminal",
        action="store_true",
        help="On macOS, re-run video-doctor inside Terminal.app so Screen Recording permission follows Terminal's TCC grant.",
    )
    p_desktop_video_doctor.add_argument("--json", action="store_true", help="Emit machine-readable JSON")

    p_desktop_video_setup = desktop_sub.add_parser("video-setup", help="Print first-run video proof setup steps for one desktop target")
    p_desktop_video_setup.add_argument("target", nargs="?", default="mac", help="Desktop target name (default: mac)")
    p_desktop_video_setup.add_argument("--machine", help="Human-readable machine label to include in generated smoke labels")
    p_desktop_video_setup.add_argument("--check", action="store_true", help="Also run the video-doctor checks and include current readiness")
    p_desktop_video_setup.add_argument(
        "--run-in-terminal",
        action="store_true",
        help="On macOS, re-run video-setup inside Terminal.app when using --check so Screen Recording permission follows Terminal's TCC grant.",
    )
    p_desktop_video_setup.add_argument(
        "--skip-remotion-smoke",
        action="store_true",
        help="When using --check, skip the Remotion smoke render check.",
    )
    p_desktop_video_setup.add_argument("--json", action="store_true", help="Emit machine-readable JSON")

    p_desktop_serve = desktop_sub.add_parser("serve", help="Serve a desktop publish report over local HTTP")
    p_desktop_serve.add_argument("path", nargs="?", help="Optional report directory (default: latest published report)")
    p_desktop_serve.add_argument("--host", default="127.0.0.1", help="Host/interface to bind (default: 127.0.0.1)")
    p_desktop_serve.add_argument("--port", type=int, default=8765, help="Port to bind (default: 8765)")

    p_desktop_video = desktop_sub.add_parser("video", help="Run a desktop action with video proof recording enabled")
    p_desktop_video.add_argument("target", help="Desktop target name")
    p_desktop_video.add_argument("--command", dest="launch_command", help="Quoted command to launch in the GUI session")
    p_desktop_video.add_argument("--bundle-id", help="macOS bundle identifier to launch via `open -b`")
    p_desktop_video.add_argument(
        "--recipe",
        choices=["standalone-interaction", "reaper-plugin-editor", "inspector-workflow", "component-zoom", "design-parity"],
        help="Apply a named high-value proof recipe before running the desktop video action.",
    )
    p_desktop_video.add_argument("--plugin", help="Plugin name for plugin-host proof recipes")
    p_desktop_video.add_argument("--plugin-format", choices=["vst3", "auv2", "auv3", "clap", "lv2"], help="Plugin format for plugin-host proof recipes")
    p_desktop_video.add_argument("--host-app", help="Host application label for plugin-host proof recipes")
    p_desktop_video.add_argument("--component-id", help="Component/view id for component-zoom proof recipes")
    p_desktop_video.add_argument("--label", help="Optional artifact label")
    p_desktop_video.add_argument("--output", help="Optional screenshot output path")
    p_desktop_video.add_argument(
        "--action",
        choices=["smoke", "click", "inspect"],
        default="click",
        help="Desktop action to record (default: click)",
    )
    p_desktop_video.add_argument(
        "--capture-ui-snapshot",
        action="store_true",
        help="Request a Pulp-owned ViewInspector dump when supported by the action/target.",
    )
    p_desktop_video.add_argument("--click", help="Click at content-relative X,Y after launch")
    p_desktop_video.add_argument("--click-view-id", help="Click the center of the first visible ViewInspector node with this id")
    p_desktop_video.add_argument("--click-view-type", help="Click the center of the first visible ViewInspector node with this type")
    p_desktop_video.add_argument("--click-view-text", help="Click the center of the first visible ViewInspector node with this text")
    p_desktop_video.add_argument("--click-view-label", help="Click the center of the first visible ViewInspector node with this label")
    p_desktop_video.add_argument("--pulp-app-automation", action="store_true", help="Prefer a Pulp-owned in-app automation path when supported")
    p_desktop_video.add_argument("--capture-before", action="store_true", help="Capture a before screenshot for smoke actions with interaction")
    p_desktop_video.add_argument("--settle-secs", type=float, default=0.5, help="Seconds to wait after an interaction before the final screenshot (default: 0.5)")
    p_desktop_video.add_argument("--timeout", type=float, default=15.0, help="Wait timeout in seconds (default: 15)")
    p_desktop_video.add_argument("--json", action="store_true", help="Emit machine-readable JSON")
    add_desktop_video_args(p_desktop_video)
    add_desktop_source_args(p_desktop_video)

    p_desktop_cleanup = desktop_sub.add_parser("cleanup", help="Prune old desktop automation bundles")
    p_desktop_cleanup.add_argument("target", nargs="?", help="Optional one-target filter")
    p_desktop_cleanup.add_argument(
        "--older-than-days",
        type=int,
        help="Remove bundles older than N days (default: configured retention)",
    )
    p_desktop_cleanup.add_argument("--keep-last", type=int, default=0, help="Always keep the newest N bundles per filter (default: 0)")
    p_desktop_cleanup.add_argument("--json", action="store_true", help="Emit machine-readable JSON")

    p_desktop_smoke = desktop_sub.add_parser("smoke", help="Run a desktop automation smoke action on one target")
    p_desktop_smoke.add_argument("target", help="Desktop target name")
    p_desktop_smoke.add_argument("--command", dest="launch_command", help="Quoted command to launch in the GUI session")
    p_desktop_smoke.add_argument("--bundle-id", help="macOS bundle identifier to launch via `open -b`")
    p_desktop_smoke.add_argument("--label", help="Optional artifact label")
    p_desktop_smoke.add_argument("--output", help="Optional screenshot output path")
    p_desktop_smoke.add_argument(
        "--capture-ui-snapshot",
        action="store_true",
        help="Request a Pulp-owned ViewInspector dump via PULP_VIEW_TREE_OUT and fail if the app does not write it",
    )
    p_desktop_smoke.add_argument("--click", help="Click at content-relative X,Y after launch")
    p_desktop_smoke.add_argument("--click-view-id", help="Click the center of the first visible ViewInspector node with this id")
    p_desktop_smoke.add_argument("--click-view-type", help="Click the center of the first visible ViewInspector node with this type")
    p_desktop_smoke.add_argument("--click-view-text", help="Click the center of the first visible ViewInspector node with this text")
    p_desktop_smoke.add_argument("--click-view-label", help="Click the center of the first visible ViewInspector node with this label")
    p_desktop_smoke.add_argument("--pulp-app-automation", action="store_true", help="Prefer a Pulp-owned in-app automation path for direct launch commands when supported")
    p_desktop_smoke.add_argument("--capture-before", action="store_true", help="Capture a before screenshot when running an interaction")
    p_desktop_smoke.add_argument("--settle-secs", type=float, default=0.5, help="Seconds to wait after an interaction before the final screenshot (default: 0.5)")
    p_desktop_smoke.add_argument("--timeout", type=float, default=15.0, help="Wait timeout in seconds (default: 15)")
    p_desktop_smoke.add_argument("--json", action="store_true", help="Emit machine-readable JSON")
    add_desktop_video_args(p_desktop_smoke)
    add_desktop_source_args(p_desktop_smoke)

    p_desktop_click = desktop_sub.add_parser("click", help="Launch an app, perform one click interaction, and capture before/after evidence")
    p_desktop_click.add_argument("target", help="Desktop target name")
    p_desktop_click.add_argument("--command", dest="launch_command", help="Quoted command to launch in the GUI session")
    p_desktop_click.add_argument("--bundle-id", help="macOS bundle identifier to launch via `open -b`")
    p_desktop_click.add_argument("--label", help="Optional artifact label")
    p_desktop_click.add_argument("--output", help="Optional screenshot output path")
    p_desktop_click.add_argument(
        "--capture-ui-snapshot",
        action="store_true",
        help="Request a Pulp-owned ViewInspector dump via PULP_VIEW_TREE_OUT when using a direct launch command",
    )
    p_desktop_click.add_argument("--click", help="Click at content-relative X,Y after launch")
    p_desktop_click.add_argument("--click-view-id", help="Click the center of the first visible ViewInspector node with this id")
    p_desktop_click.add_argument("--click-view-type", help="Click the center of the first visible ViewInspector node with this type")
    p_desktop_click.add_argument("--click-view-text", help="Click the center of the first visible ViewInspector node with this text")
    p_desktop_click.add_argument("--click-view-label", help="Click the center of the first visible ViewInspector node with this label")
    p_desktop_click.add_argument("--pulp-app-automation", action="store_true", help="Prefer a Pulp-owned in-app automation path for direct launch commands when supported")
    p_desktop_click.add_argument("--settle-secs", type=float, default=0.5, help="Seconds to wait after the interaction before the final screenshot (default: 0.5)")
    p_desktop_click.add_argument("--timeout", type=float, default=15.0, help="Wait timeout in seconds (default: 15)")
    p_desktop_click.add_argument("--json", action="store_true", help="Emit machine-readable JSON")
    add_desktop_video_args(p_desktop_click)
    add_desktop_source_args(p_desktop_click)

    p_desktop_inspect = desktop_sub.add_parser("inspect", help="Launch an app and capture screenshot + available UI state")
    p_desktop_inspect.add_argument("target", help="Desktop target name (for example: mac)")
    p_desktop_inspect.add_argument("--command", dest="launch_command", help="Quoted command to launch in the GUI session")
    p_desktop_inspect.add_argument("--bundle-id", help="macOS bundle identifier to launch via `open -b` for screenshot-only inspect")
    p_desktop_inspect.add_argument("--label", help="Optional artifact label")
    p_desktop_inspect.add_argument("--output", help="Optional screenshot output path")
    p_desktop_inspect.add_argument("--pulp-app-automation", action="store_true", help="Use the Pulp-owned in-app automation path when the target adapter requires it")
    p_desktop_inspect.add_argument("--timeout", type=float, default=15.0, help="Wait timeout in seconds (default: 15)")
    p_desktop_inspect.add_argument("--json", action="store_true", help="Emit machine-readable JSON")
    add_desktop_video_args(p_desktop_inspect)
    add_desktop_source_args(p_desktop_inspect)
    return parser

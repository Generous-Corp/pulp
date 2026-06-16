"""Desktop subcommand parser construction for pulp ci-local."""

from __future__ import annotations

import argparse


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


def add_desktop_subcommands(sub: argparse._SubParsersAction) -> None:
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
    p_desktop_publish.add_argument("--json", action="store_true", help="Emit machine-readable JSON")

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
    add_desktop_source_args(p_desktop_inspect)

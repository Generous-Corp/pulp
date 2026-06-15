"""Cloud subcommand parser construction for pulp ci-local."""

from __future__ import annotations

import argparse


def add_cloud_subcommands(sub: argparse._SubParsersAction) -> None:
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

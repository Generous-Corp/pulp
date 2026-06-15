"""Cloud run command orchestration."""
from __future__ import annotations

from collections.abc import Callable
import argparse
import time
import uuid


def cmd_cloud_run(
    args: argparse.Namespace,
    *,
    gh_available_fn: Callable[[], bool],
    load_optional_config_fn: Callable[[], dict],
    resolve_github_actions_settings_fn: Callable[[dict], dict],
    resolve_github_repository_fn: Callable[[dict], str],
    builtin_github_workflows: dict,
    current_branch_fn: Callable[[], str],
    resolve_default_provider_for_workflow_fn: Callable[..., tuple[str, str]],
    gh_repo_variables_fn: Callable[[str], dict[str, str]],
    resolve_workflow_dispatch_defaults_fn: Callable[..., tuple[dict[str, str], dict[str, str]]],
    resolve_cli_dispatch_field_values_fn: Callable[[argparse.Namespace, object], dict[str, str]],
    normalize_runs_on_json_fn: Callable[[str, str], str],
    resolve_workflow_field_value_and_source_fn: Callable[..., tuple[str, str]],
    now_iso_fn: Callable[[], str],
    normalize_cloud_record_fn: Callable[[dict], dict],
    cloud_run_record_payload_fn: Callable[..., dict],
    gh_current_login_fn: Callable[[], str | None],
    save_cloud_record_fn: Callable[[dict], object],
    cloud_workflow_dispatch_fields_fn: Callable[..., dict[str, str]],
    gh_workflow_dispatch_fn: Callable[[str, str, str, dict[str, str]], None],
    gh_find_dispatched_run_fn: Callable[..., dict | None],
    enrich_cloud_record_provider_metadata_fn: Callable[[dict], dict],
    update_cloud_record_from_run_fn: Callable[..., dict],
    cloud_dispatch_lines_fn: Callable[..., list[str]],
    refresh_cloud_record_fn: Callable[..., dict],
    cloud_final_status_line_fn: Callable[[dict], str],
    print_fn: Callable[[str], None] = print,
    sleep_fn: Callable[[float], None] = time.sleep,
    uuid_hex_fn: Callable[[], str] = lambda: uuid.uuid4().hex,
) -> int:
    if not gh_available_fn():
        print_fn("Error: gh CLI not available or not authenticated. Run: gh auth login")
        return 1

    config = load_optional_config_fn()
    try:
        settings = resolve_github_actions_settings_fn(config)
        repository = resolve_github_repository_fn(settings)
    except ValueError as exc:
        print_fn(f"Error: {exc}")
        return 1

    workflow_key = args.workflow or settings.get("workflow", "build")
    workflow = builtin_github_workflows.get(workflow_key)
    if workflow is None:
        print_fn(
            f"Error: Unknown workflow '{workflow_key}'. Use `pulp ci-local cloud workflows` to list supported workflows."
        )
        return 1

    branch = args.branch or current_branch_fn()
    try:
        provider, _provider_source = resolve_default_provider_for_workflow_fn(
            settings,
            workflow_key,
            explicit_provider=getattr(args, "provider", None),
        )
        repository_variables = gh_repo_variables_fn(repository)
        config_dispatch_fields, _config_dispatch_sources = resolve_workflow_dispatch_defaults_fn(
            config,
            repository_variables,
            workflow_key,
            provider,
            workflow.get("dispatch_fields"),
        )
        cli_dispatch_fields = resolve_cli_dispatch_field_values_fn(
            args, workflow.get("dispatch_fields")
        )
        selector_input = workflow.get("selector_input")
        if getattr(args, "runner_selector_json", None):
            selector_json = normalize_runs_on_json_fn(
                args.runner_selector_json,
                setting_name="--runner-selector-json",
            )
        elif selector_input:
            selector_json, _selector_source = resolve_workflow_field_value_and_source_fn(
                config,
                repository_variables,
                workflow_key,
                provider,
                selector_input,
            )
        else:
            selector_json = ""
        config_dispatch_fields.update(cli_dispatch_fields)
    except ValueError as exc:
        print_fn(f"Error: {exc}")
        return 1

    selector_input = workflow.get("selector_input")
    if selector_json and not selector_input:
        print_fn(f"Error: workflow '{workflow_key}' does not accept an explicit runner selector.")
        return 1

    dispatch_id = uuid_hex_fn()[:12]
    dispatch_time = now_iso_fn()
    record = normalize_cloud_record_fn(
        cloud_run_record_payload_fn(
            dispatch_id=dispatch_id,
            repository=repository,
            workflow_key=workflow_key,
            workflow=workflow,
            branch=branch,
            requested_by=gh_current_login_fn() or "",
            provider=provider,
            selector_json=selector_json,
            dispatch_fields=config_dispatch_fields,
            dispatch_time=dispatch_time,
        )
    )
    save_cloud_record_fn(record)

    fields = cloud_workflow_dispatch_fields_fn(
        workflow,
        provider=provider,
        dispatch_fields=config_dispatch_fields,
        selector_json=selector_json,
    )

    try:
        gh_workflow_dispatch_fn(repository, workflow["file"], branch, fields)
    except RuntimeError as exc:
        print_fn(f"Error: {exc}")
        return 1

    matched = gh_find_dispatched_run_fn(
        repository,
        workflow["file"],
        branch,
        dispatch_time,
        timeout_secs=int(settings["match_timeout_secs"]),
    )

    if matched:
        record = enrich_cloud_record_provider_metadata_fn(
            update_cloud_record_from_run_fn(record, matched, provider_resolved=provider)
        )
        record["match_ambiguous"] = bool(matched.get("match_ambiguous"))
        save_cloud_record_fn(record)

    for line in cloud_dispatch_lines_fn(record, workflow_key=workflow_key, branch=branch, provider=provider):
        print_fn(line)

    if not args.wait:
        return 0

    if not record.get("run_id"):
        print_fn("Error: blocking wait requested, but the dispatched GitHub run could not be matched.")
        return 1

    while record.get("status") != "completed":
        sleep_fn(int(settings["wait_poll_secs"]))
        try:
            record = refresh_cloud_record_fn(record, repository, require_snapshot=True)
        except RuntimeError as exc:
            print_fn(f"Error: {exc}")
            return 1

    print_fn(cloud_final_status_line_fn(record))
    return 0 if record.get("conclusion") == "success" else 1

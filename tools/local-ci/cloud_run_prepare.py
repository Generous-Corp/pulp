"""Pure helpers for preparing cloud workflow dispatch records and fields."""
from __future__ import annotations


def cloud_run_record_payload(
    *,
    dispatch_id: str,
    repository: str,
    workflow_key: str,
    workflow: dict,
    branch: str,
    requested_by: str,
    provider: str,
    selector_json: str,
    dispatch_fields: dict[str, str],
    dispatch_time: str,
) -> dict:
    return {
        "dispatch_id": dispatch_id,
        "repository": repository,
        "workflow_key": workflow_key,
        "workflow_file": workflow["file"],
        "workflow_name": workflow["display_name"],
        "requested_ref": branch,
        "requested_by": requested_by,
        "provider_requested": provider,
        "runner_selector_json": selector_json,
        "dispatch_fields": dispatch_fields,
        "status": "unresolved",
        "dispatched_at": dispatch_time,
        "updated_at": dispatch_time,
        "match_strategy": "workflow+branch+created_at",
    }


def cloud_workflow_dispatch_fields(
    workflow: dict,
    *,
    provider: str,
    dispatch_fields: dict[str, str],
    selector_json: str,
) -> dict[str, str]:
    fields: dict[str, str] = {}
    provider_input = workflow.get("provider_input")
    if provider_input:
        fields[provider_input] = provider
    fields.update(dispatch_fields)
    selector_input = workflow.get("selector_input")
    if selector_input and selector_json:
        fields[selector_input] = selector_json
    return fields

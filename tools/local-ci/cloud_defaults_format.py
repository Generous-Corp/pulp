"""Line rendering helpers for `pulp ci-local cloud defaults`."""
from __future__ import annotations

from cloud_billing import billing_note_text, resolve_billing_settings
from cloud_records import render_selector_value
from github_workflows import BUILTIN_GITHUB_WORKFLOWS, summarize_workflow_provider_defaults


def cloud_field_detail_line(
    name: str,
    value: str,
    source: str = "",
    *,
    indent: str = "    ",
    unset_note: str = "",
) -> str:
    rendered = render_selector_value(value) if name.endswith("_selector_json") else str(value)
    if rendered:
        suffix = f" ({source})" if source else ""
        return f"{indent}{name}: {rendered}{suffix}"
    if unset_note:
        return f"{indent}{name}: unset ({unset_note})"
    return f"{indent}{name}: unset"


def cloud_defaults_lines(
    config: dict,
    settings: dict,
    *,
    repository: str = "",
    repository_note: str = "",
    repository_variables: dict[str, str] | None = None,
) -> list[str]:
    lines = ["Cloud defaults:", ""]
    if repository:
        lines.append(f"  repository: {repository}")
    else:
        lines.append("  repository: unresolved")
    if repository_note:
        lines.append(f"  note: {repository_note}")
    lines.append(f"  configured default workflow: {settings.get('workflow', 'build')}")
    lines.append(f"  configured default provider: {settings.get('provider', 'github-hosted')}")

    billing = resolve_billing_settings(config)
    lines.append(
        f"  billing estimates: {billing.get('currency', 'USD')} period-day={billing.get('billing_period_start_day', 1)} "
        f"({billing_note_text()})"
    )
    provider_truth_state = "enabled (local opt-in)" if billing.get("enable_provider_reported_totals") else "disabled (opt-in; off by default)"
    lines.append(f"  provider billing truth: {provider_truth_state}")

    variables = repository_variables or {}
    for workflow_key, workflow in BUILTIN_GITHUB_WORKFLOWS.items():
        summary = summarize_workflow_provider_defaults(
            config,
            variables,
            settings,
            workflow_key,
        )
        lines.append("")
        lines.append(f"  {workflow_key}: {workflow['display_name']} ({workflow['file']})")
        lines.append(f"    supported providers: {', '.join(workflow.get('providers', ['github-hosted']))}")
        lines.append(
            f"    default provider: {summary['provider']} ({summary['provider_source']})"
        )
        selector_input = summary.get("selector_input") or ""
        if selector_input:
            lines.append(
                cloud_field_detail_line(
                    selector_input,
                    summary.get("selector_value", ""),
                    summary.get("selector_source", ""),
                )
            )
        for field_name in workflow.get("dispatch_fields") or []:
            unset_note = ""
            if workflow_key == "build" and field_name == "macos_runner_selector_json":
                unset_note = "macOS stays local-first unless a config default or one-off override is set"
            lines.append(
                cloud_field_detail_line(
                    field_name,
                    (summary.get("dispatch_fields") or {}).get(field_name, ""),
                    (summary.get("dispatch_sources") or {}).get(field_name, ""),
                    unset_note=unset_note,
                )
            )
    return lines

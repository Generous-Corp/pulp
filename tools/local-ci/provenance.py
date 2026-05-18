"""Provenance helpers for local CI job records.

Extracted from local_ci.py as part of the R2-1 phased split. Provenance
tracks who/what dispatched a CI run — direct vs hosted, which control
plane, which runner provider/selector, and the run id/URL. Both job
records and result records carry a `provenance` dict normalised through
these helpers so downstream consumers (status display, evidence) can
read a stable shape regardless of how the job was submitted.

All three functions are pure: no I/O, no globals, no exceptions on
malformed input — missing fields default to empty strings.
"""

from __future__ import annotations


def normalize_provenance(provenance: dict | None = None) -> dict:
    normalized = dict(provenance or {})
    normalized.setdefault("execution_kind", "direct")
    normalized.setdefault("control_plane", "pulp-ci-local")
    normalized.setdefault("direct_backend", "local-ci")
    normalized.setdefault("hosted_orchestrator", "")
    normalized.setdefault("runner_provider", "")
    normalized.setdefault("runner_selector", "")
    normalized.setdefault("run_id", "")
    normalized.setdefault("run_url", "")
    return normalized


def provenance_summary(provenance: dict | None) -> str:
    info = normalize_provenance(provenance)
    execution_kind = info.get("execution_kind", "direct")
    selector = info.get("runner_selector", "")
    run_id = info.get("run_id", "")

    if execution_kind == "hosted":
        orchestrator = info.get("hosted_orchestrator", "") or "unknown-orchestrator"
        provider = info.get("runner_provider", "")
        summary = f"hosted via {orchestrator}"
        if provider:
            summary += f"/{provider}"
    else:
        summary = f"direct via {info.get('direct_backend', 'local-ci') or 'local-ci'}"

    if selector:
        summary += f" selector={selector}"
    if run_id:
        summary += f" run={run_id}"
    return summary


def normalize_result(result: dict) -> dict:
    normalized = dict(result)
    submission = normalized.get("submission") or {}
    normalized["provenance"] = normalize_provenance(
        normalized.get("provenance") or submission.get("provenance")
    )
    return normalized

"""Queue result line formatting helpers."""

from __future__ import annotations

from provenance import provenance_summary


def result_validation_line(result: dict) -> str | None:
    validation = result.get("validation", "full")
    if validation == "full":
        return None
    return f"  {'validation':10s}  {validation}"


def result_execution_line(result: dict) -> str:
    return f"  {'execution':10s}  {provenance_summary(result.get('provenance'))}"


def target_result_line(item: dict) -> str:
    icon = "PASS" if item["status"] == "pass" else item["status"].upper()
    return f"  {item['target']:10s}  {icon:12s}  {item.get('duration_secs', 0)}s"


def result_target_lines(result: dict) -> list[str]:
    return [target_result_line(item) for item in result.get("results", [])]


def result_overall_line(result: dict) -> str:
    return f"  {'overall':10s}  {result['overall'].upper()}"

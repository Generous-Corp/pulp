"""PR list and CI comment formatting helpers for local CI."""
from __future__ import annotations

from git_helpers import short_sha
from provenance import normalize_result, provenance_summary


def no_open_prs_line() -> str:
    return "No open PRs."


def open_prs_header_line(count: int) -> str:
    return f"Open PRs ({count}):"


def open_pr_list_entry_lines(pr: dict) -> list[str]:
    author = pr.get("author", {}).get("login", "?")
    labels = ", ".join(label.get("name", "") for label in pr.get("labels", []))
    label_str = f" [{labels}]" if labels else ""
    return [
        f"  #{pr['number']:4d}  {pr['title']}",
        f"         {pr['headRefName']} by {author}{label_str}",
    ]


def open_pr_list_lines(prs: list[dict]) -> list[str]:
    if not prs:
        return [no_open_prs_line()]

    lines = [open_prs_header_line(len(prs)), ""]
    for pr in prs:
        lines.extend(open_pr_list_entry_lines(pr))
    return lines


def format_ci_comment(result: dict) -> str:
    result = normalize_result(result)
    validation = result.get("validation", "full")
    title = "Local CI Smoke Results" if validation == "smoke" else "Local CI Results"
    lines = [f"## {title}\n"]
    overall = result["overall"].upper()
    icon = "white_check_mark" if overall == "PASS" else "x"
    lines.append(f":{icon}: **Overall: {overall}**\n")
    lines.append(f"Job: `{result.get('job_id', '?')}`  Commit: `{short_sha(result.get('sha', ''))}`\n")
    lines.append(f"Execution: `{provenance_summary(result.get('provenance'))}`\n")
    if result.get("provenance", {}).get("run_url"):
        lines.append(f"Run URL: {result['provenance']['run_url']}\n")
    if validation != "full":
        lines.append(f"Validation: `{validation}`\n")
        lines.append("_Smoke mode is a fast clean install/export preflight and does not run the full test suite._\n")
    lines.append("| Target | Status | Duration |")
    lines.append("|--------|--------|----------|")
    for item in result["results"]:
        status = item["status"].upper()
        s_icon = "white_check_mark" if status == "PASS" else "x"
        lines.append(f"| {item['target']} | :{s_icon}: {status} | {item.get('duration_secs', 0)}s |")

    if any(item["status"] != "pass" for item in result["results"]):
        lines.append("\n<details><summary>Failure details</summary>\n")
        for item in result["results"]:
            if item["status"] == "pass":
                continue
            lines.append(f"### {item['target']} (exit {item.get('exit_code', '?')})")
            stderr = item.get("stderr_tail", "")
            if stderr:
                lines.append(f"```\n{stderr[-500:]}\n```")
        lines.append("</details>")

    lines.append(f"\n*Run at {result.get('completed_at', 'unknown')}*")
    return "\n".join(lines)

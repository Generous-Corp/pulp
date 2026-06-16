"""Branch-publish metadata helpers for desktop automation reports."""

from __future__ import annotations


def desktop_published_run_payload(run: dict, *, remote_base: str, branch: str) -> dict:
    artifact_urls = {}
    for key, value in (run.get("artifacts") or {}).items():
        if isinstance(value, str):
            artifact_urls[key] = f"{remote_base}/blob/{branch}/desktop-automation/latest/{value}"
    return {
        "label": run.get("label"),
        "target": run.get("target"),
        "action": run.get("action"),
        "artifact_urls": artifact_urls,
    }


def desktop_branch_publish_metadata(
    report: dict,
    *,
    branch: str,
    report_name: str,
    remote_base: str | None,
) -> dict:
    published = {
        "mode": "branch",
        "branch": branch,
        "report_path": f"desktop-automation/reports/{report_name}",
        "latest_path": "desktop-automation/latest",
    }
    if not remote_base:
        return published

    published["branch_url"] = f"{remote_base}/tree/{branch}"
    published["report_url"] = f"{remote_base}/tree/{branch}/desktop-automation/reports/{report_name}"
    published["latest_url"] = f"{remote_base}/tree/{branch}/desktop-automation/latest"
    published["latest_index_json_url"] = f"{remote_base}/blob/{branch}/desktop-automation/latest/index.json"
    published["runs"] = [
        desktop_published_run_payload(run, remote_base=remote_base, branch=branch)
        for run in report.get("runs", [])
    ]
    return published

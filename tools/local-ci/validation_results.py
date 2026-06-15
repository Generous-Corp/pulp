"""Validation result payload helpers for local CI."""

from __future__ import annotations

from pathlib import Path


def validation_result_from_run(
    target_name: str,
    run: dict,
    *,
    log_path: Path,
    validation: str,
    transport_mode: str,
    timeout_secs: int = 3600,
) -> dict:
    if run["timed_out"]:
        return {
            "target": target_name,
            "status": "timeout",
            "exit_code": -1,
            "duration_secs": run["duration_secs"],
            "stdout_tail": "",
            "stderr_tail": f"Validation timed out after {timeout_secs}s",
            "log_file": str(log_path),
            "transport_mode": transport_mode,
        }

    tail = run["output"][-2000:] if run["output"] else ""
    if validation == "smoke":
        if "__PULP_VALIDATION__:smoke" not in run["output"] or "__PULP_TEST_POLICY__:skip" not in run["output"]:
            failed = True
            tail = (
                "Smoke validation contract violated: expected validation=smoke and test_policy=skip markers.\n"
                + tail
            )[-2000:]
        else:
            failed = run["returncode"] != 0
    else:
        failed = run["returncode"] != 0

    return {
        "target": target_name,
        "status": "pass" if not failed else "fail",
        "exit_code": run["returncode"],
        "duration_secs": run["duration_secs"],
        "stdout_tail": "" if failed else tail,
        "stderr_tail": tail if failed else "",
        "log_file": str(log_path),
        "validation": validation,
        "transport_mode": transport_mode,
    }


def validation_error_result(
    target_name: str,
    detail: str,
    *,
    log_path: Path,
    transport_mode: str,
) -> dict:
    return {
        "target": target_name,
        "status": "error",
        "exit_code": -1,
        "duration_secs": 0.0,
        "stdout_tail": "",
        "stderr_tail": detail,
        "log_file": str(log_path),
        "transport_mode": transport_mode,
    }


def unreachable_target_result(target_name: str, detail: str = "Host unreachable") -> dict:
    return {
        "target": target_name,
        "status": "unreachable",
        "exit_code": -1,
        "duration_secs": 0,
        "stdout_tail": "",
        "stderr_tail": detail,
    }


def target_exception_result(target_name: str, exc: Exception) -> dict:
    return {
        "target": target_name,
        "status": "error",
        "exit_code": -1,
        "duration_secs": 0,
        "stdout_tail": "",
        "stderr_tail": str(exc),
    }


def completed_job_result(
    job: dict,
    results: list[dict],
    *,
    completed_at: str,
    provenance: dict,
) -> dict:
    payload = {
        "job_id": job["id"],
        "branch": job["branch"],
        "sha": job["sha"],
        "priority": job["priority"],
        "submission": job.get("submission"),
        "provenance": provenance,
        "targets": job.get("targets", []),
        "queued_at": job.get("queued_at", ""),
        "completed_at": completed_at,
        "results": results,
        "overall": "pass" if all(result["status"] == "pass" for result in results) else "fail",
    }
    if results:
        payload["validation"] = job.get("validation", "full")
    return payload


def sorted_target_results(results: list[dict]) -> list[dict]:
    return sorted(results, key=lambda item: item["target"])

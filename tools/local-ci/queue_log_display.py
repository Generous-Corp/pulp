"""Queue log line formatting helpers."""

from __future__ import annotations

from git_helpers import short_sha


def missing_job_logs_line() -> str:
    return "No matching job logs found."


def missing_log_files_line(job: dict) -> str:
    return f"No logs found for job [{job['id']}] {job['branch']}."


def job_logs_header_line(job: dict) -> str:
    return f"Logs for [{job['id']}] {job['branch']} @ {short_sha(job.get('sha', ''))}"


def log_section_header_line(target: str) -> str:
    return f"== {target} =="


def empty_log_line() -> str:
    return "(empty)"

#!/usr/bin/env python3
"""Decide and (optionally) request one sanitizer job rerun for a known flake.

The decision is deliberately pure and conservative.  The CLI only performs
GitHub reads, then requests GitHub's failed-job rerun endpoint when enabled.
"""
from __future__ import annotations

import argparse
import io
import json
import os
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path
from typing import Any, Iterable

SCHEMA = "pulp-flake-rerun-once/v1"
SANITIZER_WORKFLOW = "Sanitizer Tests"
DETERMINISTIC_LABEL = "pr-fast"


def _under(path: str, root: str) -> bool:
    path, root = path.strip("/"), root.strip("/")
    return bool(root) and (path == root or path.startswith(root + "/"))


def _outside_surface(test: str, changed_files: Iterable[str], mapping: dict[str, Any]) -> bool:
    """Return true only when a known test mapping is untouched by the PR."""
    declared = mapping.get(test)
    if not declared or not isinstance(declared, (list, tuple, set)):
        return False
    paths = [p for p in declared if isinstance(p, str) and p.strip()]
    if not paths:
        return False
    return not any(_under(changed, source) for changed in changed_files for source in paths)


def decide(
    run: dict[str, Any],
    failing: Iterable[str],
    main: Any,
    changed_files: Iterable[str],
    test_paths: dict[str, Any],
    deterministic: Iterable[str] = (),
) -> dict[str, Any]:
    """Pure policy decision; no GitHub reads or writes occur here."""
    failures = sorted(set(str(name) for name in failing if name))
    reasons: list[str] = []
    if run.get("event") == "merge_group":
        reasons.append("merge_group run")
    if run.get("event") not in (None, "pull_request"):
        reasons.append("run is not a pull_request workflow")
    if run.get("workflow_name") and run["workflow_name"] != SANITIZER_WORKFLOW:
        reasons.append("not the sanitizer workflow")
    try:
        attempt = int(run.get("run_attempt", 1))
    except (TypeError, ValueError):
        attempt = 0
    if attempt != 1:
        reasons.append("run_attempt is not 1")
    if run.get("conclusion") not in (None, "failure"):
        reasons.append("run did not fail")
    if len(failures) != 1:
        reasons.append("run did not have exactly one failing test")

    test = failures[0] if len(failures) == 1 else ""
    deterministic_set = set(deterministic)
    if test and (test in deterministic_set or test in set(run.get("deterministic", ()) or ())):
        reasons.append("deterministic test (pr-fast or drift-fast)")

    def main_value(name: str, default: Any = None) -> Any:
        return main.get(name, default) if isinstance(main, dict) else getattr(main, name, default)

    main_read = bool(main_value("read", False))
    main_conclusion = main_value("conclusion")
    main_failed = set(main_value("failing", ()) or ())
    passes_main = bool(test and main_read and (
        main_conclusion == "success" or (main_conclusion == "failure" and test not in main_failed)
    ))
    if not passes_main:
        reasons.append("test did not pass on main's latest executed gate")
    if test and not _outside_surface(test, changed_files, test_paths):
        reasons.append("test-to-path mapping is unknown or changed surface overlaps")

    eligible = not reasons
    return {
        "schema": SCHEMA,
        "eligible": eligible,
        "would_rerun": eligible,
        "test": test,
        "failing": failures,
        "reasons": reasons or ["all policy conditions hold"],
        "main_evidence_read": main_read,
    }


def _gh(path: str, *, write: bool = False) -> str | None:
    cli = os.environ.get("PULP_GH_CLI", "gh")
    command = [cli, "api"]
    if write:
        command += ["--method", "POST"]
    command.append(path)
    try:
        result = subprocess.run(command, capture_output=True, text=True, timeout=120)
    except (OSError, subprocess.TimeoutExpired):
        return None
    return result.stdout.strip() if result.returncode == 0 else None


def _gh_bytes(path: str) -> bytes | None:
    cli = os.environ.get("PULP_GH_CLI", "gh")
    try:
        result = subprocess.run([cli, "api", path], capture_output=True, timeout=120)
    except (OSError, subprocess.TimeoutExpired):
        return None
    return result.stdout if result.returncode == 0 else None


def _json(raw: str | None) -> Any:
    try:
        return json.loads(raw) if raw else None
    except json.JSONDecodeError:
        return None


def _shadow():
    """Import the established shadow helpers only when the CLI is used."""
    here = Path(__file__).resolve().parent
    sys.path.insert(0, str(here))
    import flake_exoneration_shadow as shadow  # type: ignore
    return shadow


def _artifact_files(repo: str, run_id: str, key: str) -> tuple[bytes | None, bytes | None]:
    raw = _gh(f"repos/{repo}/actions/runs/{run_id}/artifacts?per_page=100")
    listing = _json(raw) or {}
    artifact = next((a for a in listing.get("artifacts", [])
                     if a.get("name") == f"ctest-logs-{key}" and not a.get("expired")), None)
    if not artifact or not str(artifact.get("id", "")).isdigit():
        return None, None
    blob = _gh_bytes(f"repos/{repo}/actions/artifacts/{artifact['id']}/zip")
    if not blob:
        return None, None
    try:
        with zipfile.ZipFile(io.BytesIO(blob)) as archive:
            junit = archive.read("ctest.junit.xml")
            selected = archive.read("selected.json")
    except (zipfile.BadZipFile, KeyError):
        return None, None
    return junit, selected


def _main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True)
    ap.add_argument("--run-id", required=True)
    ap.add_argument("--enabled", action="store_true")
    args = ap.parse_args(argv)
    raw = _gh(f"repos/{args.repo}/actions/runs/{args.run_id}")
    run = _json(raw) or {}
    run["workflow_name"] = run.get("name")
    if run.get("event") == "merge_group":
        result = decide(run, (), None, (), {})
    else:
        prs = run.get("pull_requests") or []
        pr = prs[0].get("number") if len(prs) == 1 else None
        # The workflow run is the matrix, so identify the failed sanitizer jobs.
        jobs = _json(_gh(f"repos/{args.repo}/actions/runs/{args.run_id}/jobs?filter=latest&per_page=100")) or {}
        failed_keys = []
        for job in jobs.get("jobs", []):
            if job.get("conclusion") != "failure":
                continue
            if "AddressSanitizer" in job.get("name", ""):
                failed_keys.append("asan")
            elif "UndefinedBehaviorSanitizer" in job.get("name", ""):
                failed_keys.append("ubsan")
        labels: dict[str, set[str]] = {}
        failing: list[str] = []
        mapping: dict[str, list[str]] = {}
        shadow = _shadow()
        for key in sorted(set(failed_keys)):
            junit, selected = _artifact_files(args.repo, args.run_id, key)
            if junit:
                with tempfile.NamedTemporaryFile(suffix=".xml") as report:
                    report.write(junit)
                    report.flush()
                    labels.update(shadow.junit_failure_labels(Path(report.name)))
            if selected:
                data = _json(selected) or {}
                for test in data.get("tests", []):
                    name = test.get("name")
                    paths = [a for a in test.get("command", [])
                             if isinstance(a, str) and "/" in a
                             and not any(part in a for part in ("/build-asan/", "/build-ubsan/", "/build/"))]
                    if name and paths:
                        mapping[name] = paths
        failing = sorted(labels)
        changed = _json(_gh(f"repos/{args.repo}/pulls/{pr}/files?per_page=100")) if pr else None
        changed_files = [f.get("filename") for f in (changed or []) if f.get("filename")]
        shadow = _shadow()
        main = shadow.main_evidence(args.repo, lookback=10)
        deterministic = shadow.deterministic_tests(labels, shadow.drift_fast_tests())
        result = decide(run, failing, main, changed_files, mapping, deterministic)
    result.update({"run_id": str(args.run_id), "mode": "enforce" if args.enabled else "shadow"})
    if args.enabled and result["eligible"]:
        ok = _gh(f"repos/{args.repo}/actions/runs/{args.run_id}/rerun-failed-jobs", write=True)
        result["requested"] = ok is not None
    else:
        result["requested"] = False
    print(f"::notice title={SCHEMA}::{json.dumps(result, sort_keys=True)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(_main(sys.argv[1:]))

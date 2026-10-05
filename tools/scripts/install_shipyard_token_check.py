#!/usr/bin/env python3
"""Require a GitHub token on every workflow step that installs Shipyard.

``tools/install-shipyard.sh`` delegates to Shipyard's upstream installer, which
resolves the pinned release through the GitHub API and authenticates only when
``SHIPYARD_GITHUB_TOKEN`` or ``GITHUB_TOKEN`` is in its environment. Anonymous
calls from shared hosted-runner IPs exhaust the 60/hour limit on busy days and
fail the step with HTTP 403. The installer refuses to run tokenless under
GitHub Actions; this lint catches the same omission statically, before a run.

A step that runs ``install-shipyard.sh`` passes when one of those variables is
set to a non-empty value in its own ``env:``, its job's ``env:``, or the
workflow's top-level ``env:`` (the levels GitHub merges into a step).

Usage:
    install_shipyard_token_check.py            # scan .github/workflows
    install_shipyard_token_check.py <files...> # scan specific files

Exit codes: 0 = every installing step carries a token, 1 = violations,
2 = PyYAML unavailable.
"""
from __future__ import annotations

import glob
import sys

try:
    import yaml
except ImportError:
    print("install_shipyard_token_check: PyYAML not available", file=sys.stderr)
    sys.exit(2)

INSTALLER = "install-shipyard.sh"
TOKEN_VARS = ("GITHUB_TOKEN", "SHIPYARD_GITHUB_TOKEN")


def _has_token(*envs) -> bool:
    for env in envs:
        if not isinstance(env, dict):
            continue
        for var in TOKEN_VARS:
            value = env.get(var)
            if isinstance(value, str) and value.strip():
                return True
    return False


def installing_steps(doc):
    """Yield (job_name, job, index, step) for each step that runs the installer."""
    if not isinstance(doc, dict):
        return
    for job_name, job in (doc.get("jobs") or {}).items():
        if not isinstance(job, dict):
            continue
        for index, step in enumerate(job.get("steps") or []):
            if isinstance(step, dict) and INSTALLER in str(step.get("run") or ""):
                yield job_name, job, index, step


def check_doc(doc, label: str) -> list[str]:
    """Return violation strings for one parsed workflow document."""
    if not isinstance(doc, dict):
        return []
    workflow_env = doc.get("env")
    violations = []
    for job_name, job, index, step in installing_steps(doc):
        if _has_token(step.get("env"), job.get("env"), workflow_env):
            continue
        step_name = step.get("name") or f"step {index}"
        violations.append(
            f"{label}: job '{job_name}' step '{step_name}' runs {INSTALLER} "
            "without GITHUB_TOKEN or SHIPYARD_GITHUB_TOKEN in its env; add "
            "`env: GITHUB_TOKEN: ${{ github.token }}` to the step"
        )
    return violations


def _load(path: str):
    with open(path, encoding="utf-8") as handle:
        return yaml.safe_load(handle)


def main(argv: list[str]) -> int:
    paths = argv or sorted(
        glob.glob(".github/workflows/*.yml") + glob.glob(".github/workflows/*.yaml")
    )
    if not paths:
        print("install_shipyard_token_check: no workflow files found", file=sys.stderr)
        return 1
    steps = 0
    violations = []
    for path in paths:
        try:
            doc = _load(path)
        except yaml.YAMLError as exc:
            violations.append(f"{path}: YAML parse error: {exc}")
            continue
        steps += sum(1 for _ in installing_steps(doc))
        violations.extend(check_doc(doc, path))
    if violations:
        for line in violations:
            print(line, file=sys.stderr)
        return 1
    print(f"install_shipyard_token_check: OK ({steps} step(s) install Shipyard, all with a token)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

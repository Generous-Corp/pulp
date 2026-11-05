#!/usr/bin/env python3
"""Every job that reads a credential reads it from a main-only environment.

A repository-level secret is readable by any workflow run, including one
started by pushing a branch that edits the workflow. An environment secret is
released only to a job whose ref passes the environment's deployment policy
(branch main, plus tags v* for `release`). This test pins the code half of that
boundary: each credential named below is read only by jobs that declare the
expected environment, and a job in a workflow that pull requests can trigger
declares an environment only behind an event guard, so no pull-request run is
ever pointed at one. The policy half lives in the repository settings.
"""
from __future__ import annotations

import re
import unittest
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS = ROOT / ".github" / "workflows"

# Credential -> environments that may hold it.
ENVIRONMENT_SECRETS: dict[str, set[str]] = {
    # Tagging: the fleet-release App, or the interim release-bot PAT.
    "FLEET_RELEASE_APP_PRIVATE_KEY": {"release"},
    "RELEASE_BOT_SSH_SIGNING_KEY": {"release", "version"},
    # Interim PAT: tags (release), the bump PR (version), and three
    # main-only monitors (automation).
    "RELEASE_BOT_TOKEN": {"release", "version", "automation"},
    "SHIPYARD_APP_ID": {"automation"},
    "SHIPYARD_APP_PRIVATE_KEY": {"automation"},
    "VELLUM_DISPATCHER_APP_PRIVATE_KEY": {"automation"},
    "CLOUDFLARE_API_TOKEN": {"automation"},
    "CLOUDFLARE_ACCOUNT_ID": {"automation"},
    "TIER_RECORD_APP_ID": {"tier-record"},
    "TIER_RECORD_APP_KEY": {"tier-record"},
}

# Repository-level secrets that stay readable from any ref, with the reason.
REPOSITORY_SECRETS: dict[str, str] = {
    # Upload-only coverage token; pull-request jobs upload with it.
    "CODECOV_TOKEN": "upload-only, read by pull-request jobs",
    # Read-only App the pull_request_target / merge_group trusted gate uses.
    "VELLUM_READER_APP_PRIVATE_KEY": "read-only App, read by merge-group jobs",
    # Optional developer-supplied SDK location; not configured.
    "PULP_AAX_SDK_ZIP_URL": "not a credential, unset",
    "PULP_AAX_SDK_ZIP_SHA256": "not a credential, unset",
    # macOS signing for sign-and-release.yml (tag-triggered); not configured.
    # Set them only in a tag-admitting environment, and only after the v* tag
    # ruleset restricts creation to the release identity.
    "MACOS_CERTIFICATE": "unset",
    "MACOS_CERTIFICATE_PWD": "unset",
    "KEYCHAIN_PASSWORD": "unset",
    "SIGNING_IDENTITY": "unset",
    "APPLE_ID": "unset",
    "APPLE_TEAM_ID": "unset",
    "APPLE_PASSWORD": "unset",
}

TAG_GUARD = "Require the tag to point at a commit on main"
PULL_REQUEST_EVENTS = {"pull_request", "pull_request_target", "merge_group"}
SECRET_RE = re.compile(r"secrets\.([A-Za-z_][A-Za-z0-9_]*)")


def _triggers(workflow: dict) -> set[str]:
    on = workflow.get(True, workflow.get("on"))
    if isinstance(on, str):
        return {on}
    if isinstance(on, list):
        return set(on)
    return set(on or {})


def _environment(job: dict) -> str | None:
    env = job.get("environment")
    if isinstance(env, dict):
        return env.get("name")
    return env


def _jobs():
    for path in sorted(WORKFLOWS.glob("*.y*ml")):
        workflow = yaml.safe_load(path.read_text(encoding="utf-8"))
        for name, job in (workflow.get("jobs") or {}).items():
            text = yaml.safe_dump(job)
            secrets = set(SECRET_RE.findall(text)) - {"GITHUB_TOKEN"}
            yield path.name, _triggers(workflow), name, job, secrets


class WorkflowSecretEnvironments(unittest.TestCase):
    def test_the_scan_sees_every_known_credential(self) -> None:
        # Control: the scan must find each listed credential somewhere, or a
        # renamed secret would make every assertion below vacuous.
        seen = set()
        for *_, secrets in _jobs():
            seen |= secrets
        self.assertEqual(set(ENVIRONMENT_SECRETS) - seen, set())

    def test_every_credential_is_read_inside_its_environment(self) -> None:
        unknown = []
        misplaced = []
        for wf, _, job_name, job, secrets in _jobs():
            env = _environment(job)
            for secret in sorted(secrets):
                if secret in REPOSITORY_SECRETS:
                    continue
                allowed = ENVIRONMENT_SECRETS.get(secret)
                if allowed is None:
                    unknown.append(f"{wf}:{job_name}:{secret}")
                elif env not in allowed:
                    misplaced.append(f"{wf}:{job_name}:{secret} env={env!r} want one of {sorted(allowed)}")
        self.assertEqual(unknown, [], "classify each new secret in this test")
        self.assertEqual(misplaced, [])

    def test_no_pull_request_run_is_pointed_at_an_environment(self) -> None:
        offenders = []
        for wf, triggers, job_name, job, _ in _jobs():
            if _environment(job) is None or not (triggers & PULL_REQUEST_EVENTS):
                continue
            guard = str(job.get("if") or "")
            if "github.event_name" not in guard:
                offenders.append(f"{wf}:{job_name}")
        self.assertEqual(offenders, [])

    def test_tag_runs_prove_the_commit_landed_before_reading_a_credential(self) -> None:
        # The release environment admits tags v* as well as main, so a tag
        # pushed onto an unmerged commit would reach it. Each such job first
        # proves the tagged commit is on main, before any step that names a
        # credential.
        missing = []
        for path in sorted(WORKFLOWS.glob("*.y*ml")):
            workflow = yaml.safe_load(path.read_text(encoding="utf-8"))
            on = workflow.get(True, workflow.get("on")) or {}
            push = on.get("push") if isinstance(on, dict) else None
            if not (isinstance(push, dict) and push.get("tags")):
                continue
            for name, job in (workflow.get("jobs") or {}).items():
                if _environment(job) is None:
                    continue
                steps = job.get("steps") or []
                names = [str(step.get("name", "")) for step in steps]
                first_secret = next(
                    (i for i, step in enumerate(steps)
                     if set(SECRET_RE.findall(yaml.safe_dump(step))) - {"GITHUB_TOKEN"}),
                    len(steps),
                )
                guard = names.index(TAG_GUARD) if TAG_GUARD in names else None
                if guard is None or guard > first_secret or "merge-base --is-ancestor" not in str(steps[guard].get("run")):
                    missing.append(f"{path.name}:{name}")
        self.assertEqual(missing, [])


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Structural and semantic contract for the required macOS aliases."""

from __future__ import annotations

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / ".github" / "workflows" / "build.yml"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def job_body(text: str, name: str) -> str:
    match = re.search(
        rf"(?ms)^  {re.escape(name)}:\n(.*?)(?=^  [A-Za-z0-9_-]+:\n|\Z)",
        text,
    )
    require(match is not None, f"build.yml must define job {name}")
    return match.group(1)


text = WORKFLOW.read_text(encoding="utf-8")
require(
    re.search(r"(?m)^permissions:\n  actions: read\n  contents: read$", text)
    is not None,
    "jobs API reporters require explicit actions:read and contents:read",
)

pr_alias = job_body(text, "macos")
merge_bootstrap = job_body(text, "macos-merge-group")
build = job_body(text, "build")

require(
    "needs: [resolve-provider, classify]" in pr_alias
    and "needs: [build" not in pr_alias,
    "PR bootstrap must not depend on the advisory build matrix",
)
require("always()" in pr_alias, "PR alias must report after failed dependencies")
require(
    "github.event_name == 'pull_request'" in pr_alias
    and "github.event_name == 'workflow_dispatch'" in pr_alias
    and "native_build_required != 'true'" in pr_alias,
    "PR/dispatch bootstrap must activate only when the native matrix is unavailable",
)
require("gh api" not in pr_alias, "PR bootstrap must not poll matrix jobs")
require(
    "provider resolution did not succeed" in pr_alias
    and "classify did not succeed" in pr_alias,
    "PR bootstrap must fail closed when its dependencies are untrusted",
)
require("Skip-safe PR/dispatch" in pr_alias, "native skip must remain green")
require(
    "macos-pr-unused" in pr_alias,
    "native PRs must not emit a duplicate required context from the bootstrap",
)
require(
    "github.event_name == 'pull_request'" in build
    and "github.event_name == 'workflow_dispatch'" in build
    and "github.event_name == 'merge_group'" in build
    and "matrix.key == 'macos'" in build
    and "&& 'macos'" in build,
    "PR, dispatch, and merge-group macOS legs must own the required context",
)
require(
    "needs: [resolve-provider, classify]" in merge_bootstrap
    and "needs: [build" not in merge_bootstrap,
    "merge bootstrap must not depend on the advisory build matrix",
)
require("gh api" not in merge_bootstrap, "merge bootstrap must not poll jobs")
require(
    "macos-merge-unused" in merge_bootstrap
    and "native_build_required != 'true'" in merge_bootstrap,
    "native merge groups must not emit a duplicate required context",
)
require(
    "provider resolution did not succeed" in merge_bootstrap
    and "classify did not succeed" in merge_bootstrap,
    "merge bootstrap must fail closed when its dependencies are untrusted",
)
require("Skip-safe merge group" in merge_bootstrap, "native skip must remain green")

print("required macos alias contract: ok")

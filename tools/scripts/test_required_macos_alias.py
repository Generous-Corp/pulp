#!/usr/bin/env python3
"""Structural and semantic contract for the required macOS aliases."""

from __future__ import annotations

import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import textwrap


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


def parser_from(job: str) -> str:
    match = re.search(
        r'(?ms)python3 - "\$jobs_json" <<\'PY\'\n(?P<script>.*?)^\s+PY$',
        job,
    )
    require(match is not None, "required alias must parse the jobs response")
    return textwrap.dedent(match.group("script"))


def run_parser(script: str, pages: object) -> subprocess.CompletedProcess[str]:
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8") as fixture:
        if isinstance(pages, list):
            for page in pages:
                json.dump(page, fixture)
                fixture.write("\n")
        else:
            json.dump(pages, fixture)
        fixture.flush()
        return subprocess.run(
            [sys.executable, "-c", script, fixture.name],
            text=True,
            capture_output=True,
            check=False,
        )


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
    "needs: [build, classify]" in pr_alias,
    "PR alias must wait for the terminal build matrix and classification",
)
require("if: always()" in pr_alias, "PR alias must report after failed dependencies")
require("needs.build.result" not in pr_alias, "advisory results may not determine macos")
require(
    "gh api --paginate" in pr_alias
    and "--slurp" not in pr_alias
    and "filter=latest&per_page=100" in pr_alias,
    "PR alias must read every jobs page without new-gh-only --slurp",
)
require("for attempt in 1 2 3" in pr_alias, "PR alias needs bounded transport retries")
require(
    'startswith("macOS")' in pr_alias and "if len(matches) != 1" in pr_alias,
    "PR alias must accept exactly one macOS matrix leg",
)
require('if [ "$conclusion" = "success" ]' in pr_alias, "PR alias accepts only success")
require("cd /tmp" in pr_alias, "PR alias inline Python must use a stable cwd")

require("Skip-safe PR" in pr_alias, "native skip must remain green")
require(
    "classify job did not succeed" in pr_alias,
    "classification uncertainty must remain fail-closed",
)
require(
    "github.event_name == 'merge_group'" in build
    and "matrix.key == 'macos'" in build
    and "&& 'macos'" in build,
    "merge-group macOS matrix leg must own the stable required context",
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
require(
    "Skip-safe merge group" in merge_bootstrap,
    "merge bootstrap must green only the intentional native-skip path",
)

parser = parser_from(pr_alias)

success = run_parser(
    parser,
    [
        {"jobs": [{"name": "Linux (x64)", "conclusion": "success"}]},
        {"jobs": [{"name": "macOS (ARM64) [local]", "conclusion": "success"}]},
    ],
)
require(success.returncode == 0 and success.stdout.strip() == "success", success.stderr)

for conclusion in ("failure", "cancelled", "skipped", None, "unknown"):
    result = run_parser(
        parser,
        [{"jobs": [{"name": "macOS (ARM64) [local]", "conclusion": conclusion}]}],
    )
    require(result.returncode == 0, result.stderr)
    expected = conclusion if isinstance(conclusion, str) else ""
    require(result.stdout.strip() == expected, f"parser changed {conclusion!r}")

for invalid in (
    [{"jobs": []}],
    [
        {
            "jobs": [
                {"name": "macOS (ARM64) [local]", "conclusion": "success"},
                {"name": "macOS duplicate", "conclusion": "success"},
            ]
        }
    ],
    [],
    [[{"name": "macOS (ARM64) [local]", "conclusion": "success"}]],
):
    result = run_parser(parser, invalid)
    require(result.returncode != 0, f"parser accepted invalid payload: {invalid!r}")

print("required macos alias contract: ok")

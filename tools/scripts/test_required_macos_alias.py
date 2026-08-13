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

aliases = {
    "macos": job_body(text, "macos"),
}
for name, job in aliases.items():
    require("if: always()" in job, f"{name} must report after failed dependencies")
    require(
        "needs.build.result" not in job,
        f"{name} must not let advisory matrix conclusions determine macos",
    )
    require(
        "gh api --paginate" in job
        and "--slurp" not in job
        and "filter=latest&per_page=100" in job,
        f"{name} must read every page without requiring new-gh-only --slurp",
    )
    require(
        "for attempt in 1 2 3" in job,
        f"{name} must make exactly three bounded transport attempts",
    )
    require(
        'startswith("macOS")' in job,
        f"{name} must select only the macOS matrix leg",
    )
    require(
        'if [ "$conclusion" = "success" ]' in job,
        f"{name} may accept only the success conclusion",
    )
    require("cd /tmp" in job, f"{name} inline Python must use a stable cwd")

require("Skip-safe PR" in aliases["macos"], "native skip must remain green")
require(
    "needs: [build, classify]" in aliases["macos"],
    "PR reporter must wait for the terminal combined matrix",
)
require(
    "if len(matches) != 1" in aliases["macos"],
    "terminal PR reporter must accept exactly one macOS leg",
)
require(
    "classify job did not succeed" in aliases["macos"],
    "classification uncertainty must remain fail-closed",
)
require(
    "github.event_name == 'merge_group' && matrix.key == 'macos' && 'macos'"
    in job_body(text, "build"),
    "the real merge-group macOS matrix leg must own the stable macos context",
)
fallback = job_body(text, "macos-merge-group-fallback")
require(
    "needs: [resolve-provider, classify]" in fallback,
    "merge-group fallback must not depend on Linux terminality through build",
)
require(
    "PULP_PREAMBLE_RUNS_ON_JSON" in fallback
    and "gh api" not in fallback
    and "while true" not in fallback
    and "sleep 10" not in fallback,
    "merge-group fallback must release the shared preamble immediately",
)
require(
    "needs.resolve-provider.result != 'success'" in fallback
    and "needs.classify.result != 'success'" in fallback
    and "needs.classify.outputs.native_build_required != 'true'" in fallback,
    "merge-group fallback must cover routing/classification failure and skip-safe groups",
)
require(
    "macos-merge-unused" in fallback,
    "native merge groups must not get a second required macos context",
)

parsers = {name: parser_from(job) for name, job in aliases.items()}
parser = parsers["macos"]

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

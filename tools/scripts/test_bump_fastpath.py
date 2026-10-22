#!/usr/bin/env python3
"""The bump fast-path report, its classify notice, and the advisory skips.

- `build.yml`'s classify step reports `bump-fast-path: taken|refused: <why>`
  for the release bot's bump only (run here in Bash with a fake verifier).
- `bump_fastpath_report.py` counts those notices.
- The four advisory workflows skip every job on `release/version-bump`, still
  run for any other pull request, and none of their contexts is required.
"""

from __future__ import annotations

import importlib.util
import json
import os
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / ".github" / "workflows" / "build.yml"
RULESET = ROOT / ".github" / "rulesets" / "main-protection.json"
WEEKLY = ROOT / ".github" / "workflows" / "bump-fastpath-weekly.yml"
ADVISORY = {
    "coverage.yml": "Coverage",
    "control-shipping-native.yml": "Control shipping native matrix",
    "examples-validation.yml": "Example validation",
    "intel-portability.yml": "Intel portability (advisory)",
}
GUARD = "github.head_ref != 'release/version-bump'"

SPEC = importlib.util.spec_from_file_location(
    "bump_fastpath_report", ROOT / "tools" / "scripts" / "bump_fastpath_report.py")
assert SPEC is not None and SPEC.loader is not None
report = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(report)

SHA = "a" * 40
CANDIDATE = "b" * 40


def classify_snippet() -> str:
    """The verifier call and its report, from build.yml's classify step."""
    text = BUILD.read_text(encoding="utf-8")
    start = text.index("              # Why the fast path was not taken")
    end = text.index("            fi\n          else\n", start)
    return "\n".join(line[14:] if line.startswith(" " * 14) else line
                     for line in text[start:end].split("\n"))


class ClassifyNotice(unittest.TestCase):
    """Run the reporting block with a fake verifier and a fake `git`."""

    def run_block(self, *, event: str, head_ref: str = "", verifier_rc: int = 1,
                  verifier_err: str = "", trusted: str = SHA, subject: str = "",
                  candidate: str = "") -> tuple[str, str, str]:
        with tempfile.TemporaryDirectory() as scratch:
            work = Path(scratch)
            summary = work / "summary.md"
            fake_python = work / "python"
            fake_python.write_text(
                "#!/bin/bash\n"
                f"printf '%b' {json.dumps(verifier_err.replace(chr(10), chr(92) + 'n'))} >&2\n"
                f"exit {verifier_rc}\n", encoding="utf-8")
            fake_python.chmod(0o755)
            script = (
                "set -euo pipefail\n"
                "git() {\n"
                '  case "$1" in\n'
                "    show) echo 'verifier source' ;;\n"
                f"    log) printf '%s\\n' {json.dumps(subject)} ;;\n"
                "  esac\n"
                "}\n"
                f"ci_python={fake_python}\n"
                "trusted_bump_check=$(mktemp)\nbump_check_err=$(mktemp)\n"
                f"trusted_verifier_base={trusted}\n"
                f"EVENT_NAME={event}\nEVENT_PATH=/dev/null\nbase={SHA}\n"
                f"EVENT_HEAD_SHA={SHA}\naccept_pr_merge_ref=\n"
                f"candidate={candidate}\n"
                "native_build_required=true\n"
                + classify_snippet().replace("${{ github.token }}", "token")
                + '\necho "native_build_required=$native_build_required"\n')
            result = subprocess.run(
                ["/bin/bash", "-c", script], capture_output=True, text=True, encoding="utf-8",
                timeout=60,
                env={"PATH": "/usr/bin:/bin", "GITHUB_HEAD_REF": head_ref,
                     "GITHUB_STEP_SUMMARY": str(summary)})
            self.assertEqual(result.returncode, 0, result.stderr)
            return (result.stdout, result.stderr,
                    summary.read_text(encoding="utf-8") if summary.exists() else "")

    def test_a_bump_refused_by_the_verifier_reports_its_reason(self) -> None:
        out, err, summary = self.run_block(
            event="pull_request", head_ref="release/version-bump",
            verifier_err="generated-version-bump: full validation "
                         "(candidate tree differs from trusted byte regeneration)\n")
        self.assertIn("::notice title=bump-fast-path::refused: verifier: candidate tree "
                      "differs from trusted byte regeneration", out)
        self.assertIn("native_build_required=true", out)
        self.assertIn("bump fast path (pull_request, aaaaaaaaaaaa): refused: verifier:",
                      summary)
        self.assertIn("full validation", err, "the verifier's own line is kept")

    def test_a_bump_the_verifier_accepts_reports_taken(self) -> None:
        out, _, summary = self.run_block(event="pull_request",
                                         head_ref="release/version-bump", verifier_rc=0)
        self.assertIn("::notice title=bump-fast-path::taken", out)
        self.assertIn("native_build_required=false", out)
        self.assertIn("): taken", summary)

    def test_a_merge_group_the_verifier_never_ran_for_reports_its_shape(self) -> None:
        # Main moved three merges past the bump's parent between generation
        # and the group, so no trusted base was resolved and nothing said why.
        out, _, _ = self.run_block(event="merge_group", trusted="",
                                   candidate=CANDIDATE, subject="chore: bump versions")
        self.assertIn("::notice title=bump-fast-path::refused: shape: no trusted "
                      "verifier base", out)
        self.assertIn("native_build_required=true", out)

    def test_other_pull_requests_and_groups_report_nothing(self) -> None:
        for kwargs in ({"event": "pull_request", "head_ref": "feature/x"},
                       {"event": "merge_group", "candidate": CANDIDATE,
                        "subject": "feat: something"},
                       {"event": "merge_group", "trusted": ""}):
            with self.subTest(**kwargs):
                out, _, summary = self.run_block(**kwargs)
                self.assertNotIn("bump-fast-path", out)
                self.assertEqual(summary, "")

    def test_a_reason_with_newlines_and_percent_is_one_safe_notice(self) -> None:
        out, _, _ = self.run_block(
            event="pull_request", head_ref="release/version-bump",
            verifier_err="generated-version-bump: full validation (100% off\n)\n")
        notices = [line for line in out.splitlines() if "bump-fast-path" in line]
        self.assertEqual(len(notices), 1, out)
        self.assertNotIn("100% ", notices[0])


class Report(unittest.TestCase):
    def fetcher(self, responses: dict[str, object]):
        def fetch(endpoint: str) -> object:
            for prefix, body in responses.items():
                if endpoint.startswith(prefix):
                    return body
            raise AssertionError(f"unexpected endpoint {endpoint}")
        return fetch

    def test_outcome_reads_the_last_notice_and_defaults_to_unreported(self) -> None:
        self.assertEqual(report.outcome_of([]), "unreported")
        self.assertEqual(report.outcome_of([
            {"title": "other", "message": "x"},
            {"title": "bump-fast-path", "message": "refused: shape: y"},
            {"title": "bump-fast-path", "message": "taken"}]), "taken")

    def test_bump_runs_are_found_by_branch_and_counted(self) -> None:
        repo = "o/r"
        runs = {"workflow_runs": [
            {"id": 1, "head_branch": "gh-readonly-queue/main/pr-10-" + SHA,
             "conclusion": "success"},
            {"id": 2, "head_branch": "gh-readonly-queue/main/pr-11-" + SHA,
             "conclusion": "success"},
            {"id": 3, "head_branch": "release/version-bump", "conclusion": "success"},
        ]}
        jobs = {"jobs": [
            {"id": 91, "name": "classify"},
            {"id": 92, "name": "macos", "conclusion": "success",
             "started_at": "2026-10-22T06:00:00Z", "completed_at": "2026-10-22T06:30:00Z"},
        ]}
        fetch = self.fetcher({
            f"repos/{repo}/actions/workflows/build.yml/runs?event=merge_group": runs,
            f"repos/{repo}/actions/workflows/build.yml/runs?event=pull_request":
                {"workflow_runs": []},
            f"repos/{repo}/pulls/10": {"head": {"ref": "release/version-bump"}},
            f"repos/{repo}/pulls/11": {"head": {"ref": "feature/x"}},
            f"repos/{repo}/actions/runs/": jobs,
            f"repos/{repo}/check-runs/91/annotations":
                [{"title": "bump-fast-path", "message": "refused: shape: moved"}],
        })
        from datetime import datetime, timezone
        records = report.bump_runs(fetch, repo, datetime(2026, 10, 15, tzinfo=timezone.utc),
                                   report.pull_is_bump(fetch, repo))
        self.assertEqual([r["run"] for r in records], [1, 3])
        summary = report.summarize(records)
        self.assertEqual(summary["refused"], 2)
        self.assertEqual(summary["refusal_reasons"], {"shape: moved": 2})
        self.assertEqual(summary["macos_minutes_on_refused"], 60.0)
        self.assertIn("2 refused", report.render(summary, 7))


class AdvisorySkips(unittest.TestCase):
    def jobs(self, name: str) -> dict:
        return yaml.safe_load((ROOT / ".github" / "workflows" / name).read_text(encoding="utf-8"))["jobs"]

    def test_every_job_skips_the_bump_and_keeps_its_condition_otherwise(self) -> None:
        for workflow in ADVISORY:
            for job, spec in self.jobs(workflow).items():
                with self.subTest(workflow=workflow, job=job):
                    condition = str(spec.get("if", ""))
                    if condition.startswith("${{") and condition.endswith("}}"):
                        condition = condition[3:-2].strip()
                    # The guard is ANDed in front, so on release/version-bump
                    # the job is false whatever else holds, and for any other
                    # head (or none, on a schedule) the guard is true and the
                    # rest is the job's original condition.
                    self.assertTrue(condition == GUARD or condition.startswith(GUARD + " && "),
                                    condition)
                    rest = condition[len(GUARD + " && "):] if condition != GUARD else ""
                    if rest and not (rest.startswith("(") and rest.endswith(")")):
                        self.assertNotIn("||", rest, "an unparenthesized || would bind past the guard")
                    self.assertEqual(rest.count("("), rest.count(")"))

    def test_the_guard_is_false_only_on_the_bump_branch(self) -> None:
        def guard(head_ref: str) -> bool:
            return head_ref != "release/version-bump"
        self.assertFalse(guard("release/version-bump"))
        for head_ref in ("", "feature/x", "release/version-bump-2", "release/other"):
            self.assertTrue(guard(head_ref), head_ref)
        self.assertEqual(GUARD, "github.head_ref != 'release/version-bump'")

    def test_non_bump_pull_requests_still_trigger_each_workflow(self) -> None:
        for workflow in ADVISORY:
            with self.subTest(workflow=workflow):
                doc = yaml.safe_load((ROOT / ".github" / "workflows" / workflow).read_text(encoding="utf-8"))
                on = doc.get("on", doc.get(True))
                self.assertIn("pull_request", on)
                self.assertEqual(on["pull_request"].get("types"), ["auto_merge_enabled"])
                self.assertNotIn("branches-ignore", on["pull_request"])

    def test_no_skipped_context_is_required(self) -> None:
        ruleset = json.loads(RULESET.read_text(encoding="utf-8"))
        required = {check["context"] for rule in ruleset["rules"]
                    if rule["type"] == "required_status_checks"
                    for check in rule["parameters"]["required_status_checks"]}
        self.assertIn("macos", required, "control: the required list was read")
        for workflow in ADVISORY:
            for job, spec in self.jobs(workflow).items():
                with self.subTest(workflow=workflow, job=job):
                    for name in self.context_names(job, spec):
                        self.assertNotIn(name, required)

    @staticmethod
    def context_names(job: str, spec: dict) -> list[str]:
        """The check names a job reports: its name with each literal matrix
        value filled in, and a wildcard where the matrix is computed."""
        name = str(spec.get("name", job))
        fields = re.findall(r"\$\{\{ matrix\.(\w+) \}\}", name)
        if not fields:
            return [name]
        matrix = (spec.get("strategy") or {}).get("matrix")
        rows = matrix.get("include", []) if isinstance(matrix, dict) else []
        if not rows:
            # A computed matrix (fromJSON): any value is possible, so no
            # required context may match the name's fixed text.
            return [re.sub(r"\$\{\{ matrix\.\w+ \}\}", "*", name)]
        names = []
        for row in rows:
            filled = name
            for field in fields:
                filled = filled.replace("${{ matrix.%s }}" % field, str(row.get(field)))
            names.append(filled)
        return names

    def test_the_weekly_count_runs_the_report(self) -> None:
        doc = yaml.safe_load(WEEKLY.read_text(encoding="utf-8"))
        self.assertIn("schedule", doc.get("on", doc.get(True)))
        steps = [s.get("run", "") for job in doc["jobs"].values() for s in job["steps"]]
        self.assertTrue(any("tools/scripts/bump_fastpath_report.py" in s for s in steps))
        job = next(iter(doc["jobs"].values()))
        self.assertIn("github.repository", job.get("if", ""), "fork-guarded")
        self.assertEqual(job.get("runs-on"), "ubuntu-latest")


if __name__ == "__main__":
    unittest.main()

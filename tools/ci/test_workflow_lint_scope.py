#!/usr/bin/env python3
"""Tests for workflow-lint.yml's event coverage, scope and aggregate verdict.

Holds .github/workflows/workflow-lint.yml to the shape a required check needs:
it triggers on merge_group, no trigger carries a `paths:` filter, and the
`workflow-lint` aggregate runs under `if: always()` over every suite job. The
scope and verdict functions are exercised directly, and the structural checks
are run against mutated copies of the workflow (a `paths:` filter put back, the
merge_group trigger removed, the aggregate's `always()` dropped) so each one is
shown able to fail.

Needs PyYAML.

    python3 tools/ci/test_workflow_lint_scope.py
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import yaml

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "ci"))

import workflow_lint_scope as wls  # noqa: E402

WORKFLOW = REPO / ".github" / "workflows" / "workflow-lint.yml"
AGGREGATE = "workflow-lint"


def triggers(doc: dict) -> dict:
    return doc.get("on", doc.get(True)) or {}


def shape_problems(doc: dict) -> list[str]:
    """What keeps this workflow from being a safe required check."""
    problems: list[str] = []
    on = triggers(doc)
    for event in ("pull_request", "merge_group"):
        if event not in on:
            problems.append(f"no {event} trigger")
    for event, cfg in on.items():
        if isinstance(cfg, dict) and ({"paths", "paths-ignore"} & set(cfg)):
            problems.append(f"{event} carries a paths filter")
    if "branches" in (on.get("pull_request") or {}):
        problems.append("pull_request is branch-filtered")
    jobs = doc.get("jobs") or {}
    agg = jobs.get(AGGREGATE)
    if agg is None:
        problems.append(f"no {AGGREGATE} job")
        return problems
    if agg.get("name") != AGGREGATE:
        problems.append(f"{AGGREGATE} job is not named {AGGREGATE}")
    if str(agg.get("if", "")).strip() != "always()":
        problems.append(f"{AGGREGATE} does not run under always()")
    needs = agg.get("needs") or []
    needs = [needs] if isinstance(needs, str) else needs
    for name in jobs:
        if name != AGGREGATE and name not in needs:
            problems.append(f"{AGGREGATE} does not wait for {name}")
    scope = jobs.get("scope") or {}
    if "if" in scope:
        problems.append("scope job is conditional")
    return problems


class WorkflowShapeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.text = WORKFLOW.read_text(encoding="utf-8")
        self.doc = yaml.safe_load(self.text)

    def test_live_workflow_is_a_safe_required_check(self) -> None:
        self.assertEqual(shape_problems(self.doc), [])

    def test_suites_are_gated_on_scope(self) -> None:
        jobs = self.doc["jobs"]
        suites = [n for n in jobs if n not in ("scope", AGGREGATE)]
        self.assertIn("lint", suites)
        self.assertIn("generated-version-bump-contract", suites)
        for name in suites:
            self.assertEqual(jobs[name].get("needs"), "scope", name)
            self.assertEqual(jobs[name].get("if"), "needs.scope.outputs.run == 'true'", name)

    def test_aggregate_passes_every_suite_result_to_the_verdict(self) -> None:
        agg = self.doc["jobs"][AGGREGATE]
        body = "\n".join(str(step.get("run", "")) for step in agg["steps"])
        env = {}
        for step in agg["steps"]:
            env.update(step.get("env") or {})
        self.assertIn("workflow_lint_scope.py verdict", body)
        for name in self.doc["jobs"]:
            if name in ("scope", AGGREGATE):
                continue
            self.assertIn(f"--job \"{name}=$", body, name)
            self.assertTrue(any(f"needs.{name}.result" in str(v) for v in env.values()), name)

    def test_scope_reads_the_merge_group_range(self) -> None:
        env = self.doc["jobs"]["scope"]["steps"][1]["env"]
        self.assertEqual(env["MQ_BASE"], "${{ github.event.merge_group.base_sha }}")
        self.assertEqual(env["MQ_HEAD"], "${{ github.event.merge_group.head_sha }}")

    # Negative controls: each structural rule fails on a copy that breaks it.
    def _mutated(self, old: str, new: str) -> dict:
        self.assertIn(old, self.text)
        return yaml.safe_load(self.text.replace(old, new, 1))

    def test_break_paths_filter_is_refused(self) -> None:
        doc = self._mutated("  pull_request:\n", "  pull_request:\n    paths: ['.github/**']\n")
        self.assertIn("pull_request carries a paths filter", shape_problems(doc))

    def test_break_missing_merge_group_is_refused(self) -> None:
        doc = self._mutated("  merge_group:\n", "")
        self.assertIn("no merge_group trigger", shape_problems(doc))

    def test_break_conditional_aggregate_is_refused(self) -> None:
        old = "    needs: [scope, lint, generated-version-bump-contract]\n    if: always()\n"
        doc = self._mutated(old, old.replace("always()", "success()"))
        self.assertIn(f"{AGGREGATE} does not run under always()", shape_problems(doc))

    def test_break_aggregate_not_waiting_for_a_suite_is_refused(self) -> None:
        doc = self._mutated("needs: [scope, lint, generated-version-bump-contract]",
                            "needs: [scope, lint]")
        self.assertIn(f"{AGGREGATE} does not wait for generated-version-bump-contract",
                      shape_problems(doc))


class PathsFileTests(unittest.TestCase):
    def test_paths_file_names_the_workflow_surfaces(self) -> None:
        globs = wls.load_globs()
        self.assertGreater(len(globs), 100)
        for g in (".github/workflows/**", ".github/actions/**", "tools/ci/**",
                  ".github/actionlint.yaml", ".github/rulesets/**"):
            self.assertIn(g, globs)

    def test_every_literal_path_exists(self) -> None:
        missing = [g for g in wls.load_globs()
                   if not any(ch in g for ch in "*?[") and not (REPO / g).exists()]
        self.assertEqual(missing, [])

    def test_this_suite_and_its_inputs_are_in_scope(self) -> None:
        globs = wls.load_globs()
        for path in ("tools/ci/workflow_lint_scope.py", "tools/ci/workflow_lint_paths.txt",
                     "tools/ci/release_path_changed.py", ".github/workflows/workflow-lint.yml"):
            self.assertTrue(wls.decide([path], globs), path)


class ScopeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.globs = wls.load_globs()

    def test_incident_change_is_in_scope(self) -> None:
        # The 2026-11-10 pair both edited workflows and their lint suites.
        self.assertTrue(wls.decide([".github/workflows/build.yml"], self.globs))
        self.assertTrue(wls.decide(["tools/ci/flake_exoneration.py"], self.globs))

    def test_negative_control_source_change_is_out_of_scope(self) -> None:
        self.assertFalse(wls.decide(["core/view/src/widgets.cpp", "docs/guides/ci.md"], self.globs))

    def test_unread_diff_runs_every_suite(self) -> None:
        self.assertTrue(wls.decide(None, self.globs))

    def test_compare_cap_runs_every_suite(self) -> None:
        files = [f"core/x{i}.cpp" for i in range(300)]
        self.assertTrue(wls.decide(files, self.globs))
        self.assertFalse(wls.decide(files[:299], self.globs))

    def test_event_range(self) -> None:
        self.assertEqual(wls.event_range({"EVENT": "merge_group", "MQ_BASE": "a", "MQ_HEAD": "b"}), ("a", "b"))
        self.assertEqual(wls.event_range({"EVENT": "pull_request", "PR_BASE": "a", "PR_HEAD": "b"}), ("a", "b"))
        self.assertEqual(wls.event_range({"EVENT": "push", "PUSH_BEFORE": "a", "PUSH_AFTER": "b"}), ("a", "b"))
        self.assertIsNone(wls.event_range({"EVENT": "push", "PUSH_BEFORE": wls.ZERO_SHA, "PUSH_AFTER": "b"}))
        self.assertIsNone(wls.event_range({"EVENT": "merge_group", "MQ_BASE": "", "MQ_HEAD": "b"}))
        self.assertIsNone(wls.event_range({"EVENT": "workflow_dispatch"}))

    def test_rename_is_judged_by_both_paths(self) -> None:
        payload = '{"files": [{"filename": "core/a.cpp", "previous_filename": ".github/workflows/x.yml"}]}'
        done = subprocess.CompletedProcess([], 0, stdout=payload, stderr="")
        with mock.patch.object(wls.subprocess, "run", return_value=done):
            files = wls.compare_files("o/r", "a", "b")
        self.assertEqual(files, ["core/a.cpp", ".github/workflows/x.yml"])
        self.assertTrue(wls.decide(files, self.globs))

    def test_compare_failure_runs_every_suite(self) -> None:
        env = {"EVENT": "merge_group", "MQ_BASE": "a", "MQ_HEAD": "b"}
        with mock.patch.dict(os.environ, env), \
                mock.patch.object(wls.subprocess, "run", side_effect=subprocess.CalledProcessError(1, "gh")), \
                mock.patch("builtins.print") as printed:
            self.assertEqual(wls.main(["scope", "--from-event", "--repo", "o/r"]), 0)
        self.assertIn("true", [c.args[0] for c in printed.call_args_list])

    def test_files_cli(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            changed = Path(tmp) / "changed.txt"
            changed.write_text("core/a.cpp\n", encoding="utf-8")
            out = subprocess.run([sys.executable, str(REPO / "tools/ci/workflow_lint_scope.py"),
                                  "scope", "--files", str(changed)],
                                 capture_output=True, text=True, check=True)
            self.assertEqual(out.stdout.strip(), "false")
            changed.write_text("core/a.cpp\n.github/workflows/build.yml\n", encoding="utf-8")
            out = subprocess.run([sys.executable, str(REPO / "tools/ci/workflow_lint_scope.py"),
                                  "scope", "--files", str(changed)],
                                 capture_output=True, text=True, check=True)
            self.assertEqual(out.stdout.strip(), "true")


class VerdictTests(unittest.TestCase):
    JOBS = ("lint", "generated-version-bump-contract")

    def v(self, scope: str, run: str, *results: str) -> int:
        return wls.verdict(scope, run, dict(zip(self.JOBS, results)))[0]

    def test_verdict_table(self) -> None:
        # Out of scope: skipped suites pass, so the context always reports.
        self.assertEqual(self.v("success", "false", "skipped", "skipped"), 0)
        self.assertEqual(self.v("success", "true", "success", "success"), 0)
        # Negative controls: anything else fails.
        self.assertEqual(self.v("success", "true", "failure", "success"), 1)
        self.assertEqual(self.v("success", "true", "success", "cancelled"), 1)
        self.assertEqual(self.v("success", "true", "skipped", "success"), 1)
        self.assertEqual(self.v("success", "false", "success", "skipped"), 1)
        self.assertEqual(self.v("failure", "", "skipped", "skipped"), 1)
        self.assertEqual(self.v("cancelled", "true", "skipped", "skipped"), 1)
        self.assertEqual(self.v("success", "", "skipped", "skipped"), 1)

    def test_verdict_cli_exit_codes(self) -> None:
        with mock.patch("builtins.print"):
            self.assertEqual(wls.main(["verdict", "--scope-result", "success", "--run", "false",
                                       "--job", "lint=skipped"]), 0)
            self.assertEqual(wls.main(["verdict", "--scope-result", "success", "--run", "true",
                                       "--job", "lint=failure"]), 1)
            self.assertEqual(wls.main(["verdict", "--scope-result", "success", "--run", "true"]), 2)


if __name__ == "__main__":
    unittest.main()

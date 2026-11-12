#!/usr/bin/env python3
"""Tests for tools/ci/drift_fast.py and its workflow/manifest contract."""

from __future__ import annotations

import contextlib
import io
import json
import re
import pathlib
import sys
import tempfile
import unittest
import unittest.mock

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import drift_fast  # noqa: E402


def registrations(**tests: list[str]) -> dict:
    return {
        "tests": [
            {"name": name, "properties": [{"name": "LABELS", "value": labels}]}
            for name, labels in tests.items()
        ]
    }


def quietly(fn, *args):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = fn(*args)
    return rc, out.getvalue() + err.getvalue()


class SelectionTests(unittest.TestCase):
    manifest = {
        "schema_version": 1,
        "ctest_labels": ["pr-fast"],
        "tests": [
            {"name": "wide-non-native-selftest", "why": "x"},
            {"name": "ios-compile-gate-legs", "why": "apple only"},
        ],
    }

    def test_label_members_and_listed_tests_are_unioned(self):
        reg = drift_fast.parse_registrations(
            registrations(**{"a-drift": ["pr-fast"], "wide-non-native-selftest": [], "other": ["x"]})
        )
        selected, missing, empty = drift_fast.select(self.manifest, reg)
        self.assertEqual(selected, ["a-drift", "wide-non-native-selftest"])
        self.assertEqual(missing, ["ios-compile-gate-legs"])
        self.assertEqual(empty, [])

    def test_label_matching_nothing_is_reported(self):
        reg = drift_fast.parse_registrations(registrations(**{"wide-non-native-selftest": []}))
        _, _, empty = drift_fast.select(self.manifest, reg)
        self.assertEqual(empty, ["pr-fast"])

    def test_command_naming_an_unbuilt_artifact_is_excluded(self):
        with tempfile.TemporaryDirectory() as tmp:
            build = pathlib.Path(tmp)
            (build / "generated.py").write_text("", encoding="utf-8")
            doc = {
                "tests": [
                    {"name": "needs-binary", "command": ["python3", str(build / "generated.py"),
                                                         str(build / "test" / "pulp-test-x")]},
                    {"name": "source-only", "command": ["python3", "/src/tools/x.py",
                                                        "--build-dir", str(build)]},
                ]
            }
            unbuilt = drift_fast.unbuilt_artifacts(doc, build)
        self.assertEqual(list(unbuilt), ["needs-binary"])
        self.assertTrue(unbuilt["needs-binary"].endswith("pulp-test-x"))

    def test_exact_regex_matches_only_named_tests(self):
        import re

        rx = re.compile(drift_fast.exact_regex(["a.b", "c"]))
        self.assertTrue(rx.match("a.b"))
        self.assertFalse(rx.match("aXb"))
        self.assertFalse(rx.match("c-selftest"))

    def test_manifest_rejects_duplicates_and_missing_reason(self):
        problems = drift_fast.manifest_problems(
            {"schema_version": 1, "tests": [{"name": "a", "why": "x"}, {"name": "a"}]}
        )
        self.assertIn("a: missing 'why'", problems)
        self.assertIn("a: listed twice", problems)

    def test_checked_in_manifest_is_valid(self):
        self.assertEqual(drift_fast.manifest_problems(drift_fast.load_manifest()), [])


class VerdictTests(unittest.TestCase):
    PASS = "100% tests passed, 0 tests failed out of 3\n"
    FAIL = "67% tests passed, 1 tests failed out of 3\n"

    def test_pass(self):
        rc, _ = quietly(drift_fast.verdict, 0, self.PASS, 3)
        self.assertEqual(rc, 0)

    def test_pass_in_recent_ctest_format(self):
        rc, _ = quietly(drift_fast.verdict, 0, "100% tests passed out of 3\n", 3)
        self.assertEqual(rc, 0)

    def test_failure(self):
        rc, out = quietly(drift_fast.verdict, 8, self.FAIL, 3)
        self.assertEqual(rc, 1)
        self.assertIn("FAILED", out)

    def test_count_mismatch_is_not_a_pass(self):
        rc, out = quietly(drift_fast.verdict, 0, "100% tests passed, 0 tests failed out of 2\n", 3)
        self.assertEqual(rc, 2)
        self.assertIn("ran 2 test(s) but 3 were selected", out)

    def test_no_summary_is_not_a_pass(self):
        rc, _ = quietly(drift_fast.verdict, 0, "No tests were found!!!\n", 3)
        self.assertEqual(rc, 2)

    def test_skips_are_named(self):
        output = (
            " 1/3 Test #7: consumption-census-drift .....***Skipped   0.30 sec\n" + self.PASS
        )
        rc, out = quietly(drift_fast.verdict, 0, output, 3)
        self.assertEqual(rc, 0)
        self.assertIn("NOT CHECKED (skipped): consumption-census-drift", out)


class ContractTests(unittest.TestCase):
    def check(self, workflow_text: str) -> tuple[int, str]:
        with tempfile.TemporaryDirectory() as tmp:
            wf = pathlib.Path(tmp) / "drift-fast.yml"
            wf.write_text(workflow_text, encoding="utf-8")
            args = drift_fast.argparse.Namespace(manifest=str(drift_fast.MANIFEST), workflow=str(wf))
            return quietly(drift_fast.check, args)

    def test_checked_in_workflow_satisfies_contract(self):
        args = drift_fast.argparse.Namespace(
            manifest=str(drift_fast.MANIFEST), workflow=str(drift_fast.WORKFLOW)
        )
        rc, out = quietly(drift_fast.check, args)
        self.assertEqual(rc, 0, out)

    def test_workflow_without_merge_group_fails(self):
        text = drift_fast.WORKFLOW.read_text(encoding="utf-8").replace("  merge_group:\n", "")
        rc, out = self.check(text)
        self.assertEqual(rc, 1)
        self.assertIn("does not trigger on merge_group", out)

    def test_workflow_with_path_filter_fails(self):
        text = drift_fast.WORKFLOW.read_text(encoding="utf-8").replace(
            "  merge_group:\n", "    paths: ['core/**']\n  merge_group:\n"
        )
        rc, out = self.check(text)
        self.assertEqual(rc, 1)
        self.assertIn("filters its trigger with paths:", out)

    def test_workflow_with_conditional_job_fails(self):
        runs_on = re.search(r"^    runs-on: .*\n", drift_fast.WORKFLOW.read_text(encoding="utf-8"), re.M).group(0)
        text = drift_fast.WORKFLOW.read_text(encoding="utf-8").replace(
            runs_on, "    if: github.event_name == 'merge_group'\n" + runs_on
        )
        rc, out = self.check(text)
        self.assertEqual(rc, 1)
        self.assertIn("job-level if:", out)

    def test_runner_is_hosted_unless_the_routing_variable_is_set(self):
        # With PULP_DRIFT_FAST_RUNS_ON_JSON unset the expression's fallback is
        # what GitHub parses; it must be the hosted label, as JSON.
        text = drift_fast.WORKFLOW.read_text(encoding="utf-8")
        match = re.search(
            r"runs-on: \$\{\{ fromJSON\(.*vars\.PULP_DRIFT_FAST_RUNS_ON_JSON \|\| '([^']*)'\) \}\}", text)
        self.assertIsNotNone(match, "drift-fast must route through PULP_DRIFT_FAST_RUNS_ON_JSON")
        self.assertEqual(json.loads(match.group(1)), "ubuntu-latest")
        self.assertEqual(text.count("runs-on:"), 1)

    def test_workflow_not_calling_run_fails(self):
        text = drift_fast.WORKFLOW.read_text(encoding="utf-8").replace(
            "tools/ci/drift_fast.py run", "true"
        )
        rc, out = self.check(text)
        self.assertEqual(rc, 1)
        self.assertIn("never calls", out)

    HYDRATE = "tools/scripts/hydrate_gpu_provenance_commits.py"

    def test_workflow_without_hydration_fails(self):
        text = drift_fast.WORKFLOW.read_text(encoding="utf-8")
        text = text.replace(f"python3 {self.HYDRATE}", "true")
        rc, out = self.check(text)
        self.assertEqual(rc, 1)
        self.assertIn(f"never runs precondition {self.HYDRATE}", out)

    def test_hydration_named_only_in_a_comment_fails(self):
        text = drift_fast.WORKFLOW.read_text(encoding="utf-8")
        text = text.replace(f"python3 {self.HYDRATE}", f"true\n      # {self.HYDRATE}")
        rc, out = self.check(text)
        self.assertEqual(rc, 1)
        self.assertIn("never runs precondition", out)

    def test_hydration_after_the_driver_fails(self):
        text = drift_fast.WORKFLOW.read_text(encoding="utf-8").replace(
            f"python3 {self.HYDRATE}", "true"
        )
        text += f"\n      - run: python3 {self.HYDRATE}\n"
        rc, out = self.check(text)
        self.assertEqual(rc, 1)
        self.assertIn("after tools/ci/drift_fast.py run", out)

    def test_checked_in_manifest_requires_hydration(self):
        steps = [e["step"] for e in drift_fast.load_manifest().get("workflow_preconditions", [])]
        self.assertIn(self.HYDRATE, steps)

    def test_precondition_without_reason_is_rejected(self):
        problems = drift_fast.manifest_problems(
            {"schema_version": 1, "workflow_preconditions": [{"step": "x"}],
             "tests": [{"name": "a", "why": "x"}]}
        )
        self.assertIn("workflow_preconditions entries need a 'step' and a 'why'", problems)

    def test_every_listed_test_is_a_real_registration_name(self):
        # Guards typos: each listed name must appear in some test manifest.
        sources = "\n".join(
            p.read_text(encoding="utf-8")
            for p in (drift_fast.REPO_ROOT / "test").rglob("*.cmake")
        )
        sources += (drift_fast.REPO_ROOT / "test" / "CMakeLists.txt").read_text(encoding="utf-8")
        for entry in json.loads(drift_fast.MANIFEST.read_text(encoding="utf-8"))["tests"]:
            self.assertIn(f"NAME {entry['name']}", sources, entry["name"])


class DocsOnlyScopeTests(unittest.TestCase):
    """drift_fast.py scope: source mode only for docs-only diffs."""

    PROSE = ["docs/reports/a9-edit-replay-evidence-20261112.md"]

    def test_code_diff_takes_the_full_lane(self):
        # Positive control: every code-touching diff keeps today's full lane.
        for files in (["core/view/src/widgets.cpp"],
                      ["test/cmake/pr_fast_tests.cmake"],
                      ["tools/ci/drift_fast.json"],
                      ["docs/reports/x.md", "tools/scripts/gates.sh"],
                      ["docs/status/consumption-profiles.json"],
                      ["docs/migrations/2026-11-01-x.md"]):
            mode, entries, why = drift_fast.docs_only_plan(files)
            self.assertEqual(mode, "full", (files, why))
            self.assertEqual(entries, [])

    def test_unreadable_diff_takes_the_full_lane(self):
        self.assertEqual(drift_fast.docs_only_plan(None)[0], "full")
        self.assertEqual(drift_fast.docs_only_plan([])[0], "full")

    def test_prose_diff_runs_its_readers_from_source(self):
        mode, entries, why = drift_fast.docs_only_plan(self.PROSE)
        self.assertEqual(mode, "source", why)
        names = {e["name"] for e in entries}
        for reader in ("skills-doc-sync", "tools-registry-check"):
            self.assertIn(reader, names)
        # Nothing it runs needs a configure.
        for entry in entries:
            self.assertFalse(any("${" in a or "$<" in a for a in entry["argv"]), entry)

    def test_without_the_reviewed_exemptions_prose_takes_the_full_lane(self):
        # Negative control: the exemption list is what admits source mode.
        data = drift_fast.load_manifest()
        data = {**data, "docs_only": {"prose_not_readers": []}}
        mode, _, why = drift_fast.docs_only_plan(self.PROSE, data=data)
        self.assertEqual(mode, "full")
        self.assertIn("script-test-inputs-drift", why)

    def test_exemptions_do_not_cover_docs_outside_prose(self):
        # CLAUDE.md, a SKILL.md or docs/status/ data are read by configured
        # tests the exemptions do not speak for.
        for files in (["CLAUDE.md"], [".agents/skills/ci/SKILL.md"],
                      ["docs/status/tools.yaml"]):
            self.assertEqual(drift_fast.docs_only_plan(files)[0], "full", files)

    def test_scope_error_takes_the_full_lane(self):
        with unittest.mock.patch.object(drift_fast, "_source_selftests",
                                        side_effect=RuntimeError("boom")):
            mode, _, why = drift_fast.docs_only_plan(self.PROSE)
        self.assertEqual(mode, "full")
        self.assertIn("boom", why)

    def test_exempt_names_are_real_drift_tests(self):
        ss = drift_fast._source_selftests()
        data = drift_fast.load_manifest()
        known = set(ss.pr_fast_names()) | {t["name"] for t in data["tests"]}
        for entry in data["docs_only"]["prose_not_readers"]:
            self.assertIn(entry["name"], known)

    def test_ungated_full_run_breaks_the_contract(self):
        text = drift_fast.WORKFLOW.read_text(encoding="utf-8")
        self.assertEqual(drift_fast.scope_problems(text, "w"), [])
        broken = text.replace("        if: steps.scope.outputs.mode != 'source'\n"
                              "        run: |\n"
                              "          python3 tools/ci/drift_fast.py run --build-dir",
                              "        run: |\n"
                              "          python3 tools/ci/drift_fast.py run --build-dir")
        self.assertNotEqual(broken, text)
        self.assertTrue(drift_fast.scope_problems(broken, "w"))

    def test_run_source_refuses_a_full_diff(self):
        with unittest.mock.patch.object(drift_fast, "docs_only_plan",
                                        return_value=("full", [], "code:core")), \
                unittest.mock.patch.object(drift_fast._source_selftests(), "diff_names",
                                           return_value=["core/x.cpp"]):
            rc, out = quietly(drift_fast.run_source,
                              type("A", (), {"base": "HEAD^1", "jobs": 1})())
        self.assertEqual(rc, 2)
        self.assertIn("refused", out)


if __name__ == "__main__":
    unittest.main()

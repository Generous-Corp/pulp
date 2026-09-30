#!/usr/bin/env python3
"""Tests for tools/ci/drift_fast.py and its workflow/manifest contract."""

from __future__ import annotations

import contextlib
import io
import json
import pathlib
import sys
import tempfile
import unittest

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
        text = drift_fast.WORKFLOW.read_text(encoding="utf-8").replace(
            "    runs-on: ubuntu-latest\n",
            "    if: github.event_name == 'merge_group'\n    runs-on: ubuntu-latest\n",
        )
        rc, out = self.check(text)
        self.assertEqual(rc, 1)
        self.assertIn("job-level if:", out)

    def test_workflow_not_calling_run_fails(self):
        text = drift_fast.WORKFLOW.read_text(encoding="utf-8").replace(
            "tools/ci/drift_fast.py run", "true"
        )
        rc, out = self.check(text)
        self.assertEqual(rc, 1)
        self.assertIn("never calls", out)

    def test_every_listed_test_is_a_real_registration_name(self):
        # Guards typos: each listed name must appear in some test manifest.
        sources = "\n".join(
            p.read_text(encoding="utf-8")
            for p in (drift_fast.REPO_ROOT / "test").rglob("*.cmake")
        )
        sources += (drift_fast.REPO_ROOT / "test" / "CMakeLists.txt").read_text(encoding="utf-8")
        for entry in json.loads(drift_fast.MANIFEST.read_text(encoding="utf-8"))["tests"]:
            self.assertIn(f"NAME {entry['name']}", sources, entry["name"])


if __name__ == "__main__":
    unittest.main()

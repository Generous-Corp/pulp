#!/usr/bin/env python3
"""Focused positive, negative, and mutation tests for protected receipts."""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import subprocess
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest import mock

import protected_merge_receipt as receipt


def git(repo: Path, *args: str, input_text: str | None = None) -> str:
    return subprocess.run(
        ["git", "-C", str(repo), *args],
        check=True,
        capture_output=True,
        text=True,
        input=input_text,
    ).stdout.strip()


class ProtectedMergeReceiptTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.repo = Path(self.temp.name) / "repo"
        self.repo.mkdir()
        git(self.repo, "init", "-q")
        git(self.repo, "config", "user.email", "ci@example.invalid")
        git(self.repo, "config", "user.name", "CI")
        for path in receipt.POLICY_PATHS:
            target = self.repo / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(f"policy:{path}\n", encoding="utf-8")
        (self.repo / "source.txt").write_text("base\n", encoding="utf-8")
        git(self.repo, "add", ".")
        git(self.repo, "commit", "-qm", "base")
        self.base = git(self.repo, "rev-parse", "HEAD")
        git(self.repo, "checkout", "-qb", "feature")
        (self.repo / "source.txt").write_text("head\n", encoding="utf-8")
        git(self.repo, "commit", "-qam", "head")
        self.head = git(self.repo, "rev-parse", "HEAD")
        tree = git(self.repo, "rev-parse", f"{self.head}^{{tree}}")
        self.validated = git(
            self.repo,
            "commit-tree", tree, "-p", self.base, "-p", self.head,
            input_text="validated\n",
        )
        self.group = git(
            self.repo,
            "commit-tree", tree, "-p", self.base, "-p", self.head,
            input_text="group\n",
        )
        self.build = self.repo / "build"
        self.build.mkdir()
        executable = self.build / "pulp-test"
        executable.write_bytes(b"exact tested artifact\n")
        executable.chmod(0o755)
        compiler = self.build / "fake-cxx"
        compiler.write_text("#!/bin/sh\necho fake-cxx 1.0\n", encoding="utf-8")
        compiler.chmod(0o755)
        (self.build / "CMakeCache.txt").write_text(
            "CMAKE_BUILD_TYPE:STRING=Release\n"
            f"CMAKE_CXX_COMPILER:FILEPATH={compiler}\n"
            "CMAKE_GENERATOR:INTERNAL=Ninja\n",
            encoding="utf-8",
        )
        self.ctest_json = self.build / "ctest.json"
        self.ctest_json.write_text(
            json.dumps({"tests": [{"name": "unit", "command": [str(executable)]}]}),
            encoding="utf-8",
        )
        self.evidence = self.repo / "evidence"
        self.evidence.mkdir()
        self.exit_file = self.evidence / "exit-code"
        self.exit_file.write_text("0\n", encoding="utf-8")
        self.selection = self.evidence / "selection.json"
        self.write_selection()
        self.selected = self.evidence / "selected.json"
        self.selected.write_text(self.ctest_json.read_text(), encoding="utf-8")
        self.junit = self.evidence / "ctest.junit.xml"
        self.write_junit({"unit": "run"})

    def write_selection(self, **overrides: str) -> None:
        selection = {
            "label_exclude": receipt.REQUIRED_LABEL_EXCLUDE,
            "exclude_regex": "STFT",
            "label_include": "",
            "include_regex": "",
        }
        selection.update(overrides)
        self.selection.write_text(json.dumps(selection), encoding="utf-8")

    def write_junit(self, cases: dict[str, str], failure: str | None = None) -> None:
        body = []
        for name, status in cases.items():
            child = ""
            if name == failure:
                child = '<failure message="Failed"/>'
            elif status == "notrun":
                child = '<skipped message="SKIP_RETURN_CODE=4"/>'
            body.append(f'<testcase name="{name}" classname="{name}" status="{status}">{child}</testcase>')
        self.junit.write_text(
            '<?xml version="1.0" encoding="UTF-8"?>\n'
            f'<testsuite name="(empty)" tests="{len(cases)}">{"".join(body)}</testsuite>\n',
            encoding="utf-8",
        )

    def resign(self, issued: dict) -> dict:
        unsigned = dict(issued)
        del unsigned["receipt_digest"]
        issued["receipt_digest"] = receipt.digest(unsigned)
        return issued

    def tearDown(self) -> None:
        self.temp.cleanup()

    def issue_args(self) -> argparse.Namespace:
        return argparse.Namespace(
            repo=self.repo,
            repository="Generous-Corp/pulp",
            workflow="Build and Test",
            target="macos",
            base_sha=self.base,
            head_sha=self.head,
            checkout_sha=self.validated,
            workflow_sha=self.base,
            run_id="42",
            run_attempt="1",
            build_dir=self.build,
            ctest_json=self.ctest_json,
            ctest_exit_file=self.exit_file,
            ctest_selection=self.selection,
            ctest_selected_json=self.selected,
            ctest_junit=self.junit,
        )

    def verify_args(self, group: str | None = None) -> argparse.Namespace:
        return argparse.Namespace(
            repo=self.repo,
            repository="Generous-Corp/pulp",
            workflow="Build and Test",
            target="macos",
            group_sha=group or self.group,
        )

    def test_identical_group_derives_new_subject_bound_decision(self) -> None:
        issued = receipt.issue(self.issue_args())
        decision = receipt.verify_receipt(issued, self.verify_args())
        self.assertEqual(decision["verdict"], "reuse")
        self.assertEqual(decision["merge_group_sha"], self.group)
        self.assertEqual(decision["source_receipt_digest"], issued["receipt_digest"])
        self.assertNotEqual(decision["decision_digest"], issued["receipt_digest"])

    def test_exact_head_checkout_derives_same_subject_bound_decision(self) -> None:
        args = self.issue_args()
        args.checkout_sha = self.head
        issued = receipt.issue(args)
        decision = receipt.verify_receipt(issued, self.verify_args())
        self.assertEqual(issued["validated_checkout"]["sha"], self.head)
        self.assertEqual(decision["verdict"], "reuse")
        self.assertEqual(decision["merge_group_sha"], self.group)

    def test_shallow_merge_checkout_preserves_object_parent_identity(self) -> None:
        # actions/checkout fetch-depth=1 records the checked-out synthetic merge
        # in .git/shallow. Revision walkers hide its parents, but exact receipt
        # identity must use the parents stored in the commit object itself.
        (self.repo / ".git/shallow").write_text(
            f"{self.validated}\n", encoding="ascii"
        )
        issued = receipt.issue(self.issue_args())
        self.assertEqual(
            issued["validated_checkout"]["parents"], [self.base, self.head]
        )
        self.assertEqual(
            receipt.verify_receipt(issued, self.verify_args())["verdict"], "reuse"
        )

    def test_changed_base_or_head_fails_closed(self) -> None:
        issued = receipt.issue(self.issue_args())
        changed = git(
            self.repo, "commit-tree", f"{self.head}^{{tree}}", "-p", self.head,
            input_text="changed base\n",
        )
        with self.assertRaisesRegex(receipt.ReceiptError, "two-parent"):
            receipt.verify_receipt(issued, self.verify_args(changed))

    def test_changed_tree_fails_closed(self) -> None:
        issued = receipt.issue(self.issue_args())
        (self.repo / "different.txt").write_text("different\n", encoding="utf-8")
        git(self.repo, "add", "different.txt")
        different_tree = git(self.repo, "write-tree")
        changed = git(
            self.repo,
            "commit-tree", different_tree, "-p", self.base, "-p", self.head,
            input_text="changed tree\n",
        )
        with self.assertRaisesRegex(receipt.ReceiptError, "tree is not identical"):
            receipt.verify_receipt(issued, self.verify_args(changed))

    def test_policy_mutation_fails_closed(self) -> None:
        issued = receipt.issue(self.issue_args())
        policy_path = self.repo / receipt.POLICY_PATHS[0]
        policy_path.write_text("mutated\n", encoding="utf-8")
        git(self.repo, "add", str(policy_path))
        mutated_tree = git(self.repo, "write-tree")
        changed = git(
            self.repo,
            "commit-tree", mutated_tree, "-p", self.base, "-p", self.head,
            input_text="policy mutation\n",
        )
        with self.assertRaises(receipt.ReceiptError):
            receipt.verify_receipt(issued, self.verify_args(changed))

    def test_receipt_and_artifact_digest_mutations_fail_closed(self) -> None:
        issued = receipt.issue(self.issue_args())
        issued["head_sha"] = self.base
        with self.assertRaisesRegex(receipt.ReceiptError, "digest"):
            receipt.verify_receipt(issued, self.verify_args())

        issued = receipt.issue(self.issue_args())
        issued["artifact"]["files"][0]["sha256"] = "0" * 64
        unsigned = dict(issued)
        del unsigned["receipt_digest"]
        issued["receipt_digest"] = receipt.digest(unsigned)
        with self.assertRaisesRegex(receipt.ReceiptError, "artifact identity"):
            receipt.verify_receipt(issued, self.verify_args())

    def test_issue_rejects_unrelated_checkout_and_missing_artifact(self) -> None:
        args = self.issue_args()
        unrelated = git(
            self.repo,
            "commit-tree", f"{self.head}^{{tree}}", "-p", self.head,
            input_text="unrelated checkout\n",
        )
        args.checkout_sha = unrelated
        with self.assertRaisesRegex(receipt.ReceiptError, "neither the exact head"):
            receipt.issue(args)
        args = self.issue_args()
        Path(json.loads(self.ctest_json.read_text())["tests"][0]["command"][0]).unlink()
        with self.assertRaisesRegex(receipt.ReceiptError, "artifact unavailable"):
            receipt.issue(args)

    def test_receipt_records_the_executed_test_evidence(self) -> None:
        issued = receipt.issue(self.issue_args())
        validation = issued["validation"]
        self.assertEqual(validation["selected"], 1)
        self.assertEqual(validation["passed"], 1)
        self.assertEqual(validation["failed"], 0)
        self.assertEqual(validation["inventory_count"], 1)
        self.assertEqual(validation["selection"]["label_exclude"], receipt.REQUIRED_LABEL_EXCLUDE)

    def test_issue_refuses_without_test_evidence(self) -> None:
        # A pull-request run that skipped its Test step has no exit status and
        # no JUnit report; it must not be able to produce a receipt.
        self.exit_file.unlink()
        with self.assertRaisesRegex(receipt.ReceiptError, "exit status is unavailable"):
            receipt.issue(self.issue_args())
        self.exit_file.write_text("0\n", encoding="utf-8")
        self.junit.unlink()
        with self.assertRaisesRegex(receipt.ReceiptError, "JUnit report is unavailable"):
            receipt.issue(self.issue_args())

    def test_issue_refuses_failing_or_incomplete_runs(self) -> None:
        self.exit_file.write_text("8\n", encoding="utf-8")
        with self.assertRaisesRegex(receipt.ReceiptError, "did not exit cleanly"):
            receipt.issue(self.issue_args())
        self.exit_file.write_text("0\n", encoding="utf-8")
        self.write_junit({"unit": "fail"}, failure="unit")
        with self.assertRaisesRegex(receipt.ReceiptError, "failed tests"):
            receipt.issue(self.issue_args())
        self.write_junit({})
        with self.assertRaisesRegex(receipt.ReceiptError, "exactly the selected tests"):
            receipt.issue(self.issue_args())
        self.selected.write_text(json.dumps({"tests": []}), encoding="utf-8")
        with self.assertRaisesRegex(receipt.ReceiptError, "no tests"):
            receipt.issue(self.issue_args())

    def test_issue_refuses_a_narrowed_test_tier(self) -> None:
        self.write_selection(label_include="pr-fast")
        with self.assertRaisesRegex(receipt.ReceiptError, "narrowed test tier"):
            receipt.issue(self.issue_args())
        self.write_selection(label_exclude="validation")
        with self.assertRaisesRegex(receipt.ReceiptError, "different label set"):
            receipt.issue(self.issue_args())

    def test_skipped_tests_count_as_run_evidence_but_not_passes(self) -> None:
        self.write_junit({"unit": "notrun"})
        with self.assertRaisesRegex(receipt.ReceiptError, "no executed tests"):
            receipt.issue(self.issue_args())

    def test_verify_rejects_forged_or_legacy_validation_records(self) -> None:
        # The former verifier compared against the same constant the issuer
        # wrote, so a record asserting success without evidence verified. A
        # correctly digested receipt must still be refused on its contents.
        forged = receipt.issue(self.issue_args())
        forged["validation"] = {"conclusion": "success", "ctest_exit": 0}
        with self.assertRaisesRegex(receipt.ReceiptError, "validation fields"):
            receipt.verify_receipt(self.resign(forged), self.verify_args())

        for field, value, message in (
            ("failed", 1, "failed tests"),
            ("passed", 0, "no executed tests"),
            ("selected", 2, "every selected test"),
            ("ctest_exit", False, "successful validation"),
        ):
            issued = receipt.issue(self.issue_args())
            issued["validation"][field] = value
            with self.subTest(field=field), self.assertRaisesRegex(receipt.ReceiptError, message):
                receipt.verify_receipt(self.resign(issued), self.verify_args())

        issued = receipt.issue(self.issue_args())
        issued["validation"]["selection"]["include_regex"] = "Knob"
        with self.assertRaisesRegex(receipt.ReceiptError, "narrowed test tier"):
            receipt.verify_receipt(self.resign(issued), self.verify_args())

        issued = receipt.issue(self.issue_args())
        issued["validation"]["inventory_count"] = 100
        with self.assertRaisesRegex(receipt.ReceiptError, "too little"):
            receipt.verify_receipt(self.resign(issued), self.verify_args())

        issued = receipt.issue(self.issue_args())
        issued["schema"] = "pulp.protected-validation-receipt/v1"
        with self.assertRaisesRegex(receipt.ReceiptError, "schema"):
            receipt.verify_receipt(self.resign(issued), self.verify_args())

    def download_args(self) -> argparse.Namespace:
        return argparse.Namespace(
            api_url="https://api.github.test",
            token="secret",
            artifact_name=f"protected-validation-macos-{self.head}-{self.base}",
            repository="Generous-Corp/pulp",
            workflow="Build and Test",
            target="macos",
            base_sha=self.base,
            head_sha=self.head,
            output=Path(self.temp.name) / "downloaded.json",
        )

    def test_download_requires_one_authenticated_exact_run_artifact(self) -> None:
        issued = receipt.issue(self.issue_args())
        archive_buffer = io.BytesIO()
        with zipfile.ZipFile(archive_buffer, "w") as bundle:
            bundle.writestr("receipt.json", receipt.canonical_json(issued) + b"\n")
        archive = archive_buffer.getvalue()
        artifact = {
            "id": 7,
            "name": self.download_args().artifact_name,
            "expired": False,
            "digest": f"sha256:{hashlib.sha256(archive).hexdigest()}",
            "archive_download_url": "https://objects.test/receipt.zip",
            "workflow_run": {"id": 42},
        }
        run = {
            "name": "Build and Test",
            "event": "pull_request",
            "conclusion": "success",
            "head_sha": self.head,
            "pull_requests": [{
                "head": {"sha": self.head}, "base": {"sha": self.base}
            }],
        }
        response = mock.MagicMock()
        response.__enter__.return_value.read.return_value = archive
        opener = mock.MagicMock()
        opener.open.return_value = response
        with mock.patch.object(receipt.urllib.request, "build_opener", return_value=opener), mock.patch.object(
            receipt, "_api_json", side_effect=[{"artifacts": [artifact]}, run]
        ):
            authority = receipt.download(self.download_args())
        self.assertEqual(authority, {"run_id": "42", "artifact_id": "7"})
        self.assertEqual(json.loads(self.download_args().output.read_text()), issued)

    def test_download_rejects_ambiguous_or_expired_evidence(self) -> None:
        artifact = {
            "name": self.download_args().artifact_name,
            "expired": False,
        }
        with mock.patch.object(receipt.urllib.request, "build_opener"), mock.patch.object(
            receipt, "_api_json", return_value={"artifacts": [artifact, artifact]}
        ):
            with self.assertRaisesRegex(receipt.ReceiptError, "exactly one"):
                receipt.download(self.download_args())


if __name__ == "__main__":
    unittest.main()

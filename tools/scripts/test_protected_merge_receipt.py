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

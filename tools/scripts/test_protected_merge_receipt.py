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


    def test_decision_note_carries_the_verified_receipts_evidence(self) -> None:
        issued = receipt.issue(self.issue_args())
        receipt.verify_receipt(issued, self.verify_args())
        note = receipt.decision_note("macos", "reuse", "exact-tree receipt revalidated", issued)
        self.assertEqual(note, {
            "schema": "shipyard-receipt-decision/v1", "target": "macos", "verdict": "reuse",
            "reason": "exact-tree receipt revalidated", "source_run_id": "42",
            "selected": 1, "passed": 1, "skipped": 0, "inventory_count": 1,
        })


class DecisionNoteCliTest(unittest.TestCase):
    """The notes Shipyard reads describe a decision; they never make one."""

    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.dir = Path(self.temp.name)

    def tearDown(self) -> None:
        self.temp.cleanup()

    def note(self, *args: str) -> dict:
        out = self.dir / f"note-{len(list(self.dir.iterdir()))}.json"
        self.assertEqual(receipt.main(["note", *args, "--output", str(out)]), 0)
        return json.loads(out.read_text())

    def test_refusal_reason_is_the_verifiers_last_stderr_line(self) -> None:
        stderr = self.dir / "stderr.txt"
        stderr.write_text("Traceback noise\nprotected receipt: receipt records a narrowed "
                          "test tier, not full validation\n")
        forged = self.dir / "receipt.json"
        forged.write_text(json.dumps({"run_id": "77", "validation": {
            "selected": 9, "passed": 9, "skipped": 0, "inventory_count": 3000}}))
        note = self.note("--target", "linux", "--verdict", "refuse",
                         "--reason-file", str(stderr), "--receipt", str(forged))
        self.assertEqual(note["verdict"], "refuse")
        self.assertEqual(note["reason"], "receipt records a narrowed test tier, not full validation")
        self.assertEqual((note["selected"], note["inventory_count"], note["source_run_id"]),
                         (9, 3000, "77"))

    def test_absent_or_malformed_evidence_reads_as_unknown_not_zero(self) -> None:
        garbage = self.dir / "garbage.json"
        garbage.write_text("{not json")
        note = self.note("--target", "macos", "--verdict", "refuse",
                         "--reason", "no protected receipt for this head and base",
                         "--receipt", str(garbage))
        self.assertEqual([note[k] for k in receipt.NOTE_COUNT_KEYS], [None] * 4)
        self.assertIsNone(note["source_run_id"])
        blank = self.note("--target", "macos", "--verdict", "refuse",
                          "--reason-file", str(self.dir / "missing.txt"))
        self.assertEqual(blank["reason"], "no reason recorded")

    def test_publish_emits_one_parseable_annotation_per_note_and_a_table(self) -> None:
        reuse = receipt.decision_note("macos", "reuse", "exact-tree receipt revalidated",
                                      {"run_id": "42", "validation": {
                                          "selected": 3500, "passed": 3490, "skipped": 10,
                                          "inventory_count": 3540}})
        refuse = receipt.decision_note("linux", "refuse", "100% | odd\nreason")
        paths = []
        for n in (reuse, refuse):
            paths.append(self.dir / f"{n['target']}.json")
            paths[-1].write_text(json.dumps(n))
        summary = self.dir / "summary.md"
        buf = io.StringIO()
        with mock.patch("sys.stdout", buf):
            self.assertEqual(receipt.main(["publish-notes", *map(str, paths),
                                           "--summary", str(summary)]), 0)
        lines = buf.getvalue().splitlines()
        self.assertEqual(len(lines), 2)
        prefix = "::notice title=shipyard-receipt-decision::"
        self.assertTrue(all(line.startswith(prefix) for line in lines), lines)
        # GitHub unescapes %25/%0A/%0D before storing the annotation message.
        decoded = [json.loads(line[len(prefix):].replace("%0A", "\n").replace("%0D", "\r")
                              .replace("%25", "%")) for line in lines]
        self.assertEqual(decoded, [reuse, refuse])
        # A bare `%` would start an escape sequence and corrupt the message.
        self.assertIn('"reason":"100%25 | odd reason"', lines[1])
        table = summary.read_text()
        self.assertIn("| macos | reused receipt from run 42 | 3500 selected / 3490 passed / "
                      "10 skipped of 3540 built | exact-tree receipt revalidated |", table)
        self.assertIn("| linux | validated in full (receipt refused) | n/a | 100% \\| odd reason |",
                      table)

    def test_no_decision_reason_is_a_plain_notice_never_a_verdict(self) -> None:
        summary = self.dir / "summary.md"
        buf = io.StringIO()
        with mock.patch("sys.stdout", buf):
            self.assertEqual(receipt.main([
                "publish-notes", "--summary", str(summary),
                "--no-decision-reason", "merge group changed no native build input"]), 0)
        # Nothing was decided, so no shipyard-receipt-decision annotation may
        # appear: Shipyard would render it as a verdict the merge never made.
        self.assertEqual(buf.getvalue().splitlines(), [
            "::notice::receipt reuse not evaluated: merge group changed no native build input"])
        self.assertIn("| — | no receipt decision was needed | | "
                      "merge group changed no native build input |", summary.read_text())

    def test_no_decision_reason_is_ignored_when_targets_were_decided(self) -> None:
        refuse = receipt.decision_note("macos", "refuse",
                                       "the merge-group commit has 3 parent(s); reuse needs "
                                       "exactly two")
        lines, table = receipt.render_notes([refuse], "merge group changed no native build input")
        self.assertEqual(len(lines), 1)
        self.assertTrue(lines[0].startswith("::notice title=shipyard-receipt-decision::"))
        self.assertNotIn("not evaluated", "\n".join(lines) + table)
        self.assertIn("3 parent(s); reuse needs exactly two", table)


class ReuseStepRefusalReasonTest(unittest.TestCase):
    """The build.yml reuse step names why a merge group's shape refused reuse.

    Runs the step's own bash against commits whose parent shape is built on
    purpose, so the recorded reason is checked against a known count.
    """

    WORKFLOW = Path(__file__).resolve().parents[2] / ".github" / "workflows" / "build.yml"

    @classmethod
    def step_script(cls) -> str:
        lines = cls.WORKFLOW.read_text(encoding="utf-8").splitlines()
        start = lines.index("      - id: reuse")
        run = next(i for i in range(start, len(lines)) if lines[i] == "        run: |")
        body = []
        for line in lines[run + 1:]:
            if line.strip() and not line.startswith(" " * 10):
                break
            body.append(line[10:])
        return "\n".join(body) + "\n"

    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.repo = self.root / "repo"
        (self.repo / "tools" / "scripts").mkdir(parents=True)
        (self.repo / "tools" / "scripts" / "protected_merge_receipt.py").write_bytes(
            Path(receipt.__file__).read_bytes())
        git(self.repo, "init", "-q")
        git(self.repo, "config", "user.email", "ci@example.invalid")
        git(self.repo, "config", "user.name", "CI")
        self.commits = []
        for name in ("a", "b", "c"):
            (self.repo / name).write_text(name, encoding="utf-8")
            git(self.repo, "add", name)
            git(self.repo, "commit", "-qm", name)
            self.commits.append(git(self.repo, "rev-parse", "HEAD"))
        self.tree = git(self.repo, "rev-parse", "HEAD^{tree}")
        (self.root / "step.sh").write_text(self.step_script(), encoding="utf-8")

    def tearDown(self) -> None:
        self.temp.cleanup()

    def octopus(self, parents: list[str]) -> str:
        # Distinct parents only: commit-tree collapses a repeated -p.
        self.assertEqual(len(set(parents)), len(parents))
        args = [arg for sha in parents for arg in ("-p", sha)]
        sha = git(self.repo, "commit-tree", self.tree, *args, input_text="group\n")
        self.assertEqual(len(git(self.repo, "rev-list", "--parents", "-n", "1", sha).split()),
                         len(parents) + 1)
        return sha

    def run_step(self, sha: str, native: str = "true") -> tuple[str, str]:
        runner_temp = self.root / f"rt-{sha[:12]}-{native}"
        runner_temp.mkdir()
        summary, output = runner_temp / "summary.md", runner_temp / "output"
        env = dict(os.environ, GITHUB_EVENT_NAME="merge_group", NATIVE_BUILD_REQUIRED=native,
                   GITHUB_SHA=sha, RUNNER_TEMP=str(runner_temp),
                   GITHUB_STEP_SUMMARY=str(summary), GITHUB_OUTPUT=str(output),
                   ORIGINAL_MATRIX='{"include":[{"key":"macos"},{"key":"linux"}]}',
                   RECEIPT_TOKEN="unused", A2T_RECEIPT_VERIFICATION_REQUIRED="false",
                   GITHUB_REPOSITORY="example/repo", GITHUB_WORKSPACE=str(self.repo))
        done = subprocess.run(["bash", "-e", str(self.root / "step.sh")], cwd=self.repo,
                              env=env, capture_output=True, text=True, timeout=60)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertIn("build_required=true", output.read_text())
        return done.stdout, summary.read_text() if summary.exists() else ""

    def decided_reasons(self, stdout: str) -> dict[str, str]:
        prefix = "::notice title=shipyard-receipt-decision::"
        notes = [json.loads(line[len(prefix):]) for line in stdout.splitlines()
                 if line.startswith(prefix)]
        return {note["target"]: note["reason"] for note in notes}

    def test_single_and_octopus_parents_are_named_by_count(self) -> None:
        # The second commit has exactly one parent; the octopus has three.
        for sha, count in ((self.commits[1], 1), (self.octopus(self.commits), 3)):
            stdout, _ = self.run_step(sha)
            reason = f"the merge-group commit has {count} parent(s); reuse needs exactly two"
            self.assertEqual(self.decided_reasons(stdout), {"macos": reason, "linux": reason})

    def test_unreadable_commit_is_not_reported_as_a_parent_count(self) -> None:
        stdout, _ = self.run_step("0" * 40)
        reason = "the merge-group commit's parents could not be read"
        self.assertEqual(self.decided_reasons(stdout), {"macos": reason, "linux": reason})

    def test_merge_group_without_native_input_says_nothing_was_evaluated(self) -> None:
        stdout, summary = self.run_step(self.octopus(self.commits[:2]), native="false")
        self.assertEqual(self.decided_reasons(stdout), {})
        self.assertIn("::notice::receipt reuse not evaluated: merge group changed no native "
                      "build input", stdout)
        self.assertIn("merge group changed no native build input |", summary)


if __name__ == "__main__":
    unittest.main()

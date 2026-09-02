#!/usr/bin/env python3
"""Focused tests for the protected A2T structural-verifier producer."""

from __future__ import annotations

import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


SCRIPT = Path(__file__).with_name("a2t_structural_verification_ci.py")
SPEC = importlib.util.spec_from_file_location("a2t_structural_verification_ci", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)
ROOT = Path(__file__).parents[2]
SCHEMA_SPEC = importlib.util.spec_from_file_location(
    "a2t_test_json_schema_lite", ROOT / MODULE.JSON_SCHEMA_PATH
)
assert SCHEMA_SPEC and SCHEMA_SPEC.loader
SCHEMA_VALIDATOR = importlib.util.module_from_spec(SCHEMA_SPEC)
SCHEMA_SPEC.loader.exec_module(SCHEMA_VALIDATOR)
GOLDEN = (
    ROOT / "docs/validation/gpu-trace-overhead/fixtures/"
    "a2t-structural-verifier-attestation-v1.golden.json"
)


def run(repository: Path, *args: str) -> str:
    completed = subprocess.run(args, cwd=repository, check=True, capture_output=True, text=True)
    return completed.stdout.strip()


class ProducerTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        run(self.root, "git", "init", "-q")
        run(self.root, "git", "config", "user.name", "A2T Test")
        run(self.root, "git", "config", "user.email", "a2t@example.invalid")
        run(self.root, "git", "config", "commit.gpgsign", "false")
        for relative in (
            MODULE.VERIFIER_PATH, MODULE.ISSUER_PATH, MODULE.WORKFLOW_PATH,
            MODULE.SCHEMA_PATH, *set(MODULE.EXECUTED_DEPENDENCIES.values()),
            Path("test/fixtures/perfetto-gpu/fixture.pftrace"),
        ):
            (self.root / relative).parent.mkdir(parents=True, exist_ok=True)
        (self.root / MODULE.VERIFIER_PATH).write_text(
            "#!/usr/bin/env python3\n"
            "import json, subprocess, sys\n"
            "from pathlib import Path\n"
            "receipt_path = Path(sys.argv[1])\n"
            "receipt = json.loads(receipt_path.read_text())\n"
            "head = subprocess.run(['git', 'rev-parse', 'HEAD'], check=True, capture_output=True, text=True).stdout.strip()\n"
            "if head != receipt['source_revision'] or receipt_path.resolve().is_relative_to(Path.cwd().resolve()):\n"
            "    print('wrong verifier checkout or receipt mount', file=sys.stderr)\n"
            "    raise SystemExit(1)\n"
            "print('gpu-trace-overhead-acceptance: ok (v3 structural integrity only; nonterminal)')\n",
            encoding="utf-8",
        )
        (self.root / MODULE.JSON_SCHEMA_PATH).write_bytes(
            (ROOT / MODULE.JSON_SCHEMA_PATH).read_bytes()
        )
        for relative in (
            MODULE.GPU_CONTRACT_PATH, MODULE.SDK_HANDOFF_PATH,
            MODULE.SDK_PROVENANCE_PATH,
        ):
            (self.root / relative).write_text(
                f"# bounded dependency fixture: {relative.name}\n", encoding="utf-8"
            )
        (self.root / MODULE.ISSUER_PATH).write_bytes(SCRIPT.read_bytes())
        (self.root / MODULE.WORKFLOW_PATH).write_text("name: Build and Test\n", encoding="utf-8")
        (self.root / MODULE.SCHEMA_PATH).write_bytes(
            (ROOT / MODULE.SCHEMA_PATH).read_bytes()
        )
        self.trace = self.root / "test/fixtures/perfetto-gpu/fixture.pftrace"
        self.trace.write_bytes(b"bounded trace fixture")
        run(self.root, "git", "add", ".")
        run(self.root, "git", "commit", "-qm", "source barrier")
        self.source = run(self.root, "git", "rev-parse", "HEAD")
        receipt = {
            "schema": "pulp.gpu-trace-overhead-acceptance.v3",
            "source_revision": self.source,
            "mcp_source_revision": self.source,
            "integration_head": self.source,
            "artifacts": {"trace": {
                "role": "repository/test/fixtures/perfetto-gpu/fixture.pftrace",
                "sha256": MODULE._sha256(self.trace.read_bytes()),
                "bytes": self.trace.stat().st_size,
            }},
        }
        (self.root / MODULE.RECEIPT_PATH).parent.mkdir(parents=True)
        (self.root / MODULE.RECEIPT_PATH).write_text(json.dumps(receipt), encoding="utf-8")
        run(self.root, "git", "add", ".")
        run(self.root, "git", "commit", "-qm", "evidence")
        self.evidence = run(self.root, "git", "rev-parse", "HEAD")
        self.pr_merge = self.make_merge_head(self.source, self.evidence)
        self.event = self.root / "event.json"
        self.event.write_text(json.dumps({"pull_request": {
            "base": {"sha": self.source}, "head": {"sha": self.evidence},
        }}), encoding="utf-8")
        self.environment = {
            "GITHUB_ACTIONS": "true",
            "GITHUB_EVENT_NAME": "pull_request",
            "GITHUB_SHA": self.pr_merge,
            "GITHUB_REPOSITORY": MODULE.REPOSITORY_NAME,
            "GITHUB_EVENT_PATH": str(self.event),
            "GITHUB_WORKFLOW_SHA": self.evidence,
            "GITHUB_RUN_ID": "1234",
            "GITHUB_RUN_ATTEMPT": "2",
            "GITHUB_JOB": MODULE.JOB_KEY,
            "RUNNER_OS": "macOS",
            "A2T_CHECK_NAME": MODULE.CHECK_NAME,
            "A2T_STEP_NAME": MODULE.STEP_NAME,
        }

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def issue(self) -> dict:
        path = MODULE.issue(self.root, self.root / "out", self.environment)
        assert path is not None
        return json.loads(path.read_text())

    def commit_tree(self, tree_revision: str, *parents: str) -> str:
        command = ["git", "commit-tree", f"{tree_revision}^{{tree}}"]
        for parent in parents:
            command.extend(("-p", parent))
        completed = subprocess.run(
            command, cwd=self.root, input="synthetic event commit\n",
            check=True, capture_output=True, text=True,
        )
        return completed.stdout.strip()

    def make_merge_head(self, base: str, head: str, *, tree: str | None = None) -> str:
        return self.commit_tree(tree or head, base, head)

    def set_pull_request_event(self, base: str, head: str) -> None:
        self.environment["GITHUB_EVENT_NAME"] = "pull_request"
        self.environment["GITHUB_SHA"] = self.make_merge_head(base, head)
        self.event.write_text(json.dumps({"pull_request": {
            "base": {"sha": base}, "head": {"sha": head},
        }}), encoding="utf-8")

    def test_issues_execution_facts_without_future_or_self_authentication(self) -> None:
        payload = self.issue()
        self.assertEqual(payload["source_revision"], self.source)
        self.assertEqual(payload["evidence_head"], self.evidence)
        self.assertEqual(payload["contract"]["commit"], self.source)
        self.assertEqual(payload["issuer"]["commit"], self.source)
        self.assertEqual(set(payload["dependencies"]), set(MODULE.EXECUTED_DEPENDENCIES))
        for name, path in MODULE.EXECUTED_DEPENDENCIES.items():
            self.assertEqual(payload["dependencies"][name]["commit"], self.source)
            self.assertEqual(payload["dependencies"][name]["path"], path.as_posix())
        self.assertEqual(payload["run"]["attempt"], 2)
        self.assertEqual(payload["step"]["verifier_command"], MODULE.VERIFIER_COMMAND)
        encoded = json.dumps(payload)
        for forbidden in ("artifact_id", "artifact_digest", "artifact_size", "conclusion", "integration"):
            self.assertNotIn(forbidden, encoded)

    def test_golden_fixture_is_closed_v1_and_forbids_future_authority(self) -> None:
        schema = json.loads((ROOT / MODULE.SCHEMA_PATH).read_text())
        golden_bytes = GOLDEN.read_bytes()
        self.assertEqual(golden_bytes, MODULE.canonical_golden_bytes())
        golden = json.loads(golden_bytes)
        self.assertEqual(SCHEMA_VALIDATOR.validate(golden, schema), [])
        self.assertEqual(
            golden["schema"],
            "pulp.gpu-trace-structural-verifier-attestation.v1",
        )
        for forbidden in (
            "integration", "protected_merge", "artifact_id", "artifact_digest",
            "artifact_size_in_bytes", "conclusion",
        ):
            self.assertNotIn(forbidden, json.dumps(golden))
            mutated = dict(golden)
            mutated[forbidden] = "future-or-self-claimed"
            self.assertTrue(SCHEMA_VALIDATOR.validate(mutated, schema))

        for dependency in MODULE.EXECUTED_DEPENDENCIES:
            mutated = json.loads(json.dumps(golden))
            del mutated["dependencies"][dependency]
            self.assertTrue(SCHEMA_VALIDATOR.validate(mutated, schema))

    def test_canonical_producer_regenerates_golden_byte_exactly(self) -> None:
        output = self.root / "generated-golden.json"
        completed = subprocess.run(
            [sys.executable, SCRIPT, "--write-golden", output],
            cwd=ROOT, check=False, capture_output=True, text=True,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(output.read_bytes(), GOLDEN.read_bytes())

    def test_receipt_addition_runs_and_issues_only_on_pull_request(self) -> None:
        self.assertEqual(
            MODULE.receipt_change_decision(self.root, self.environment),
            (True, True, self.evidence),
        )
        for event_name, payload in (
            ("merge_group", {"merge_group": {"base_sha": self.source, "head_sha": self.evidence}}),
            ("push", {"ref": "refs/heads/main", "before": self.source, "after": self.evidence}),
        ):
            with self.subTest(event=event_name):
                self.environment["GITHUB_EVENT_NAME"] = event_name
                self.event.write_text(json.dumps(payload), encoding="utf-8")
                self.assertEqual(
                    MODULE.receipt_change_decision(self.root, self.environment),
                    (True, False, self.evidence),
                )

    def test_protected_push_runs_verify_only_without_attestation(self) -> None:
        self.environment["GITHUB_EVENT_NAME"] = "push"
        self.environment["GITHUB_JOB"] = MODULE.VERIFY_ONLY_JOB_KEY
        self.event.write_text(json.dumps({
            "ref": "refs/heads/main", "before": self.source,
            "after": self.evidence,
        }), encoding="utf-8")
        output = MODULE.issue(
            self.root, self.root / "out", self.environment,
            create_attestation=False,
        )
        self.assertIsNone(output)
        self.assertFalse((self.root / "out").exists())

    def test_inherited_unchanged_receipt_skips_unrelated_and_tool_only_heads(self) -> None:
        for relative in (Path("README.md"), MODULE.ISSUER_PATH):
            with self.subTest(path=relative.as_posix()):
                target = self.root / relative
                original = target.read_bytes() if target.exists() else b""
                target.write_bytes(original + b"\n# later change\n")
                run(self.root, "git", "add", relative.as_posix())
                run(self.root, "git", "commit", "-qm", "later unrelated change")
                head = run(self.root, "git", "rev-parse", "HEAD")
                self.set_pull_request_event(self.evidence, head)
                self.assertEqual(
                    MODULE.receipt_change_decision(self.root, self.environment),
                    (False, False, head),
                )
                self.evidence = head

    def test_stale_pr_skips_receipt_added_only_on_current_base(self) -> None:
        stale_head = self.commit_tree(self.source, self.source)
        merge_head = self.make_merge_head(
            self.evidence, stale_head, tree=self.evidence,
        )
        self.environment["GITHUB_EVENT_NAME"] = "pull_request"
        self.environment["GITHUB_SHA"] = merge_head
        self.event.write_text(json.dumps({"pull_request": {
            "base": {"sha": self.evidence}, "head": {"sha": stale_head},
        }}), encoding="utf-8")
        self.assertEqual(
            MODULE.receipt_change_decision(self.root, self.environment),
            (False, False, stale_head),
        )

    def test_modified_receipt_runs_but_deletion_fails_closed(self) -> None:
        receipt = self.root / MODULE.RECEIPT_PATH
        receipt.write_bytes(receipt.read_bytes() + b"\n")
        run(self.root, "git", "add", receipt.as_posix())
        run(self.root, "git", "commit", "-qm", "modify receipt")
        modified = run(self.root, "git", "rev-parse", "HEAD")
        self.set_pull_request_event(self.evidence, modified)
        self.assertEqual(
            MODULE.receipt_change_decision(self.root, self.environment),
            (True, True, modified),
        )
        receipt.unlink()
        run(self.root, "git", "add", receipt.as_posix())
        run(self.root, "git", "commit", "-qm", "delete receipt")
        deleted = run(self.root, "git", "rev-parse", "HEAD")
        self.set_pull_request_event(modified, deleted)
        with self.assertRaisesRegex(MODULE.IssuerError, "cannot be structurally verified"):
            MODULE.receipt_change_decision(self.root, self.environment)

    def test_shallow_checkout_hydrates_exact_base_and_unavailable_base_fails(self) -> None:
        shallow = self.root / "decision-shallow"
        run(self.root, "git", "clone", "--quiet", "--depth=1", f"file://{self.root}", str(shallow))
        self.assertNotEqual(
            subprocess.run(
                ["git", "cat-file", "-e", f"{self.source}^{{commit}}"],
                cwd=shallow, capture_output=True,
            ).returncode,
            0,
        )
        self.set_pull_request_event(self.source, self.evidence)
        self.assertEqual(
            MODULE.receipt_change_decision(shallow, self.environment),
            (True, True, self.evidence),
        )
        self.event.write_text(json.dumps({"pull_request": {
            "base": {"sha": "f" * 40}, "head": {"sha": self.evidence},
        }}), encoding="utf-8")
        with self.assertRaisesRegex(MODULE.IssuerError, "cannot hydrate exact event commit"):
            MODULE.receipt_change_decision(shallow, self.environment)

    def test_event_sha_is_data_not_shell_input(self) -> None:
        marker = self.root / "injected"
        self.event.write_text(json.dumps({"pull_request": {
            "base": {"sha": f"$(touch {marker})"},
            "head": {"sha": self.evidence},
        }}), encoding="utf-8")
        with self.assertRaisesRegex(MODULE.IssuerError, "not an exact commit"):
            MODULE.receipt_change_decision(self.root, self.environment, hydrate=False)
        self.assertFalse(marker.exists())

    def test_rejects_receipt_not_identical_to_pr_head_blob(self) -> None:
        (self.root / MODULE.RECEIPT_PATH).write_text("{}", encoding="utf-8")
        with self.assertRaisesRegex(MODULE.IssuerError, "working receipt differs"):
            self.issue()

    def test_rejects_noncanonical_verifier_output(self) -> None:
        with mock.patch.object(MODULE, "_run_verifier", return_value=(0, b"not canonical\n", b"")):
            with self.assertRaisesRegex(MODULE.IssuerError, "stdout is not canonical"):
                self.issue()

    def test_rejects_verifier_failure_without_issuing_an_attestation(self) -> None:
        with mock.patch.object(MODULE, "_run_verifier", return_value=(1, b"", b"bounded failure\n")):
            with self.assertRaisesRegex(MODULE.IssuerError, "failed with exit 1"):
                self.issue()
        self.assertFalse((self.root / "out").exists())

    def test_shallow_checkout_succeeds_without_local_shared_clone(self) -> None:
        shallow = self.root / "shallow-checkout"
        run(
            self.root, "git", "clone", "--quiet", "--depth=2",
            f"file://{self.root}", str(shallow),
        )
        self.assertEqual(run(shallow, "git", "rev-parse", "--is-shallow-repository"), "true")
        original_run = subprocess.run

        def reject_old_shared_clone(args, *positional, **keywords):
            if list(args[:3]) == ["git", "clone", "--quiet"] and "--shared" in args:
                return subprocess.CompletedProcess(
                    args, 128, "", "fatal: source repository is shallow\n"
                )
            return original_run(args, *positional, **keywords)

        with mock.patch.object(MODULE.subprocess, "run", side_effect=reject_old_shared_clone):
            output = MODULE.issue(shallow, shallow / "out", self.environment)
        payload = json.loads(output.read_text())
        self.assertEqual(payload["source_revision"], self.source)
        self.assertEqual(payload["evidence_head"], self.evidence)

    def test_exact_source_worktree_is_removed_after_verifier_failure(self) -> None:
        before = run(self.root, "git", "worktree", "list", "--porcelain")
        with mock.patch.object(
            MODULE, "VERIFIER_COMMAND",
            ["python3", "-c", "raise SystemExit(7)"],
        ):
            exit_code, _stdout, _stderr = MODULE._run_verifier(
                self.root, self.source,
                (self.root / MODULE.RECEIPT_PATH).read_bytes(),
            )
        self.assertEqual(exit_code, 7)
        self.assertEqual(
            run(self.root, "git", "worktree", "list", "--porcelain"), before
        )

    def test_rejects_wrong_job_boundary(self) -> None:
        self.environment["GITHUB_JOB"] = "linux"
        with self.assertRaisesRegex(MODULE.IssuerError, "authorized native job"):
            self.issue()

    def test_rejects_issuer_changed_after_reviewed_source(self) -> None:
        (self.root / MODULE.ISSUER_PATH).write_text("# changed in E\n", encoding="utf-8")
        run(self.root, "git", "add", ".")
        run(self.root, "git", "commit", "-qm", "mutate issuer after source")
        changed_head = run(self.root, "git", "rev-parse", "HEAD")
        self.set_pull_request_event(self.source, changed_head)
        self.environment["GITHUB_WORKFLOW_SHA"] = changed_head
        with self.assertRaisesRegex(MODULE.IssuerError, "issuer changed between S and E"):
            self.issue()

    def test_rejects_every_executed_dependency_changed_after_source(self) -> None:
        unique_paths = tuple(dict.fromkeys(MODULE.EXECUTED_DEPENDENCIES.values()))
        originals = {path: (self.root / path).read_bytes() for path in unique_paths}
        for path in unique_paths:
            with self.subTest(path=path.as_posix()):
                (self.root / path).write_bytes(originals[path] + b"# E-only mutation\n")
                run(self.root, "git", "add", path.as_posix())
                run(self.root, "git", "commit", "-qm", f"mutate {path.name} after source")
                changed_head = run(self.root, "git", "rev-parse", "HEAD")
                self.set_pull_request_event(self.source, changed_head)
                self.environment["GITHUB_WORKFLOW_SHA"] = changed_head
                with self.assertRaisesRegex(
                    MODULE.IssuerError, f"executed dependency {path} changed"
                ):
                    self.issue()
                (self.root / path).write_bytes(originals[path])
                run(self.root, "git", "add", path.as_posix())
                run(self.root, "git", "commit", "-qm", f"restore {path.name}")
                restored_head = run(self.root, "git", "rev-parse", "HEAD")
                self.set_pull_request_event(self.source, restored_head)
                self.environment["GITHUB_WORKFLOW_SHA"] = restored_head

    def test_workflow_wires_exact_required_macos_producer(self) -> None:
        workflow = (ROOT / MODULE.WORKFLOW_PATH).read_text(encoding="utf-8")
        self.assertIn("name: Verify A2T structural receipt", workflow)
        self.assertIn("python3 tools/scripts/a2t_structural_verification_ci.py", workflow)
        self.assertIn("--classify-receipt-change", workflow)
        self.assertIn("needs.classify.outputs.a2t_receipt_verification_required == 'true'", workflow)
        self.assertNotIn("hashFiles('evidence/receipt.json')", workflow)
        self.assertIn("needs.classify.outputs.a2t_receipt_attestation_required == 'true'", workflow)
        self.assertIn('git fetch --no-tags --depth=1 origin "$source_revision"', workflow)
        self.assertIn("name: a2t-structural-verification-${{ needs.classify.outputs.a2t_evidence_head }}", workflow)
        self.assertIn("a2t-protected-event:", workflow)
        self.assertIn("python3 tools/scripts/a2t_structural_verification_ci.py --verify-only", workflow)
        self.assertIn("macos receipt reuse disabled: A2T receipt changed", workflow)
        self.assertIn("a2t_receipt_verification_required=true", workflow)
        self.assertIn("runner.os == 'macOS'", workflow)
        self.assertIn("matrix.key == 'macos'", workflow)
        self.assertIn("github.event_name == 'pull_request'", workflow)
        self.assertIn("if-no-files-found: error", workflow)


if __name__ == "__main__":
    unittest.main()

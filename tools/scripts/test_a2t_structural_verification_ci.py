#!/usr/bin/env python3
"""Focused tests for the protected A2T structural-verifier producer."""

from __future__ import annotations

import importlib.util
import json
import os
import subprocess
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
        self.event = self.root / "event.json"
        self.event.write_text(json.dumps({"pull_request": {"head": {"sha": self.evidence}}}), encoding="utf-8")
        self.environment = {
            "GITHUB_ACTIONS": "true",
            "GITHUB_EVENT_NAME": "pull_request",
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
        return json.loads(path.read_text())

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
        golden = json.loads(GOLDEN.read_text())
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

    def test_rejects_wrong_job_boundary(self) -> None:
        self.environment["GITHUB_JOB"] = "linux"
        with self.assertRaisesRegex(MODULE.IssuerError, "native build job"):
            self.issue()

    def test_rejects_issuer_changed_after_reviewed_source(self) -> None:
        (self.root / MODULE.ISSUER_PATH).write_text("# changed in E\n", encoding="utf-8")
        run(self.root, "git", "add", ".")
        run(self.root, "git", "commit", "-qm", "mutate issuer after source")
        changed_head = run(self.root, "git", "rev-parse", "HEAD")
        self.event.write_text(
            json.dumps({"pull_request": {"head": {"sha": changed_head}}}),
            encoding="utf-8",
        )
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
                self.event.write_text(
                    json.dumps({"pull_request": {"head": {"sha": changed_head}}}),
                    encoding="utf-8",
                )
                self.environment["GITHUB_WORKFLOW_SHA"] = changed_head
                with self.assertRaisesRegex(
                    MODULE.IssuerError, f"executed dependency {path} changed"
                ):
                    self.issue()
                (self.root / path).write_bytes(originals[path])
                run(self.root, "git", "add", path.as_posix())
                run(self.root, "git", "commit", "-qm", f"restore {path.name}")
                restored_head = run(self.root, "git", "rev-parse", "HEAD")
                self.event.write_text(
                    json.dumps({"pull_request": {"head": {"sha": restored_head}}}),
                    encoding="utf-8",
                )
                self.environment["GITHUB_WORKFLOW_SHA"] = restored_head

    def test_workflow_wires_exact_required_macos_producer(self) -> None:
        workflow = (ROOT / MODULE.WORKFLOW_PATH).read_text(encoding="utf-8")
        self.assertIn("name: Verify A2T structural receipt", workflow)
        self.assertIn("python3 tools/scripts/a2t_structural_verification_ci.py", workflow)
        self.assertIn("refs/remotes/origin/a2t-evidence-head:evidence/receipt.json", workflow)
        self.assertIn('git fetch --no-tags --depth=1 origin "$source_revision"', workflow)
        self.assertIn("name: a2t-structural-verification-${{ github.event.pull_request.head.sha }}", workflow)
        self.assertIn("runner.os == 'macOS'", workflow)
        self.assertIn("matrix.key == 'macos'", workflow)
        self.assertIn("github.event_name == 'pull_request'", workflow)
        self.assertIn("if-no-files-found: error", workflow)


if __name__ == "__main__":
    unittest.main()

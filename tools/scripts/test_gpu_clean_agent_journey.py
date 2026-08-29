#!/usr/bin/env python3
"""Planted-negative tests for the two-party GPU clean-agent verifier."""

from __future__ import annotations

import copy
import json
import math
import os
import pathlib
import sys
import tempfile
import time
import unittest
from unittest import mock

import gpu_clean_agent_journey as journey


SYMPTOM = "compute-readback-mismatch"
RECIPE = "gpu-compute.magnitude.v1"


def pass_row(verdict: str = "pass") -> dict:
    failed = verdict == "fail"
    return {
        "sequence": 0,
        "name": "oracle",
        "verdict": verdict,
        "work_completed": True,
        "expected": 0.0,
        "observed": 2.5 if failed else 0.0,
        "absolute_error": 2.5 if failed else 0.0,
        "code": "cpu_oracle_mismatch" if failed else "cpu_oracle_match",
    }


def artifact_rows(contents: dict[str, bytes]) -> list[dict]:
    return [
        {
            "name": name,
            "kind": "numeric-samples",
            "mime": "application/octet-stream",
            "bytes": len(payload),
            "sha256": journey._sha256_bytes(payload),
        }
        for name, payload in sorted(contents.items())
    ]


def result(
    evidence_id: str,
    contents: dict[str, bytes],
    *,
    failed: bool = False,
) -> dict:
    return {
        "schema": journey.RESULT_SCHEMA,
        "version": 1,
        "gpu_evidence_id": evidence_id * 32,
        "recipe_id": RECIPE,
        "source_digest": ("d" if failed else "b") * 64,
        "signature_digest": ("e" if failed else "c") * 64,
        "dimensions": {"width": 1, "height": 1, "work_items": 1},
        "seed": 0,
        "clock": "fixed-step",
        "input_format": "complex-f32",
        "output_format": "f32",
        "encoding": "little-endian-ieee754",
        "tolerance": {"absolute": 0.0, "relative": 0.0},
        "adapter_policy": "hardware-required",
        "adapter": {
            "status": "authentic",
            "class": "hardware",
            "backend": "Metal",
            "name": "Apple M3 Ultra",
            "vendor": "apple",
            "architecture": "metal-3",
            "device": "vendor=0x106b,device=0x0001",
        },
        "numeric_sample_count": 1,
        "mutation": "wgsl-imaginary-weight" if failed else None,
        "verdict": "fail" if failed else "pass",
        "passes": [pass_row("fail" if failed else "pass")],
        "artifacts": artifact_rows(contents),
        "recommendations": [],
    }


def write_json(path: pathlib.Path, value: dict) -> None:
    path.write_bytes(journey._json_bytes(value))


def bound(path: pathlib.Path) -> dict:
    return {"path": str(path), "sha256": journey._sha256(path), "bytes": path.stat().st_size}


class SyntheticCase:
    def __init__(self, base: pathlib.Path) -> None:
        self.base = base
        self.source = base / "source"
        self.workspace = base / "workspace"
        self.private = base / "private"
        self.source.mkdir()
        self.workspace.mkdir()
        self.private.mkdir()
        self.receipt = base / "terminal-receipt.json"
        self.case_path = self.private / "case.json"
        self.session_path = self.private / "agent-session.json"
        self.pulp = pathlib.Path(sys.executable).resolve(strict=True)
        self.docs = ["https://example.com/public-gpu-cli", "https://example.com/public-gpu-guide"]

        task_path = self.workspace / "TASK.md"
        script_path = self.workspace / "run-probe.sh"
        task_path.write_text(journey._task_text(self.docs), encoding="utf-8")
        task_path.chmod(0o600)
        script_path.write_text(journey._seed_script(), encoding="utf-8")
        script_path.chmod(0o755)
        initial_tree = journey._snapshot_tree(self.workspace)

        good = {"input.bin": b"KEEP", "observed.bin": b"GOOD"}
        bad = {"input.bin": b"KEEP", "observed.bin": b"FAIL"}
        self.reference = result("a", good)
        self.negative = result("f", bad, failed=True)
        self.repaired = result("9", good)
        reference_artifacts = self.private / "reference-artifacts"
        self._write_artifacts(reference_artifacts, good)
        reference_result = self.private / "reference-result.json"
        write_json(reference_result, self.reference)

        prompt_path = self.private / "agent-prompt.txt"
        prompt_path.write_text(
            journey._prompt_text(self.workspace, SYMPTOM, self.docs), encoding="utf-8"
        )
        prompt_path.chmod(0o600)
        version_output = "synthetic installed pulp 1.0\n"
        case = {
            "schema": journey.CASE_SCHEMA,
            "status": "awaiting-independent-agent",
            "acceptance_gate_satisfied": False,
            "case_id": "1" * 32,
            "symptom": SYMPTOM,
            "workspace": str(self.workspace),
            "forbidden_roots": [str(self.source)],
            "source_revision": "2" * 40,
            "plan_revision": "3" * 40,
            "public_docs": self.docs,
            "installed_cli": {
                "path": str(self.pulp),
                "bytes": self.pulp.stat().st_size,
                "sha256": journey._sha256(self.pulp),
                "version_output": version_output.strip(),
                "version_output_sha256": journey._sha256_bytes(version_output.encode()),
                "linkage_sha256": journey._installed_cli_linkage_digest(
                    self.pulp, [self.source], self.private, 10.0
                ),
            },
            "harness_sha256": journey._sha256(pathlib.Path(journey.__file__).resolve()),
            "prompt": bound(prompt_path),
            "initial_tree": initial_tree,
            "correction": {
                "path": "run-probe.sh",
                "before": journey.SEED_LINE,
                "after": journey.REPAIRED_LINE,
            },
            "selection": {
                "catalog_schema": journey.CATALOG_SCHEMA,
                "catalog_revision": 1,
                "recipe_id": RECIPE,
            },
            "reference": {
                "command": [
                    str(self.pulp), "gpu", "probe", "--recipe", RECIPE,
                    "--artifacts", str(reference_artifacts), "--json",
                ],
                "exit_code": 0,
                "result_path": str(reference_result),
                "result_json_sha256": journey._sha256(reference_result),
                "result": self.reference,
                "artifacts_sha256": {
                    name: journey._sha256_bytes(payload) for name, payload in sorted(good.items())
                },
                "adapter": self.reference["adapter"],
            },
        }
        write_json(self.case_path, case)

        script_path.write_text(
            journey._seed_script().replace(journey.SEED_LINE, journey.REPAIRED_LINE, 1),
            encoding="utf-8",
        )
        script_path.chmod(0o755)
        discovery = {
            "schema": journey.DISCOVERY_SCHEMA,
            "catalog_revision": 1,
            "recipes": [{"callable": True, "recipe": {"id": RECIPE, "symptoms": [SYMPTOM]}}],
        }
        write_json(self.workspace / "discovery.json", discovery)
        write_json(self.workspace / "negative-result.json", self.negative)
        write_json(self.workspace / "repaired-result.json", self.repaired)
        self._write_artifacts(self.workspace / "artifacts" / "negative", bad)
        self._write_artifacts(self.workspace / "artifacts" / "repaired", good)

        self.final_message = (
            f"Selected {RECIPE}. Negative exit 1 verdict fail: oracle code "
            "cpu_oracle_mismatch mutation wgsl-imaginary-weight expected 0.0 observed 2.5 "
            f"absolute error 2.5. Changed {journey.SEED_LINE} to {journey.REPAIRED_LINE}. "
            "Repaired exit 0 verdict pass."
        )
        events = [
            {"type": "thread.started", "thread_id": "thread-synthetic"},
            {
                "type": "item.completed",
                "item": {
                    "type": "command_execution",
                    "command": "pwd && command -v pulp",
                    "exit_code": 0,
                    "aggregated_output": f"{self.workspace}\n{self.pulp}\n",
                },
            },
            {
                "type": "item.completed",
                "item": {
                    "type": "command_execution",
                    "command": f"./run-probe.sh {SYMPTOM}",
                    "exit_code": 1,
                    "aggregated_output": (
                        f"selected recipe: {RECIPE}\n{json.dumps(self.negative)}\n"
                        "probe exit: 1\n"
                    ),
                },
            },
            {
                "type": "item.completed",
                "item": {
                    "type": "command_execution",
                    "command": (
                        f"python3 edit run-probe.sh '{journey.SEED_LINE}' "
                        f"'{journey.REPAIRED_LINE}'"
                    ),
                    "exit_code": 0,
                    "aggregated_output": "",
                },
            },
            {
                "type": "item.completed",
                "item": {
                    "type": "command_execution",
                    "command": f"./run-probe.sh {SYMPTOM}",
                    "exit_code": 0,
                    "aggregated_output": (
                        f"selected recipe: {RECIPE}\n{json.dumps(self.repaired)}\n"
                        "probe exit: 0\n"
                    ),
                },
            },
            {"type": "item.completed", "item": {"type": "agent_message", "text": self.final_message}},
            {"type": "turn.completed", "usage": {"input_tokens": 1, "output_tokens": 1}},
        ]
        self.transcript_path = self.private / "agent-transcript.jsonl"
        self.transcript_path.write_text(
            "".join(json.dumps(event, sort_keys=True) + "\n" for event in events),
            encoding="utf-8",
        )
        self.stderr_path = self.private / "agent-stderr.log"
        self.stderr_path.write_bytes(b"")
        self.last_message_path = self.private / "agent-last-message.txt"
        self.last_message_path.write_text(self.final_message + "\n", encoding="utf-8")
        prompt_sha = case["prompt"]["sha256"]
        runtime = self.base.parent / f"pulp-gpu-agent-runtime-{self.base.name}"
        session = {
            "schema": journey.SESSION_SCHEMA,
            "status": "agent-finished-awaiting-verification",
            "acceptance_gate_satisfied": False,
            "case_id": case["case_id"],
            "case_sha256": journey._sha256(self.case_path),
            "session_id": "thread-synthetic",
            "model": "gpt-5.6-sol",
            "cwd": str(self.workspace),
            "path": os.pathsep.join([str(self.pulp.parent), os.defpath]),
            "home": str(runtime / "home"),
            "config_home": str(runtime / "config"),
            "codex_home": str(runtime / "codex"),
            "runtime_removed": True,
            "environment_keys": sorted(
                ["HOME", "XDG_CONFIG_HOME", "CODEX_HOME", "PATH", "PWD", "NO_COLOR"]
            ),
            "agent_binary": {
                "path": str(self.pulp),
                "sha256": journey._sha256(self.pulp),
                "version": "codex-cli synthetic",
            },
            "installed_cli_sha256": case["installed_cli"]["sha256"],
            "source_revision": case["source_revision"],
            "plan_revision": case["plan_revision"],
            "prompt_sha256": prompt_sha,
            "command_argv": [
                str(self.pulp), "exec", "--json", "--ephemeral", "--ignore-user-config",
                "--ignore-rules", "--skip-git-repo-check", "--sandbox", "workspace-write",
                "--color", "never", "--model", "gpt-5.6-sol", "--cd", str(self.workspace),
                "--output-last-message",
                str(runtime / "last-message.txt"), f"<prompt-sha256:{prompt_sha}>",
            ],
            "agent_exit_code": 0,
            "transcript": {**bound(self.transcript_path), "events": len(events)},
            "stderr": bound(self.stderr_path),
            "last_message": bound(self.last_message_path),
            "final_tree": journey._snapshot_tree(self.workspace),
        }
        write_json(self.session_path, session)

    @staticmethod
    def _write_artifacts(directory: pathlib.Path, contents: dict[str, bytes]) -> None:
        directory.mkdir(parents=True)
        for name, payload in contents.items():
            (directory / name).write_bytes(payload)

    def session(self) -> dict:
        return json.loads(self.session_path.read_text(encoding="utf-8"))

    def write_session(self, value: dict) -> None:
        write_json(self.session_path, value)


class CleanAgentHarnessTests(unittest.TestCase):
    def test_agent_environment_is_sanitized_and_disposable(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = pathlib.Path(raw).resolve()
            workspace = root / "workspace"
            home = root / "home"
            config = root / "config"
            codex_home = root / "codex"
            for directory in (workspace, home, config, codex_home):
                directory.mkdir()
            with mock.patch.dict(
                os.environ,
                {"OPENAI_API_KEY": "test-only", "GIT_DIR": "/private/source/.git", "PATH": "/bad"},
                clear=True,
            ):
                environment = journey._fresh_agent_environment(
                    pathlib.Path(sys.executable).resolve(), workspace, home, config, codex_home
                )
            self.assertNotIn("GIT_DIR", environment)
            self.assertEqual(environment["HOME"], str(home))
            self.assertEqual(environment["XDG_CONFIG_HOME"], str(config))
            self.assertEqual(environment["CODEX_HOME"], str(codex_home))
            self.assertEqual(environment["PWD"], str(workspace))
            self.assertNotIn("/bad", environment["PATH"])

    @unittest.skipUnless(sys.platform == "darwin", "Mach-O linkage audit is macOS-specific")
    def test_installed_cli_linkage_rejects_a_source_rpath(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = pathlib.Path(raw).resolve()
            source = root / "source"
            source.mkdir()
            responses = [
                journey.CommandResult(0, f"path {source}/external/v8/lib\n", ""),
                journey.CommandResult(0, "@rpath/libv8.dylib\n", ""),
            ]
            with mock.patch.object(journey, "_run", side_effect=responses):
                with self.assertRaisesRegex(journey.JourneyError, "source-checkout"):
                    journey._installed_cli_linkage_digest(
                        pathlib.Path(sys.executable).resolve(), [source], root, 10.0
                    )

    def test_result_shape_is_closed_and_numeric_evidence_is_coherent(self) -> None:
        evidence = result("a", {"input.bin": b"KEEP", "observed.bin": b"GOOD"})
        journey._validate_result_schema_shape(evidence)
        for mutation in (
            {**evidence, "extra": True},
            {**evidence, "dimensions": {"width": 0, "height": 1, "work_items": 1}},
            {**evidence, "tolerance": {"absolute": math.inf, "relative": 0.0}},
        ):
            with self.assertRaises(journey.JourneyError):
                journey._validate_result_schema_shape(mutation)
        row = pass_row()
        row["observed"] = 1.0
        with self.assertRaises(journey.JourneyError):
            journey._typed_passes({"passes": [row]})

    def test_paths_and_artifacts_reject_symlink_escape(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = pathlib.Path(raw).resolve()
            real = root / "real"
            real.mkdir()
            alias = root / "alias"
            alias.symlink_to(real, target_is_directory=True)
            with self.assertRaises(journey.JourneyError):
                journey._require_real_directory(alias)
            artifacts = real / "artifacts"
            artifacts.mkdir()
            outside = root / "outside.bin"
            outside.write_bytes(b"KEEP")
            (artifacts / "input.bin").symlink_to(outside)
            evidence = {"artifacts": artifact_rows({"input.bin": b"KEEP"})}
            with self.assertRaises(journey.JourneyError):
                journey._verified_artifacts(evidence, artifacts, real)
            hardlink_artifacts = real / "hardlink-artifacts"
            hardlink_artifacts.mkdir()
            os.link(outside, hardlink_artifacts / "input.bin")
            with self.assertRaises(journey.JourneyError):
                journey._verified_artifacts(evidence, hardlink_artifacts, real)

    def test_stdout_and_stderr_are_capped_during_production(self) -> None:
        for descriptor, label in ((1, "stdout"), (2, "stderr")):
            with self.subTest(label=label):
                with self.assertRaisesRegex(journey.JourneyError, label):
                    journey._run(
                        [sys.executable, "-c", f"import os; os.write({descriptor}, b'x' * 131072)"],
                        5.0,
                        stdout_limit=4096,
                        stderr_limit=4096,
                    )

    def test_noisy_workspace_is_killed_while_it_is_growing(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = pathlib.Path(raw).resolve()
            started = time.monotonic()
            with self.assertRaisesRegex(journey.JourneyError, "production bound"):
                journey._run(
                    [
                        sys.executable,
                        "-c",
                        (
                            "import os,time; p=open('noise.bin','wb',buffering=0); "
                            "[(p.write(b'x'*4096),time.sleep(.01)) for _ in range(1000)]"
                        ),
                    ],
                    5.0,
                    cwd=root,
                    monitor_roots=[journey.ResourceRoot(root, 8192, 8192, 8)],
                )
            self.assertLess(time.monotonic() - started, 2.0)

    def test_timeout_terminates_the_entire_process_group(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = pathlib.Path(raw).resolve()
            marker = root / "escaped-marker"
            child = (
                "import pathlib,time; time.sleep(.4); "
                f"pathlib.Path({str(marker)!r}).write_text('escaped')"
            )
            parent = (
                "import subprocess,sys,time; "
                f"subprocess.Popen([sys.executable,'-c',{child!r}]); time.sleep(5)"
            )
            with self.assertRaisesRegex(journey.JourneyError, "exceeded"):
                journey._run([sys.executable, "-c", parent], 0.1, cwd=root)
            time.sleep(0.6)
            self.assertFalse(marker.exists())

    def test_transcript_requires_a_real_completed_session(self) -> None:
        payload = b'{"type":"thread.started","thread_id":"abc"}\n{"type":"turn.completed"}\n'
        events, identity = journey._parse_transcript(payload)
        self.assertEqual(identity, "abc")
        self.assertEqual(len(events), 2)
        with self.assertRaises(journey.JourneyError):
            journey._parse_transcript(
                b'{"type":"thread.started","thread_id":"abc"}\n{"type":"turn.failed"}\n'
            )

    def test_terminal_verifier_accepts_a_bound_two_party_journey(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            fixture = SyntheticCase(pathlib.Path(raw).resolve())
            receipt = journey.verify_case(fixture.case_path, fixture.session_path, fixture.receipt)
            self.assertTrue(receipt["acceptance_gate_satisfied"])
            self.assertEqual(receipt["status"], "independent-agent-accepted")
            self.assertEqual(receipt["workspace"]["exact_tree_diff"]["modified"], ["run-probe.sh"])

    def test_verifier_rejects_extra_agent_edit_even_if_session_tree_is_rebound(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            fixture = SyntheticCase(pathlib.Path(raw).resolve())
            script = fixture.workspace / "run-probe.sh"
            script.write_text(script.read_text() + "# extra\n", encoding="utf-8")
            script.chmod(0o755)
            session = fixture.session()
            session["final_tree"] = journey._snapshot_tree(fixture.workspace)
            fixture.write_session(session)
            with self.assertRaisesRegex(journey.JourneyError, "exactly"):
                journey.verify_case(fixture.case_path, fixture.session_path, fixture.receipt)

    def test_verifier_rejects_tampered_negative_artifact(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            fixture = SyntheticCase(pathlib.Path(raw).resolve())
            (fixture.workspace / "artifacts" / "negative" / "observed.bin").write_bytes(b"EVIL")
            session = fixture.session()
            session["final_tree"] = journey._snapshot_tree(fixture.workspace)
            fixture.write_session(session)
            with self.assertRaisesRegex(journey.JourneyError, "digest"):
                journey.verify_case(fixture.case_path, fixture.session_path, fixture.receipt)

    def test_verifier_rejects_replayed_reference_as_repaired_proof(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            fixture = SyntheticCase(pathlib.Path(raw).resolve())
            replay = copy.deepcopy(fixture.reference)
            write_json(fixture.workspace / "repaired-result.json", replay)
            session = fixture.session()
            session["final_tree"] = journey._snapshot_tree(fixture.workspace)
            fixture.write_session(session)
            with self.assertRaisesRegex(journey.JourneyError, "independent executions"):
                journey.verify_case(fixture.case_path, fixture.session_path, fixture.receipt)

    def test_verifier_rejects_missing_typed_diagnosis_in_final_message(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            fixture = SyntheticCase(pathlib.Path(raw).resolve())
            events = [json.loads(line) for line in fixture.transcript_path.read_text().splitlines()]
            events[-2]["item"]["text"] = "It failed, then passed."
            fixture.transcript_path.write_text(
                "".join(json.dumps(event, sort_keys=True) + "\n" for event in events),
                encoding="utf-8",
            )
            fixture.last_message_path.write_text("It failed, then passed.\n", encoding="utf-8")
            session = fixture.session()
            session["transcript"] = {**bound(fixture.transcript_path), "events": len(events)}
            session["last_message"] = bound(fixture.last_message_path)
            fixture.write_session(session)
            with self.assertRaisesRegex(journey.JourneyError, "interpret"):
                journey.verify_case(fixture.case_path, fixture.session_path, fixture.receipt)

    def test_verifier_rejects_a_workspace_edit_missing_from_agent_transcript(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            fixture = SyntheticCase(pathlib.Path(raw).resolve())
            events = [json.loads(line) for line in fixture.transcript_path.read_text().splitlines()]
            events = [
                event
                for event in events
                if "python3 edit run-probe.sh" not in json.dumps(event, sort_keys=True)
            ]
            fixture.transcript_path.write_text(
                "".join(json.dumps(event, sort_keys=True) + "\n" for event in events),
                encoding="utf-8",
            )
            session = fixture.session()
            session["transcript"] = {**bound(fixture.transcript_path), "events": len(events)}
            fixture.write_session(session)
            with self.assertRaisesRegex(journey.JourneyError, "agent's edit"):
                journey.verify_case(fixture.case_path, fixture.session_path, fixture.receipt)

    def test_checked_in_legacy_receipt_is_only_a_nonterminal_disposition(self) -> None:
        path = pathlib.Path(__file__).resolve().parents[2] / (
            "docs/validation/gpu-clean-agent/m3-a5-clean-agent-20260828.json"
        )
        disposition = json.loads(path.read_text(encoding="utf-8"))
        journey._validate_superseded_disposition(disposition)
        self.assertEqual(disposition["schema"], journey.SUPERSEDED_SCHEMA)
        self.assertEqual(disposition["status"], "superseded-nonterminal")
        self.assertIs(disposition["acceptance_gate_satisfied"], False)
        self.assertNotIn("verdict", disposition)


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Planted-negative tests for the two-party GPU clean-agent trust gate."""

from __future__ import annotations

import json
import math
import os
import pathlib
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

import gpu_clean_agent_journey as journey
import gpu_clean_agent_trust as trust


SYMPTOM = "compute-readback-mismatch"
RECIPE = "gpu-compute.magnitude.v1"
THREAD_ID = "12345678-1234-4234-8234-123456789abc"
TURN_ID = "87654321-4321-4321-8321-cba987654321"
NONCE = "a" * 64


def run(*argv: str, cwd: pathlib.Path) -> str:
    completed = subprocess.run(argv, cwd=cwd, check=True, capture_output=True, text=True)
    return completed.stdout.strip()


def init_repo(
    root: pathlib.Path, files: dict[str, str],
    repository: str = "Generous-Corp/pulp",
) -> str:
    root.mkdir()
    run("git", "init", "-q", cwd=root)
    run("git", "config", "user.name", "A5 Test", cwd=root)
    run("git", "config", "user.email", "a5@example.invalid", cwd=root)
    for relative, text in files.items():
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
    run("git", "add", ".", cwd=root)
    run("git", "commit", "-qm", "fixture", cwd=root)
    revision = run("git", "rev-parse", "HEAD", cwd=root)
    run("git", "remote", "add", "origin", f"git@github.com:{repository}.git", cwd=root)
    run("git", "update-ref", "refs/remotes/origin/main", revision, cwd=root)
    return revision


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


def result(evidence_id: str, contents: dict[str, bytes], *, failed: bool = False) -> dict:
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
            "status": "authentic", "class": "hardware", "backend": "Metal",
            "name": "Apple M3 Ultra", "vendor": "apple", "architecture": "metal-3",
            "device": "vendor=0x106b,device=0x0001",
        },
        "numeric_sample_count": 1,
        "mutation": "wgsl-imaginary-weight" if failed else None,
        "verdict": "fail" if failed else "pass",
        "passes": [pass_row("fail" if failed else "pass")],
        "artifacts": artifact_rows(contents),
        "recommendations": [],
    }


def codex_rollout(*, originator: str = "codex_exec", nonce: str = NONCE) -> bytes:
    events = [
        {
            "timestamp": "2026-08-29T00:00:00Z", "type": "session_meta", "ordinal": 0,
            "payload": {
                "id": THREAD_ID, "session_id": THREAD_ID, "cwd": "/proof/workspace",
                "originator": originator, "source": "exec", "cli_version": "0.150.1",
            },
        },
        {
            "timestamp": "2026-08-29T00:00:01Z", "type": "turn_context", "ordinal": 1,
            "payload": {
                "cwd": "/proof/workspace", "model": "gpt-5.6-sol",
                "approval_policy": "never", "sandbox_policy": {"type": "danger-full-access"},
                "turn_id": TURN_ID,
            },
        },
        {
            "timestamp": "2026-08-29T00:00:02Z", "type": "event_msg", "ordinal": 2,
            "payload": {"type": "task_started", "turn_id": TURN_ID},
        },
        {
            "timestamp": "2026-08-29T00:00:03Z", "type": "response_item", "ordinal": 3,
            "payload": {"type": "message", "role": "user", "content": nonce},
        },
        {
            "timestamp": "2026-08-29T00:00:04Z", "type": "event_msg", "ordinal": 4,
            "payload": {"type": "task_complete", "turn_id": TURN_ID},
        },
    ]
    return b"".join(
        json.dumps(event, sort_keys=True).encode("utf-8") + b"\n" for event in events
    )


class CleanAgentHarnessTests(unittest.TestCase):
    def test_bounded_trust_command_kills_stream_at_cap(self) -> None:
        started = time.monotonic()
        with self.assertRaisesRegex(trust.isolation.IsolationError, "stdout cap"):
            trust.isolation.bounded_run(
                [
                    sys.executable, "-c",
                    "import os\nwhile True: os.write(1, b'x' * 65536)",
                ],
                timeout=10.0,
                output_limit=1024,
            )
        self.assertLess(time.monotonic() - started, 3.0)

    def test_connect_proxy_allows_only_named_https_host_and_records_bytes(self) -> None:
        proxy_side, server_side = socket.socketpair()

        def connector(address: tuple[str, int], timeout: float) -> socket.socket:
            self.assertEqual(address, ("chatgpt.com", 443))
            self.assertEqual(timeout, 10.0)
            return proxy_side

        with trust.ExactHostConnectProxy({"chatgpt.com"}, connector=connector) as proxy:
            client = socket.create_connection(("127.0.0.1", proxy.port))
            client.sendall(b"CONNECT chatgpt.com:443 HTTP/1.1\r\n\r\n")
            self.assertIn(b"200 Connection Established", client.recv(1024))
            client.sendall(b"ping")
            self.assertEqual(server_side.recv(4), b"ping")
            server_side.sendall(b"pong")
            self.assertEqual(client.recv(4), b"pong")
            client.close()
            server_side.close()
        audit = proxy.audit()
        self.assertEqual(audit["allowed_hosts"], ["chatgpt.com"])
        self.assertEqual(audit["connections"][0]["outcome"], "completed")
        self.assertEqual(audit["connections"][0]["bytes_to_upstream"], 4)
        self.assertEqual(audit["connections"][0]["bytes_to_client"], 4)

    def test_connect_proxy_rejects_unlisted_host_and_byte_cap(self) -> None:
        with self.assertRaisesRegex(trust.isolation.IsolationError, "allowlisted"):
            with trust.ExactHostConnectProxy({"chatgpt.com"}) as proxy:
                client = socket.create_connection(("127.0.0.1", proxy.port))
                client.sendall(b"CONNECT example.com:443 HTTP/1.1\r\n\r\n")
                self.assertIn(b"403 Forbidden", client.recv(1024))
                client.close()
        proxy_side, server_side = socket.socketpair()
        with self.assertRaisesRegex(trust.isolation.IsolationError, "byte cap"):
            with trust.ExactHostConnectProxy(
                {"chatgpt.com"}, max_bytes_each_way=4,
                connector=lambda address, timeout: proxy_side,
            ) as proxy:
                client = socket.create_connection(("127.0.0.1", proxy.port))
                client.sendall(b"CONNECT chatgpt.com:443 HTTP/1.1\r\n\r\n")
                self.assertIn(b"200 Connection Established", client.recv(1024))
                client.sendall(b"12345")
                server_side.recv(5)
                client.close()
                server_side.close()

    def test_keychain_staging_uses_codex_only_acl_and_no_auth_file(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = pathlib.Path(raw).resolve()
            codex_home = root / "codex"
            codex_home.mkdir()
            agent = root / "codex-bin"
            agent.write_bytes(b"fixture")
            payload = b'{"OPENAI_API_KEY":"secret-123456","auth_mode":"apikey"}'
            calls: list[tuple[list[str], bytes | None]] = []
            find_count = 0

            def fake_run(argv: list[str], **kwargs: object) -> subprocess.CompletedProcess[bytes]:
                nonlocal find_count
                calls.append((argv, kwargs.get("input_bytes")))
                if "find-generic-password" in argv:
                    find_count += 1
                    return subprocess.CompletedProcess(argv, 1 if find_count == 1 else 0, b"", b"")
                return subprocess.CompletedProcess(argv, 0, b"", b"")

            with mock.patch.object(trust, "_run_bytes", side_effect=fake_run):
                record = trust.install_codex_keychain_auth(
                    codex_home=codex_home, agent_bin=agent, auth_payload=payload,
                )
            add_argv, add_input = next(item for item in calls if "add-generic-password" in item[0])
            self.assertEqual(add_argv[-1], "-w")
            self.assertIn(str(agent), add_argv)
            self.assertIn("", add_argv)
            self.assertNotIn(payload.decode(), " ".join(add_argv))
            self.assertEqual(add_input, payload + b"\n")
            self.assertTrue(record["creator_access_removed"])
            self.assertFalse((codex_home / "auth.json").exists())

    def test_terminal_pair_rolls_back_and_recovers_exact_staged_bundle(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = pathlib.Path(raw).resolve()
            bundle, receipt = root / "bundle.json", root / "receipt.json"
            real_write = journey._write_bytes

            def fail_receipt(path: pathlib.Path, payload: bytes, **kwargs: object) -> None:
                if path == receipt:
                    raise journey.JourneyError("planted receipt failure")
                real_write(path, payload, **kwargs)

            with mock.patch.object(journey, "_write_bytes", side_effect=fail_receipt):
                with self.assertRaisesRegex(journey.JourneyError, "planted receipt"):
                    journey._publish_terminal_pair(
                        bundle_path=bundle, bundle_payload=b"bundle",
                        receipt_path=receipt, receipt_payload=b"receipt",
                    )
            self.assertEqual(bundle.read_bytes(), b"bundle")
            self.assertFalse(receipt.exists())
            journey._publish_terminal_pair(
                bundle_path=bundle, bundle_payload=b"bundle",
                receipt_path=receipt, receipt_payload=b"receipt",
            )
            self.assertEqual(receipt.read_bytes(), b"receipt")
            receipt.unlink()
            with self.assertRaisesRegex(journey.JourneyError, "not this exact"):
                journey._publish_terminal_pair(
                    bundle_path=bundle, bundle_payload=b"different",
                    receipt_path=receipt, receipt_payload=b"receipt",
                )

    def test_terminal_pair_recovers_receipt_parent_fsync_failure(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = pathlib.Path(raw).resolve()
            bundle, receipt = root / "bundle.json", root / "receipt.json"
            real_fsync_directory = journey._fsync_directory
            calls = 0

            def fail_receipt_fsync_once(path: pathlib.Path) -> None:
                nonlocal calls
                calls += 1
                if calls == 2:
                    raise OSError("planted post-publication fsync failure")
                real_fsync_directory(path)

            with mock.patch.object(
                journey, "_fsync_directory", side_effect=fail_receipt_fsync_once,
            ):
                journey._publish_terminal_pair(
                    bundle_path=bundle, bundle_payload=b"bundle",
                    receipt_path=receipt, receipt_payload=b"receipt",
                )
            self.assertEqual(calls, 3)
            self.assertEqual(bundle.read_bytes(), b"bundle")
            self.assertEqual(receipt.read_bytes(), b"receipt")

    def test_agent_environment_is_sanitized_and_disposable(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = pathlib.Path(raw).resolve()
            workspace, home, config, codex_home = (
                root / "workspace", root / "home", root / "config", root / "codex"
            )
            for directory in (workspace, home, config, codex_home):
                directory.mkdir()
            with mock.patch.dict(
                os.environ,
                {"OPENAI_API_KEY": "test-only", "GIT_DIR": "/private/source/.git", "PATH": "/bad"},
                clear=True,
            ):
                environment = journey._fresh_agent_environment(
                    pathlib.Path(sys.executable).resolve(), workspace, home, config, codex_home,
                    43123,
                )
            self.assertNotIn("GIT_DIR", environment)
            self.assertNotIn("OPENAI_API_KEY", environment)
            self.assertEqual(environment["CODEX_HOME"], str(codex_home))
            self.assertEqual(environment["PWD"], str(workspace))
            self.assertEqual(environment["HTTPS_PROXY"], "http://127.0.0.1:43123")
            self.assertNotIn("/bad", environment["PATH"])

    def test_task_and_prompt_disclose_neither_recipe_nor_fix(self) -> None:
        task = journey._task_text()
        prompt = journey._prompt_text(pathlib.Path("/proof/workspace"), SYMPTOM, NONCE)
        for text in (task, prompt):
            self.assertNotIn(RECIPE, text)
            self.assertNotIn(journey.SEED_LINE, text)
            self.assertNotIn(journey.REPAIRED_LINE, text)
            self.assertNotIn("https://", text)
        self.assertIn(SYMPTOM, prompt)
        self.assertIn(NONCE, prompt)

    def test_structured_final_output_is_nonce_and_evidence_shaped(self) -> None:
        case = {"run_nonce": NONCE, "symptom": SYMPTOM}
        value = {
            "run_nonce": NONCE,
            "symptom": SYMPTOM,
            "selected_recipe": RECIPE,
            "negative": {
                "exit_code": 1, "verdict": "fail", "pass": "oracle",
                "code": "cpu_oracle_mismatch", "mutation": "wgsl-imaginary-weight",
                "expected": 0.0, "observed": 2.5, "absolute_error": 2.5,
            },
            "edit": {"path": "run-probe.sh", "removed": journey.SEED_LINE,
                     "added": journey.REPAIRED_LINE},
            "repaired": {"exit_code": 0, "verdict": "pass"},
        }
        self.assertEqual(
            journey._parse_agent_final_output(json.dumps(value).encode(), case), value
        )
        value["run_nonce"] = "b" * 64
        with self.assertRaisesRegex(journey.JourneyError, "closed output"):
            journey._parse_agent_final_output(json.dumps(value).encode(), case)

    def test_transcript_requires_ordered_uuid_session_not_old_handcraft(self) -> None:
        payload = b"".join([
            json.dumps({"type": "thread.started", "thread_id": THREAD_ID}).encode() + b"\n",
            b'{"type":"turn.started"}\n',
            b'{"type":"turn.completed"}\n',
        ])
        _, identity = journey._parse_transcript(payload)
        self.assertEqual(identity, THREAD_ID)
        with self.assertRaises(journey.JourneyError):
            journey._parse_transcript(
                b'{"type":"thread.started","thread_id":"thread-synthetic"}\n'
                b'{"type":"turn.completed"}\n'
            )

    def test_codex_rollout_binds_origin_thread_model_nonce_and_completion(self) -> None:
        identity = trust.parse_codex_rollout(
            codex_rollout(), thread_id=THREAD_ID, cli_version="0.150.1",
            workspace=pathlib.Path("/proof/workspace"), model="gpt-5.6-sol", run_nonce=NONCE,
        )
        self.assertEqual(identity["turn_id"], TURN_ID)
        with self.assertRaisesRegex(trust.TrustError, "metadata"):
            trust.parse_codex_rollout(
                codex_rollout(originator="python-fixture"), thread_id=THREAD_ID,
                cli_version="0.150.1", workspace=pathlib.Path("/proof/workspace"),
                model="gpt-5.6-sol", run_nonce=NONCE,
            )
        with self.assertRaisesRegex(trust.TrustError, "nonce"):
            trust.parse_codex_rollout(
                codex_rollout(nonce="b" * 64), thread_id=THREAD_ID, cli_version="0.150.1",
                workspace=pathlib.Path("/proof/workspace"), model="gpt-5.6-sol",
                run_nonce=NONCE,
            )

    def test_public_rollout_projection_drops_instructions_and_private_payloads(self) -> None:
        events = [json.loads(line) for line in codex_rollout().splitlines()]
        events[0]["payload"]["base_instructions"] = "private-instructions"
        events[1]["payload"]["summary"] = "private-summary"
        events.insert(-1, {
            "timestamp": "2026-08-29T00:00:03Z",
            "type": "response_item",
            "ordinal": 4,
            "payload": {
                "type": "custom_tool_call", "name": "exec", "input": "pwd",
                "internal_chat_message_metadata_passthrough": "private-metadata",
            },
        })
        payload = b"".join(json.dumps(event).encode() + b"\n" for event in events)
        public = journey._public_rollout_events(payload)
        corpus = json.dumps(public, sort_keys=True)
        self.assertNotIn("private-instructions", corpus)
        self.assertNotIn("private-summary", corpus)
        self.assertNotIn("private-metadata", corpus)
        self.assertIn(THREAD_ID, corpus)
        self.assertIn('"input": "pwd"', corpus)

    def test_installed_discovery_must_precede_probe_and_rejects_network_tools(self) -> None:
        case = {
            "symptom": SYMPTOM,
            "selection": {"recipe_id": RECIPE},
            "public_material": [
                {
                    "role": "guide",
                    "installed_relative_path": (
                        "share/pulp/docs/guides/gpu-validation-checklist.md"
                    ),
                    "source_path": "docs/guides/gpu-validation-checklist.md",
                }
            ],
        }
        records = [
            ("pwd && command -v pulp", 0, "/proof/workspace\n/proof/installed/bin/pulp\n"),
            (
                f"pulp gpu recipes list --symptom {SYMPTOM} --json", 0,
                json.dumps({"symptom": SYMPTOM, "recipe": RECIPE}),
            ),
            (
                "cat /proof/installed/share/pulp/gpu-recipes.yaml", 0,
                f"id: {RECIPE}\nsymptoms: [{SYMPTOM}]\n"
                "docs: [docs/guides/gpu-validation-checklist.md]\n",
            ),
            (
                "cat /proof/installed/share/pulp/docs/guides/gpu-validation-checklist.md",
                0, "installed public GPU validation guidance " * 2,
            ),
            (f"./run-probe.sh {SYMPTOM}", 1, "probe exit: 1"),
            ("sed -i '' edit run-probe.sh", 0, ""),
            (f"./run-probe.sh {SYMPTOM}", 0, "probe exit: 0"),
        ]
        journey._validate_public_discovery_sequence(case, [], records, [4, 6])
        network_records = [*records[:4], ("curl https://example.invalid", 0, ""), *records[4:]]
        with self.assertRaisesRegex(journey.JourneyError, "installed catalog"):
            journey._validate_public_discovery_sequence(case, [], network_records, [5, 7])
        with self.assertRaisesRegex(journey.JourneyError, "installed catalog"):
            journey._validate_public_discovery_sequence(
                case, [{"type": "web_search", "query": "GPU docs"}], records, [4, 6]
            )
        late_docs = [records[0], records[1], records[4], records[2], records[3], *records[5:]]
        with self.assertRaisesRegex(journey.JourneyError, "installed catalog"):
            journey._validate_public_discovery_sequence(case, [], late_docs, [2, 6])

    def test_credential_disclosure_scan_rejects_copied_authentication(self) -> None:
        journey._reject_credential_disclosure([b"safe transcript"], ["private-token-value"])
        with self.assertRaisesRegex(journey.JourneyError, "authentication"):
            journey._reject_credential_disclosure(
                [b"tool output private-token-value"], ["private-token-value"]
            )

    @unittest.skipUnless(sys.platform == "darwin", "official Codex identity is macOS-specific")
    def test_fake_agent_executable_is_rejected_before_launch(self) -> None:
        with self.assertRaisesRegex(trust.TrustError, "codex|Mach-O"):
            trust.official_codex_identity(pathlib.Path(sys.executable).resolve())

    @unittest.skipUnless(
        sys.platform == "darwin" and shutil.which("codex"),
        "installed signed Codex is unavailable",
    )
    def test_installed_signed_codex_identity_when_available(self) -> None:
        identity = trust.official_codex_identity(pathlib.Path(shutil.which("codex")).resolve())
        self.assertEqual(identity["team_identifier"], trust.OPENAI_TEAM_ID)
        self.assertEqual(identity["identifier"], "codex")
        self.assertRegex(identity["version"], r"^codex-cli [0-9]+\.[0-9]+\.[0-9]+")

    @unittest.skipUnless(sys.platform == "darwin", "Seatbelt is macOS-specific")
    def test_outer_seatbelt_denies_direct_relative_symlink_reads_and_other_writes(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = pathlib.Path(raw).resolve()
            workspace, runtime = root / "workspace", root / "runtime"
            workspace.mkdir()
            runtime.mkdir()
            (workspace / "TASK.md").write_text("public", encoding="utf-8")
            fixture_bin = workspace / "fixture-agent"
            fixture_bin.write_text(
                "#!/bin/sh\n"
                "if [ \"$1\" = --version ]; then echo fixture-agent; exit 0; fi\n"
                "if [ \"$1\" = exec ] && [ \"$2\" = --help ]; then exit 0; fi\n"
                "if [ \"$1\" = version ]; then echo fixture-pulp; exit 0; fi\n"
                "exit 2\n",
                encoding="utf-8",
            )
            fixture_bin.chmod(0o755)
            denied: dict[str, pathlib.Path] = {}
            denied_roots: list[pathlib.Path] = []
            for label in ("private-case", "source-checkout", "canonical-plan", "build-tree"):
                directory = root / label
                directory.mkdir()
                probe = directory / "secret.txt"
                probe.write_text("secret", encoding="utf-8")
                denied[label] = probe
                denied_roots.append(directory)
            profile = runtime / "profile.sb"
            codex_home = runtime / "codex"
            codex_home.mkdir()
            listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            listener.bind(("127.0.0.1", 0))
            listener.listen(1)
            proxy_port = int(listener.getsockname()[1])
            profile.write_text(
                trust.seatbelt_profile(
                    workspace=workspace,
                    runtime_root=runtime,
                    installed_prefix=workspace,
                    agent_bin=fixture_bin,
                    denied_roots=denied_roots,
                    network_proxy_port=proxy_port,
                ),
                encoding="utf-8",
            )
            environment = journey._fresh_agent_environment(
                fixture_bin, workspace, runtime, runtime, codex_home, proxy_port,
            )
            real_run = trust._run_bytes

            def login_control(argv: list[str], **kwargs: object) -> object:
                if "login" in argv and "status" in argv:
                    return subprocess.CompletedProcess(argv, 0, b"", b"Logged in using ChatGPT\n")
                return real_run(argv, **kwargs)

            try:
                with mock.patch.object(trust, "_run_bytes", side_effect=login_control):
                    outcome = trust.seatbelt_preflight(
                        profile_path=profile,
                        workspace=workspace,
                        denied_probes=denied,
                        agent_bin=fixture_bin,
                        agent_version="fixture-agent",
                        installed_pulp=fixture_bin,
                        network_proxy_port=proxy_port,
                        codex_home=codex_home,
                        agent_environment=environment,
                    )
            finally:
                listener.close()
            self.assertTrue(outcome["passed"])
            kinds = {(item["label"], item["kind"]) for item in outcome["controls"]}
            self.assertIn(("denied-symlink", "symlink-denied"), kinds)
            self.assertIn(("non-allowlisted-read", "absolute-denied"), kinds)
            self.assertIn(("non-allowlisted-write", "write-denied"), kinds)

    @unittest.skipUnless(sys.platform == "darwin", "one-use signer is macOS-specific")
    def test_record_signature_rejects_handcrafted_or_tampered_session(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = pathlib.Path(raw).resolve()
            key = trust.create_record_keypair(root)
            core = {"case_id": "1" * 32, "transcript_sha256": "2" * 64}
            attestation = trust.sign_record(core, pathlib.Path(key["private_key_path"]))
            trust.verify_record_signature(
                core, attestation, pathlib.Path(key["public_key_path"])
            )
            with self.assertRaisesRegex(trust.TrustError, "bind|verification failed"):
                trust.verify_record_signature(
                    {**core, "transcript_sha256": "3" * 64}, attestation,
                    pathlib.Path(key["public_key_path"]),
                )
            with self.assertRaisesRegex(trust.TrustError, "closed field"):
                trust.verify_record_signature(core, {}, pathlib.Path(key["public_key_path"]))
            case = {
                "build": {"root": "/private/build"},
                "source": {"root": "/private/source"},
                "plan": {"root": "/private/plan"},
                "workspace": "/private/workspace",
                "installed_cli": {"prefix": "/private/install"},
            }
            session_core = {
                "home": "/private/runtime/home",
                "agent_binary": {"path": "/private/codex"},
                "command_argv": ["/private/codex", "--cd", "/private/workspace"],
            }
            replacements = journey._redaction_pairs(
                case, pathlib.Path("/private/case/case.json"), session_core
            )
            redacted_core = journey._redact_value(session_core, replacements)
            redacted_attestation = trust.sign_record(
                redacted_core, pathlib.Path(key["private_key_path"])
            )
            trust.verify_record_signature(
                redacted_core, redacted_attestation, pathlib.Path(key["public_key_path"])
            )
            with self.assertRaisesRegex(trust.TrustError, "bind|verification failed"):
                trust.verify_record_signature(
                    {**redacted_core, "home": "$TAMPERED"}, redacted_attestation,
                    pathlib.Path(key["public_key_path"]),
                )

    def test_git_provenance_is_derived_and_rejects_arbitrary_or_dirty_state(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = pathlib.Path(raw).resolve() / "repo"
            revision = init_repo(
                root, {"plan.md": "canonical\n"}, "danielraffel/pulp-planning"
            )
            identity = trust.git_repository_identity(
                root,
                expected_repository="danielraffel/pulp-planning",
                required_document=root / "plan.md",
                require_origin_main=True,
            )
            self.assertEqual(identity["revision"], revision)
            self.assertEqual(identity["document"]["sha256"], trust.sha256_bytes(b"canonical\n"))
            (root / "untracked.txt").write_text("planted", encoding="utf-8")
            with self.assertRaisesRegex(trust.TrustError, "completely clean"):
                trust.git_repository_identity(
                    root, expected_repository="danielraffel/pulp-planning"
                )
            allowed = trust.git_repository_identity(
                root,
                expected_repository="danielraffel/pulp-planning",
                allowed_untracked=(root / "untracked.txt",),
            )
            self.assertTrue(allowed["clean"])
            (root / "other.txt").write_text("not allowed", encoding="utf-8")
            with self.assertRaisesRegex(trust.TrustError, "completely clean"):
                trust.git_repository_identity(
                    root,
                    expected_repository="danielraffel/pulp-planning",
                    allowed_untracked=(root / "untracked.txt",),
                )

    def test_build_install_provenance_replays_exact_cli_bytes_and_rejects_tamper(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = pathlib.Path(raw).resolve()
            source = root / "source"
            source_files = {
                "CMakeLists.txt": "project(Pulp\n  VERSION 1.2.3\n  LANGUAGES CXX\n)\n",
                "docs/status/gpu-recipes.yaml": "catalog\n",
                "docs/status/gpu-recipes.schema.json": "{}\n",
                "docs/guides/gpu-validation-checklist.md": "guide\n",
                "docs/reference/cli.md": "reference\n",
            }
            revision = init_repo(
                source,
                source_files,
            )
            source_identity = trust.git_repository_identity(
                source, expected_repository="Generous-Corp/pulp"
            )
            build = root / "build"
            info = build / "core/runtime/generated/Release/pulp/runtime/build_info.hpp"
            info.parent.mkdir(parents=True)
            info.write_text(
                "#pragma once\n#include <string_view>\nnamespace pulp::runtime {\n"
                'inline constexpr std::string_view kBuildType = "Release";\n'
                'inline constexpr std::string_view kBuildIso8601 = "2026-08-29T00:00:00Z";\n'
                f'inline constexpr std::string_view kGitSha = "{revision[:7]}";\n'
                "inline constexpr bool kGitDirty = false;\n"
                'inline constexpr std::string_view kSdkVersion = "1.2.3";\n'
                'inline constexpr std::string_view kStampLabel = "fixture";\n}\n',
                encoding="utf-8",
            )
            (build / "CMakeCache.txt").write_text(
                f"CMAKE_HOME_DIRECTORY:INTERNAL={source}\n"
                "CMAKE_BUILD_TYPE:STRING=Release\n"
                f"CMAKE_COMMAND:INTERNAL={shutil.which('cmake')}\n",
                encoding="utf-8",
            )
            built = build / "bin/pulp-cpp"
            built.parent.mkdir(parents=True)
            built.write_bytes(b"exact-installed-cli")
            built.chmod(0o755)
            install_script = build / "tools/cli/cmake_install.cmake"
            install_script.parent.mkdir(parents=True)
            install_lines = [
                f'file(INSTALL DESTINATION "${{CMAKE_INSTALL_PREFIX}}/bin" TYPE PROGRAM '
                f'FILES "{built}")\n'
            ]
            for source_relative, installed_relative, _ in trust.PUBLIC_MATERIAL_MAPPINGS:
                install_lines.append(
                    f'file(INSTALL DESTINATION "${{CMAKE_INSTALL_PREFIX}}/'
                    f'{pathlib.PurePosixPath(installed_relative).parent}" TYPE FILE '
                    f'FILES "{source / source_relative}")\n'
                )
            install_script.write_text("".join(install_lines), encoding="utf-8")
            prefix = root / "installed"
            (prefix / "bin").mkdir(parents=True)
            for name in ("pulp", "pulp-cpp"):
                path = prefix / "bin" / name
                path.write_bytes(b"exact-installed-cli")
                path.chmod(0o755)
            build_identity, installed = trust.build_install_identity(
                source=source_identity, build_root=build, install_script=install_script,
                installed_prefix=prefix, installed_pulp=prefix / "bin/pulp", timeout=30.0,
            )
            self.assertEqual(installed["sha256"], trust.sha256_bytes(b"exact-installed-cli"))
            self.assertEqual(build_identity["build_info"]["kGitSha"], revision[:7])
            (prefix / "bin/pulp").write_bytes(b"tampered")
            with self.assertRaisesRegex(trust.TrustError, "exact copy"):
                trust.build_install_identity(
                    source=source_identity, build_root=build, install_script=install_script,
                    installed_prefix=prefix, installed_pulp=prefix / "bin/pulp", timeout=30.0,
                )

    def test_installed_public_catalog_and_docs_are_git_blob_bound(self) -> None:
        with tempfile.TemporaryDirectory() as raw:
            root = pathlib.Path(raw).resolve()
            source = root / "source"
            files = {
                "CMakeLists.txt": "project(Pulp VERSION 1.2.3)\n",
                "docs/status/gpu-recipes.yaml": "catalog\n",
                "docs/status/gpu-recipes.schema.json": "{}\n",
                "docs/guides/gpu-validation-checklist.md": "guide\n",
                "docs/reference/cli.md": "reference\n",
            }
            init_repo(source, files)
            prefix = root / "installed"
            mappings = {
                "docs/status/gpu-recipes.yaml": "share/pulp/gpu-recipes.yaml",
                "docs/status/gpu-recipes.schema.json": "share/pulp/gpu-recipes.schema.json",
                "docs/guides/gpu-validation-checklist.md": (
                    "share/pulp/docs/guides/gpu-validation-checklist.md"
                ),
                "docs/reference/cli.md": "share/pulp/docs/reference/cli.md",
            }
            for source_relative, installed_relative in mappings.items():
                destination = prefix / installed_relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source / source_relative, destination)
            records = trust.installed_public_material(source, prefix)
            self.assertEqual({record["role"] for record in records}, {
                "catalog", "catalog-schema", "guide", "reference",
            })
            (prefix / mappings["docs/reference/cli.md"]).write_text("tampered\n", encoding="utf-8")
            with self.assertRaisesRegex(trust.TrustError, "installed public"):
                trust.installed_public_material(source, prefix)

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
            with self.assertRaises(journey.JourneyError):
                journey._verified_artifacts(
                    {"artifacts": artifact_rows({"input.bin": b"KEEP"})}, artifacts, real
                )

    def test_stdout_stderr_workspace_timeout_and_bundle_caps_fail_closed(self) -> None:
        for descriptor, label in ((1, "stdout"), (2, "stderr")):
            with self.subTest(label=label), self.assertRaisesRegex(journey.JourneyError, label):
                journey._run(
                    [sys.executable, "-c", f"import os; os.write({descriptor}, b'x' * 131072)"],
                    5.0, stdout_limit=4096, stderr_limit=4096,
                )
        with tempfile.TemporaryDirectory() as raw:
            root = pathlib.Path(raw).resolve()
            started = time.monotonic()
            with self.assertRaisesRegex(journey.JourneyError, "production bound"):
                journey._run(
                    [sys.executable, "-c", (
                        "import time; p=open('noise.bin','wb',buffering=0); "
                        "[(p.write(b'x'*4096),time.sleep(.01)) for _ in range(1000)]"
                    )],
                    5.0, cwd=root,
                    monitor_roots=[journey.ResourceRoot(root, 8192, 8192, 8)],
                )
            self.assertLess(time.monotonic() - started, 2.0)
            marker = root / "escaped-descendant"
            child = (
                "import pathlib,time; time.sleep(.5); "
                f"pathlib.Path({str(marker)!r}).write_text('escaped')"
            )
            parent = (
                "import subprocess,sys,time; "
                f"subprocess.Popen([sys.executable,'-c',{child!r}]); time.sleep(60)"
            )
            with self.assertRaisesRegex(journey.JourneyError, "bound"):
                journey._run([sys.executable, "-c", parent], 0.1)
            time.sleep(0.7)
            self.assertFalse(marker.exists())
        with self.assertRaisesRegex(journey.JourneyError, "invalid bounded"):
            journey._run([sys.executable, "-c", "pass"], math.nan)
        with self.assertRaisesRegex(journey.JourneyError, "oversized string"):
            journey._bounded_redacted_bytes(
                {"payload": "x" * (journey.MAX_BUNDLE_STRING_BYTES + 1)}, [], "bundle"
            )

    def test_redacted_bundle_replaces_every_private_path(self) -> None:
        value = {"command": "/private/source/bin /private/case/file"}
        redacted, payload = journey._bounded_redacted_bytes(
            value,
            [("/private/source", "$SOURCE_ROOT"), ("/private/case", "$PRIVATE_CASE")],
            "bundle",
        )
        self.assertEqual(redacted["command"], "$SOURCE_ROOT/bin $PRIVATE_CASE/file")
        self.assertNotIn(b"/private/", payload)

    def test_checked_in_legacy_receipt_remains_nonterminal_and_unchanged_schema(self) -> None:
        path = pathlib.Path(__file__).resolve().parents[2] / (
            "docs/validation/gpu-clean-agent/m3-a5-clean-agent-20260828.json"
        )
        disposition = json.loads(path.read_text(encoding="utf-8"))
        journey._validate_superseded_disposition(disposition)
        self.assertEqual(disposition["schema"], journey.SUPERSEDED_SCHEMA)
        self.assertEqual(
            disposition["replacement_gate"]["required_schema"],
            "pulp.gpu-clean-agent-verification.v2",
        )
        self.assertIs(disposition["acceptance_gate_satisfied"], False)


if __name__ == "__main__":
    unittest.main()

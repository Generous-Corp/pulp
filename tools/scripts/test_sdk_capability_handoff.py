#!/usr/bin/env python3

from __future__ import annotations

import json
import stat
import tempfile
import unittest
from pathlib import Path

import sdk_capability_handoff as handoff


SOURCE_SHA = "a" * 40
PLATFORM = "linux-x64"
RUNTIME_PATHS = (
    handoff.IMPORTER_RUNTIME_ROOT / "capture.mjs",
    handoff.IMPORTER_RUNTIME_ROOT / "health.mjs",
)


class SdkCapabilityHandoffTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.prefix = Path(self.temporary.name)
        shared = self.prefix / "share/pulp"
        binary = self.prefix / "bin"
        shared.mkdir(parents=True)
        binary.mkdir()
        (binary / "pulp-import-design").write_bytes(b"installed importer\0bytes")
        for path in RUNTIME_PATHS:
            (self.prefix / path).parent.mkdir(parents=True, exist_ok=True)
            (self.prefix / path).write_bytes(f"fixture {path.name}".encode())
        self.capabilities = {
            "schema": "fixture.agent-capabilities.v1",
            "capabilities": [],
        }
        (shared / "agent-capabilities.json").write_text(
            json.dumps(self.capabilities, indent=2) + "\n", encoding="utf-8"
        )
        (shared / "agent-capabilities.schema.json").write_text(
            json.dumps(
                {
                    "type": "object",
                    "additionalProperties": False,
                    "required": ["schema", "capabilities"],
                    "properties": {
                        "schema": {"const": "fixture.agent-capabilities.v1"},
                        "capabilities": {"type": "array"},
                    },
                }
            ),
            encoding="utf-8",
        )
        source_schema = (
            Path(__file__).parents[2]
            / "docs/status/agent-capability-handoff.schema.json"
        )
        (shared / source_schema.name).write_bytes(source_schema.read_bytes())

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def stamp(self) -> dict[str, object]:
        document = handoff.build_handoff(
            self.prefix,
            sdk_source_sha=SOURCE_SHA,
            platform=PLATFORM,
            expected_importer_runtime_paths={path.as_posix() for path in RUNTIME_PATHS},
        )
        handoff.write_atomically(self.prefix / handoff.HANDOFF_PATH, document)
        return document

    def verify(self, **overrides: str) -> dict[str, object]:
        arguments = {
            "expected_sdk_source_sha": SOURCE_SHA,
            "expected_platform": PLATFORM,
            "expected_importer_runtime_paths": {
                path.as_posix() for path in RUNTIME_PATHS
            },
        }
        arguments.update(overrides)
        return handoff.verify_handoff(self.prefix, **arguments)

    def test_valid_identity_embeds_exact_installed_capabilities(self) -> None:
        document = self.stamp()
        self.assertEqual(self.verify(), document)
        self.assertEqual(
            document["agent_capabilities"]["content"], self.capabilities
        )
        self.assertEqual(
            stat.S_IMODE((self.prefix / handoff.HANDOFF_PATH).stat().st_mode),
            0o644,
        )

    def test_wrong_sdk_sha_is_rejected(self) -> None:
        self.stamp()
        with self.assertRaisesRegex(handoff.HandoffError, "sdk_source_sha"):
            self.verify(expected_sdk_source_sha="b" * 40)

    def test_changed_importer_bytes_are_rejected(self) -> None:
        self.stamp()
        (self.prefix / handoff.importer_path(PLATFORM)).write_bytes(b"different")
        with self.assertRaisesRegex(handoff.HandoffError, "importer sha256"):
            self.verify()

    def test_changed_importer_runtime_bytes_are_rejected(self) -> None:
        self.stamp()
        path = RUNTIME_PATHS[0]
        (self.prefix / path).write_bytes(b"different")
        with self.assertRaisesRegex(handoff.HandoffError, "runtime sha256"):
            self.verify()

    def test_large_bundled_node_runtime_is_hashable(self) -> None:
        node = handoff.IMPORTER_RUNTIME_ROOT / "node"
        (self.prefix / node).write_bytes(b"n" * (17 * 1024 * 1024))
        expected = {path.as_posix() for path in (*RUNTIME_PATHS, node)}
        document = handoff.build_handoff(
            self.prefix,
            sdk_source_sha=SOURCE_SHA,
            platform=PLATFORM,
            expected_importer_runtime_paths=expected,
        )
        self.assertEqual(
            {entry["path"] for entry in document["importer"]["runtime"]},
            expected,
        )

    def test_duplicate_importer_runtime_path_is_rejected(self) -> None:
        document = self.stamp()
        duplicate = dict(document["importer"]["runtime"][0])
        duplicate["sha256"] = "0" * 64
        document["importer"]["runtime"].insert(0, duplicate)
        handoff.write_atomically(self.prefix / handoff.HANDOFF_PATH, document)
        with self.assertRaisesRegex(handoff.HandoffError, "duplicated"):
            self.verify()

    def test_incomplete_importer_runtime_is_rejected_before_stamping(self) -> None:
        (self.prefix / RUNTIME_PATHS[0]).unlink()
        with self.assertRaisesRegex(handoff.HandoffError, "selected contract"):
            self.stamp()

    def test_changed_capability_bytes_are_rejected(self) -> None:
        self.stamp()
        (self.prefix / handoff.CAPABILITIES_PATH).write_text(
            json.dumps({**self.capabilities, "capabilities": ["changed"]}) + "\n",
            encoding="utf-8",
        )
        with self.assertRaisesRegex(handoff.HandoffError, "capability sha256"):
            self.verify()

    def test_unknown_handoff_field_fails_schema_validation(self) -> None:
        document = self.stamp()
        document["unexpected"] = True
        handoff.write_atomically(self.prefix / handoff.HANDOFF_PATH, document)
        with self.assertRaisesRegex(handoff.HandoffError, "violates its schema"):
            self.verify()

    def test_permissive_capability_schema_cannot_bless_changed_contract(self) -> None:
        document = self.stamp()
        changed = json.dumps({"unexpected": True}).encode()
        (self.prefix / handoff.CAPABILITIES_PATH).write_bytes(changed)
        (self.prefix / handoff.CAPABILITIES_SCHEMA_PATH).write_text(
            json.dumps({"type": "object"}), encoding="utf-8"
        )
        document["agent_capabilities"]["sha256"] = handoff.sha256(changed)
        document["agent_capabilities"]["content"] = json.loads(changed)
        handoff.write_atomically(self.prefix / handoff.HANDOFF_PATH, document)
        with self.assertRaisesRegex(handoff.HandoffError,
                                    "agent_capabilities schema sha256"):
            self.verify()

    def test_permissive_handoff_schema_cannot_bless_unknown_fields(self) -> None:
        document = self.stamp()
        document["unexpected"] = True
        (self.prefix / handoff.HANDOFF_SCHEMA_PATH).write_text(
            json.dumps({"type": "object"}), encoding="utf-8"
        )
        handoff.write_atomically(self.prefix / handoff.HANDOFF_PATH, document)
        with self.assertRaisesRegex(handoff.HandoffError,
                                    "handoff schema sha256"):
            self.verify()


class DevelopmentHandoffCliTests(unittest.TestCase):
    """Exercise CLI identity checks with real Git snapshots and installed bytes."""

    def setUp(self) -> None:
        SdkCapabilityHandoffTests.setUp(self)
        import subprocess
        self.source_temporary = tempfile.TemporaryDirectory()
        self.source = Path(self.source_temporary.name)
        subprocess.run(["git", "init", "-q", str(self.source)], check=True)
        subprocess.run(["git", "-C", str(self.source), "-c", "user.name=SDK fixture",
                        "-c", "user.email=sdk-fixture@example.invalid", "commit", "-q",
                        "--allow-empty", "-m", "Immutable source fixture"], check=True)
        self.source_sha = subprocess.check_output(
            ["git", "-C", str(self.source), "rev-parse", "HEAD"], text=True).strip()
        (self.prefix / "version.txt").write_text("1.0.0\n")
        (self.prefix / "sdk_build_type.txt").write_text("Release\n")
        from sdk_provenance import _importer_runtime_paths
        for name in _importer_runtime_paths(self.prefix, PLATFORM):
            path = self.prefix / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"fixture runtime")
        self.marker = {
            "schema": "pulp.sdk-provenance.v1", "kind": "development", "profile": "forge-dev",
            "distribution_eligible": False, "source_git_dirty": False, "build_type": "Release",
            "source_git_sha": self.source_sha, "platform": PLATFORM, "sdk_version": "1.0.0",
        }
        self.write_marker()

    def tearDown(self) -> None:
        self.source_temporary.cleanup()
        SdkCapabilityHandoffTests.tearDown(self)

    def write_marker(self) -> None:
        (self.prefix / "sdk-provenance.json").write_text(json.dumps(self.marker, indent=3) + "\n")

    def cli(self, command: str, *, source_sha: str | None = None) -> int:
        import contextlib
        import io
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return handoff.main([command, "--prefix", str(self.prefix), "--source-dir", str(self.source),
                                 "--source-sha", source_sha or self.source_sha, "--platform", PLATFORM])

    def test_local_cli_preserves_both_development_profiles(self) -> None:
        for profile in ("forge-dev", "trace"):
            self.marker["profile"] = profile
            self.write_marker()
            original = (self.prefix / "sdk-provenance.json").read_bytes()
            self.assertEqual(self.cli("stamp-dev"), 0)
            self.assertEqual(self.cli("verify-dev"), 0)
            self.assertEqual((self.prefix / "sdk-provenance.json").read_bytes(), original)
            document = json.loads((self.prefix / handoff.HANDOFF_PATH).read_text())
            self.assertNotIn("distribution_eligible", document)
            self.assertEqual(document["sdk_source_sha"], self.source_sha)

    def test_local_cli_rejects_missing_handoff_and_tampered_importer(self) -> None:
        self.assertEqual(self.cli("verify-dev"), 1)
        self.assertEqual(self.cli("stamp-dev"), 0)
        (self.prefix / handoff.importer_path(PLATFORM)).write_bytes(b"changed importer")
        self.assertEqual(self.cli("verify-dev"), 1)

    def test_local_cli_rejects_dirty_or_wrong_source(self) -> None:
        self.assertEqual(self.cli("stamp-dev", source_sha="f" * 40), 1)
        (self.source / "untracked.txt").write_text("uncommitted")
        self.assertEqual(self.cli("stamp-dev"), 1)
        self.assertFalse((self.prefix / handoff.HANDOFF_PATH).exists())

    def test_local_cli_refuses_official_or_mismatched_provenance(self) -> None:
        for key, value in (("kind", "official_release"), ("distribution_eligible", True),
                           ("platform", "linux-arm64"), ("source_git_sha", "f" * 40),
                           ("build_type", "Debug")):
            original = self.marker[key]
            self.marker[key] = value
            self.write_marker()
            self.assertEqual(self.cli("stamp-dev"), 1, key)
            self.marker[key] = original
        self.assertFalse((self.prefix / handoff.HANDOFF_PATH).exists())


if __name__ == "__main__":
    unittest.main()

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


if __name__ == "__main__":
    unittest.main()

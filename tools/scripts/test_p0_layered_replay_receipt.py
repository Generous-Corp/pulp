#!/usr/bin/env python3
"""Focused controls for the evidence-only P0 layered receipt builder."""

import json
import subprocess
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest import mock
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import p0_layered_replay_receipt as receipt


class LayeredReceiptTests(unittest.TestCase):
    def test_partition_control_records_canonical_pcm_for_both_partitions(self) -> None:
        layers = {layer.name: layer.value for layer in receipt.base_layers()}
        outputs = [receipt.output_for(partition, layers["event"], layers["plan"])
                   for partition in receipt.PARTITIONS]
        self.assertEqual(outputs[0]["pcm_sha256"], outputs[1]["pcm_sha256"])
        result = receipt.receipt(receipt.base_layers(), [], "test-head")
        self.assertEqual(result["partition_controls"]["status"], "NOT_CHECKED")
        self.assertEqual(result["partition_controls"]["partitions"], [64, 128])
        self.assertEqual(len(result["partition_controls"]["output_sha256"]), 2)

    def test_each_mutation_is_attributed_to_one_layer(self) -> None:
        result = receipt.receipt(receipt.base_layers(), [], "test-head")
        controls = result["planted_negative_controls"]["controls"]
        self.assertEqual([control["status"] for control in controls], ["NOT_CHECKED"] * 5)
        self.assertEqual([control["attribution"] for control in controls], ["PASS"] * 5)
        self.assertEqual([control["changed_layers"] for control in controls],
                         [[name] for name in ("authored", "resolved", "plan", "event", "output")])
        self.assertEqual([control["rejected_at_layer"] for control in controls],
                         [control["expected_failure_layer"] for control in controls])
        self.assertEqual([control["status"] for control in result["positive_controls"]],
                         ["NOT_CHECKED"] * 5)
        self.assertEqual(result["planted_negative_controls"]["status"], "NOT_CHECKED")

    def test_cli_keeps_missing_consumers_unqualified(self) -> None:
        script = Path(__file__).with_name("p0_layered_replay_receipt.py")
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "receipt.json"
            command = [sys.executable, str(script), "--output", str(output),
                       "--source-head", "test-head", "--allow-unavailable"]
            completed = subprocess.run(command, capture_output=True, text=True,
                                       encoding="utf-8", check=False)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            data = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual([item["name"] for item in data["consumers"]],
                             ["g3-installed-public-consumer", "spectral-installed-public-consumer"])
            self.assertEqual([item["status"] for item in data["consumers"]], ["UNAVAILABLE", "UNAVAILABLE"])
            self.assertEqual(data["consumer_gate"]["status"], "NOT_QUALIFIED")

    def test_consumer_requires_attested_identity_and_protocol(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "pulp-g3-resolved-identity-consumer"
            executable.write_text("#!/bin/sh\nprintf '%s\\n' pulp.graph.g3-resolved-identity-consumer.v1\n", encoding="utf-8")
            executable.chmod(0o755)
            record = receipt.run_consumer(
                "g3-installed-public-consumer", [str(executable)],
                "pulp-g3-resolved-identity-consumer",
                "pulp.graph.g3-resolved-identity-consumer.v1", "g3")
            self.assertEqual(record["status"], "UNAVAILABLE")
            bad = receipt.run_consumer(
                "g3-installed-public-consumer", [str(executable)],
                "wrong", "pulp.graph.g3-resolved-identity-consumer.v1", "g3")
            self.assertEqual(bad["status"], "UNAVAILABLE")

    def test_partitioned_consumer_requires_each_partition_attestation(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "pulp-g3-resolved-identity-consumer"
            executable.write_text(
                "#!/bin/sh\n"
                "printf '%s partition_frames=%s\\n' "
                "pulp.graph.g3-resolved-identity-consumer.v1 \"$2\"\n",
                encoding="utf-8")
            executable.chmod(0o755)
            record = receipt.run_consumer(
                "g3-installed-public-consumer", [str(executable)],
                "pulp-g3-resolved-identity-consumer",
                "pulp.graph.g3-resolved-identity-consumer.v1", "g3", receipt.PARTITIONS)
            self.assertEqual(record["status"], "UNAVAILABLE")
            self.assertEqual([run["status"] for run in record["runs"]], ["UNAVAILABLE", "UNAVAILABLE"])

    def test_consumer_protocol_requires_an_exact_token(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "pulp-g3-resolved-identity-consumer"
            executable.write_bytes(b"\x7fELF" + b"native-test")
            executable.chmod(0o755)
            fake = SimpleNamespace(returncode=0, stdout=b"prefixpulp.graph.g3-resolved-identity-consumer.v1suffix", stderr=b"")
            with mock.patch.object(receipt.subprocess, "run", return_value=fake):
                record = receipt.run_consumer(
                    "g3-installed-public-consumer", [str(executable)],
                    "pulp-g3-resolved-identity-consumer",
                    "pulp.graph.g3-resolved-identity-consumer.v1", "g3")
            self.assertEqual(record["status"], "UNAVAILABLE")
            self.assertIn("exact", record["reason"])

    def test_receipt_keeps_layer_bytes_and_quality_metadata_separate(self) -> None:
        result = receipt.receipt(receipt.base_layers(), [], "test-head")
        self.assertEqual([layer["name"] for layer in result["layers"]],
                         ["authored", "resolved", "plan", "event", "output"])
        for layer in result["layers"]:
            self.assertTrue(layer["canonical_bytes_sha256"].startswith("sha256:"))
            self.assertGreater(layer["canonical_bytes_length"], 0)
        output = next(layer["value"] for layer in result["layers"] if layer["name"] == "output")
        self.assertEqual(output["partitions"][0]["canonical_pcm"]["encoding"], "s32le")
        self.assertIn("quality", output["partitions"][0])
        self.assertIsNone(result["source_head"])
        self.assertEqual(result["provenance"]["source_head_status"], "UNBOUND")

    def test_public_consumer_fixtures_emit_protocol_attestations(self) -> None:
        fixtures = {
            Path(__file__).parents[2] / "examples/g3-resolved-identity-consumer/main.cpp":
                "pulp.graph.g3-resolved-identity-consumer.v1",
            Path(__file__).parents[2] / "examples/spectral-frame-codec-consumer/main.cpp":
                "pulp.signal.spectral-frame-codec-consumer.v1",
        }
        for path, protocol in fixtures.items():
            source = path.read_text(encoding="utf-8")
            self.assertIn(protocol, source)
            self.assertIn("kP0ProtocolAttestation", source)


if __name__ == "__main__":
    unittest.main()

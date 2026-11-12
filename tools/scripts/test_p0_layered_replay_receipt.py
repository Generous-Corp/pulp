#!/usr/bin/env python3
"""Focused controls for the evidence-only P0 layered receipt builder."""

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import p0_layered_replay_receipt as receipt


class LayeredReceiptTests(unittest.TestCase):
    def test_partition_render_is_invariant(self) -> None:
        layers = {layer.name: layer.value for layer in receipt.base_layers()}
        outputs = [receipt.output_for(partition, layers["event"], layers["plan"])
                   for partition in receipt.PARTITIONS]
        self.assertEqual(outputs[0]["pcm_sha256"], outputs[1]["pcm_sha256"])

    def test_each_mutation_is_attributed_to_one_layer(self) -> None:
        result = receipt.receipt(receipt.base_layers(), [], "test-head")
        controls = result["planted_negative_controls"]["controls"]
        self.assertEqual([control["status"] for control in controls], ["PASS"] * 5)
        self.assertEqual([control["changed_layers"] for control in controls],
                         [[name] for name in ("authored", "resolved", "plan", "event", "output")])

    def test_cli_records_independent_consumers(self) -> None:
        script = Path(__file__).with_name("p0_layered_replay_receipt.py")
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "receipt.json"
            command = [sys.executable, str(script), "--output", str(output),
                       "--source-head", "test-head", "--g3-command", sys.executable,
                       "--spectral-command", sys.executable]
            completed = subprocess.run(command, capture_output=True, text=True, check=False)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            data = json.loads(output.read_text())
            self.assertEqual([item["name"] for item in data["consumers"]],
                             ["g3-installed-public-consumer", "spectral-installed-public-consumer"])
            self.assertEqual([item["status"] for item in data["consumers"]], ["PASS", "PASS"])


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
import importlib.util
import json
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXAMPLE_ROOT = ROOT / "examples" / "capability-control"


def load_generator():
    spec = importlib.util.spec_from_file_location(
        "control_example_generator", EXAMPLE_ROOT / "generate_examples.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader
    spec.loader.exec_module(module)
    return module


class CapabilityControlAuthoringExamples(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.generator = load_generator()
        cls.examples = cls.generator.load_examples()

    def test_generated_outputs_are_fresh(self):
        for path, expected in self.generator.outputs(self.examples).items():
            self.assertTrue(path.is_file(), path)
            self.assertEqual(path.read_text(), expected, path)

    def test_cli_examples_use_only_installed_exact_instance_surface(self):
        forbidden = ("./build", "tools/", "planning/", "--host", "--port",
                     "--discovery", "--command")
        for example in self.examples:
            command = example["cli"]
            self.assertEqual(command[:2], ["pulp", "control"], example["id"])
            text = " ".join(command)
            self.assertFalse(any(token in text for token in forbidden), text)
            if example["tier"] in ("T0", "T1"):
                self.assertIn("--instance", command, example["id"])

    def test_t0_and_t1_have_matching_generated_mcp_tools(self):
        operations = {
            "t0-offline-render": "dev.pulp.render/offline@1",
            "t1-state-read": "dev.pulp.state/read@1",
            "t1-parameter-gesture": "dev.pulp.state/parameter-gesture@1",
        }
        by_id = {item["id"]: item for item in self.examples}
        for example_id, operation in operations.items():
            suffix = re.sub(r"[^a-z0-9]+", "_", operation.removeprefix("dev.pulp.")
                            .removesuffix("@1")).strip("_")
            self.assertIn(operation, by_id[example_id]["cli"])
            self.assertEqual(by_id[example_id]["mcp"]["tool"],
                             "pulp_control_" + suffix)

    def test_mcp_jsonl_round_trips(self):
        lines = (EXAMPLE_ROOT / "generated" / "mcp-tools.jsonl").read_text().splitlines()
        decoded = [json.loads(line) for line in lines]
        self.assertEqual([item["id"] for item in decoded],
                         [item["id"] for item in self.examples])
        for item in decoded:
            self.assertTrue(item["tool"].startswith("pulp_control_"))
            self.assertIsInstance(item["arguments"], dict)


if __name__ == "__main__":
    unittest.main()

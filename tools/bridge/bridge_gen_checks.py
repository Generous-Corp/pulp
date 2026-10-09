#!/usr/bin/env python3
"""Focused positive and planted-negative checks for the bridge generator."""

# CTest input tracking: this selftest exercises the checked-in generator and
# contract outputs as well as its own source.
# "tools/bridge/bridge_gen.py"
# "tools/bridge/bridge.toml"
# "tools/bridge/generated_editor_bridge.hpp"
# "tools/bridge/generated_editor_bridge.ts"
# "docs/reference/generated-editor-bridge-contract.md"

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = HERE / "bridge_gen.py"
spec = importlib.util.spec_from_file_location("bridge_gen", SCRIPT)
assert spec is not None and spec.loader is not None
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)


class BridgeGeneratorChecks(unittest.TestCase):
    def test_checked_in_outputs_match(self) -> None:
        self.assertTrue(generator.check(generator.render(generator.load_contract())))

    def test_output_is_deterministic_and_canonical(self) -> None:
        first = generator.render(generator.load_contract())
        second = generator.render(generator.load_contract())
        self.assertEqual(first, second)
        data = generator.load_contract()
        self.assertEqual([row["name"] for row in data["commands"]], ["begin_gesture", "end_gesture", "set_parameter"])
        self.assertIn("accepted: boolean", generator.render_ts(data))
        self.assertIn("JSON.stringify(request)", generator.render_ts(data))
        self.assertIn("export function jsonTransport", generator.render_ts(data))
        self.assertLess(generator.render_cpp(data).find('"begin_gesture"'), generator.render_cpp(data).find('"parameter_changed"'))

    def test_schema_rejects_duplicate_fields(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bad.toml"
            path.write_text(
                'version = 1\nname = "editor"\n[[commands]]\nname = "x"\n'
                'request = [{name="a", type="string"}, {name="a", type="string"}]\n',
                encoding="utf-8",
            )
            with self.assertRaises(ValueError):
                generator.load_contract(path)

    def test_schema_rejects_drifted_canonical_set_parameter(self) -> None:
        malformed_contracts = (
            (
                'request = [{name="key", type="string"}]\n'
                'response = [{name="accepted", type="boolean"}]\n'
            ),
            (
                'request = [{name="key", type="string"}, {name="value", type="number"}]\n'
                'response = [{name="accepted", type="string"}]\n'
            ),
        )
        for index, fields in enumerate(malformed_contracts):
            with self.subTest(index=index), tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "bad.toml"
                path.write_text(
                    'version = 1\nname = "editor"\n[[commands]]\nname = "set_parameter"\n'
                    + fields,
                    encoding="utf-8",
                )
                with self.assertRaisesRegex(ValueError, "canonical request"):
                    generator.load_contract(path)

    def test_renderer_rejects_drifted_canonical_set_parameter(self) -> None:
        data = {
            "version": 1,
            "name": "editor",
            "commands": [
                {
                    "name": "set_parameter",
                    "request": [{"name": "key", "type": "string"}],
                    "response": [{"name": "accepted", "type": "boolean"}],
                }
            ],
            "publications": [],
        }
        with self.assertRaisesRegex(ValueError, "canonical request"):
            generator.render(data)

    def test_docs_state_when_canonical_helper_is_absent(self) -> None:
        data = {
            "version": 1,
            "name": "editor",
            "commands": [
                {
                    "name": "set_value",
                    "request": [{"name": "key", "type": "string"}],
                    "response": [],
                }
            ],
            "publications": [],
        }
        docs = generator.render_docs(data)
        self.assertIn("No canonical `set_parameter` helper is emitted", docs)
        self.assertNotIn("The canonical `set_parameter` command also emits", docs)

    def test_planted_drift_negative_control(self) -> None:
        outputs = generator.render(generator.load_contract())
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "generated.hpp"
            expected = outputs[generator.OUTPUTS["cpp"]]
            path.write_text(expected.replace("set_parameter", "set_paramter", 1), encoding="utf-8")
            self.assertFalse(generator.check({path: expected}))

if __name__ == "__main__":
    unittest.main()

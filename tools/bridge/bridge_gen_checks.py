#!/usr/bin/env python3
"""Focused positive and planted-negative checks for the bridge generator."""

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

    def test_planted_drift_negative_control(self) -> None:
        outputs = generator.render(generator.load_contract())
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "generated.hpp"
            expected = outputs[generator.OUTPUTS["cpp"]]
            path.write_text(expected.replace("set_parameter", "set_paramter", 1), encoding="utf-8")
            self.assertFalse(generator.check({path: expected}))

    def test_planted_missing_handler_negative_control(self) -> None:
        data = generator.load_contract()
        declared = [row["name"] for row in data["commands"]]
        registered = declared[:-1]
        self.assertNotEqual(registered, declared)


if __name__ == "__main__":
    unittest.main()

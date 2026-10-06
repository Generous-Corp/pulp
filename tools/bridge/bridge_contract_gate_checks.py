#!/usr/bin/env python3
"""Production-path checks for the authoritative bridge contract gate."""

# CTest input tracking: this test exercises the production gate and its
# generator/safety dependencies, including the checked-in contract source.
# "tools/bridge/bridge_contract_check.py"
# "tools/bridge/bridge_contract_safety.py"
# "tools/bridge/bridge_gen.py"
# "tools/bridge/bridge.toml"
# "tools/scripts/script_test_inputs.py"

from __future__ import annotations

import importlib.util
import io
import sys
import tempfile
import unittest
from pathlib import Path


HERE = Path(__file__).resolve().parent
SCRIPT = HERE / "bridge_contract_check.py"
spec = importlib.util.spec_from_file_location("bridge_contract_check", SCRIPT)
assert spec is not None and spec.loader is not None
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


def write_contract(directory: Path, name: str, text: str) -> tuple[Path, dict[str, Path]]:
    source = directory / f"{name}.toml"
    source.write_text(text, encoding="utf-8")
    outputs = {key: directory / f"{name}.{suffix}" for key, suffix in {
        "cpp": "hpp", "ts": "ts", "docs": "md"
    }.items()}
    return source, outputs


class BridgeContractGateChecks(unittest.TestCase):
    def test_affected_test_selector_tracks_contract_inputs(self) -> None:
        """Keep dynamic imports visible to the affected-test graph."""

        repo = HERE.parents[1]
        tracker_path = repo / "tools" / "scripts" / "script_test_inputs.py"
        tracker_spec = importlib.util.spec_from_file_location(
            "script_test_inputs", tracker_path
        )
        assert tracker_spec is not None and tracker_spec.loader is not None
        tracker = importlib.util.module_from_spec(tracker_spec)
        tracker_spec.loader.exec_module(tracker)
        record = tracker.inputs_for(
            {
                "command": [
                    sys.executable,
                    str(Path(__file__)),
                    "--docs",
                    str(repo / "docs/reference/generated-editor-bridge-contract.md"),
                ],
                "properties": [],
            },
            repo,
        )
        assert record is not None
        expected = {
            "tools/bridge/bridge.toml",
            "tools/bridge/bridge_contract_check.py",
            "tools/bridge/bridge_contract_gate_checks.py",
            "tools/bridge/bridge_contract_safety.py",
            "tools/bridge/bridge_gen.py",
            "tools/bridge/generated_editor_bridge.hpp",
            "tools/bridge/generated_editor_bridge.ts",
            "docs/reference/generated-editor-bridge-contract.md",
        }
        self.assertTrue(expected.issubset(record["inputs"]))

    def test_production_path_writes_then_checks_positive_fixture(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, outputs = write_contract(
                root,
                "valid",
                'version = 1\nname = "editor"\n[[commands]]\nname = "set_value"\n'
                'request = [{name="key", type="string"}]\n',
            )
            self.assertEqual(gate.run(source, outputs, write=True, stream=io.StringIO()), 0)
            self.assertEqual(gate.run(source, outputs, stream=io.StringIO()), 0)

    def test_production_path_rejects_reserved_identifier(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, outputs = write_contract(
                root,
                "reserved",
                'version = 1\nname = "editor"\n[[commands]]\nname = "set_value"\n'
                'request = [{name="class", type="string"}]\n',
            )
            stream = io.StringIO()
            self.assertEqual(gate.run(source, outputs, stream=stream), 1)
            self.assertIn("reserved in C++", stream.getvalue())
            self.assertFalse(any(path.exists() for path in outputs.values()))

    def test_production_path_rejects_reserved_typescript_parameter(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, outputs = write_contract(
                root,
                "reserved-ts",
                'version = 1\nname = "editor"\n[[commands]]\nname = "set_value"\n'
                'request = [{name="interface", type="string"}]\n',
            )
            stream = io.StringIO()
            self.assertEqual(gate.run(source, outputs, stream=stream), 1)
            self.assertIn("reserved in TypeScript parameter position", stream.getvalue())
            self.assertFalse(any(path.exists() for path in outputs.values()))

    def test_production_path_rejects_strict_wrapper_bindings(self) -> None:
        for command_name in ("eval", "arguments"):
            with tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                source, outputs = write_contract(
                    root,
                    f"reserved-wrapper-{command_name}",
                    'version = 1\nname = "editor"\n[[commands]]\n'
                    f'name = "{command_name}"\n',
                )
                stream = io.StringIO()
                self.assertEqual(gate.run(source, outputs, stream=stream), 1)
                self.assertIn(
                    f"wrapper '{command_name}' is reserved in TypeScript",
                    stream.getvalue(),
                )
                self.assertFalse(any(path.exists() for path in outputs.values()))

    def test_production_path_rejects_transport_parameter_collision(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, outputs = write_contract(
                root,
                "transport-collision",
                'version = 1\nname = "editor"\n[[commands]]\nname = "set_value"\n'
                'request = [{name="transport", type="string"}]\n',
            )
            stream = io.StringIO()
            self.assertEqual(gate.run(source, outputs, stream=stream), 1)
            self.assertIn("collides with the generated TypeScript transport parameter", stream.getvalue())
            self.assertFalse(any(path.exists() for path in outputs.values()))

    def test_production_path_rejects_transformed_collision(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, outputs = write_contract(
                root,
                "collision",
                'version = 1\nname = "editor"\n[[commands]]\nname = "foo_bar"\n'
                '[[commands]]\nname = "fooBar"\n',
            )
            stream = io.StringIO()
            self.assertEqual(gate.run(source, outputs, stream=stream), 1)
            self.assertIn("type collision", stream.getvalue())
            self.assertFalse(any(path.exists() for path in outputs.values()))


if __name__ == "__main__":
    unittest.main()

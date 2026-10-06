#!/usr/bin/env python3
"""Production-path checks for the authoritative bridge contract gate."""

from __future__ import annotations

import importlib.util
import io
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

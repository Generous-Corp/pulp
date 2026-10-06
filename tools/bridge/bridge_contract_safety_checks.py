#!/usr/bin/env python3
"""Positive and planted-negative checks for bridge contract safety auditing."""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path


HERE = Path(__file__).resolve().parent
SCRIPT = HERE / "bridge_contract_safety.py"
spec = importlib.util.spec_from_file_location("bridge_contract_safety", SCRIPT)
assert spec is not None and spec.loader is not None
auditor = importlib.util.module_from_spec(spec)
spec.loader.exec_module(auditor)


def load_contract(text: str) -> dict:
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "contract.toml"
        path.write_text(text, encoding="utf-8")
        return auditor.generator.load_contract(path)


class BridgeContractSafetyChecks(unittest.TestCase):
    def test_seed_contract_is_safe(self) -> None:
        data = auditor.generator.load_contract()
        self.assertEqual(auditor.audit(data), [])

    def test_reserved_cpp_field_is_rejected(self) -> None:
        data = load_contract(
            'version = 1\nname = "editor"\n[[commands]]\nname = "set_value"\n'
            'request = [{name="class", type="string"}]\n'
        )
        self.assertIn("field 'class' is reserved in C++", "\n".join(auditor.audit(data)))

    def test_transformed_names_must_remain_unique(self) -> None:
        data = load_contract(
            'version = 1\nname = "editor"\n'
            '[[commands]]\nname = "foo_bar"\n'
            '[[commands]]\nname = "fooBar"\n'
        )
        problems = "\n".join(auditor.audit(data))
        self.assertIn("generated C++ type collision: FooBarRequest", problems)
        self.assertIn("generated TypeScript wrapper collision: fooBar", problems)

    def test_reserved_typescript_wrapper_is_rejected(self) -> None:
        data = load_contract(
            'version = 1\nname = "editor"\n[[commands]]\nname = "class"\n'
        )
        self.assertIn("wrapper 'class' is reserved in TypeScript", "\n".join(auditor.audit(data)))

    def test_empty_transformed_names_are_rejected(self) -> None:
        data = load_contract(
            'version = 1\nname = "editor"\n[[commands]]\nname = "_"\n'
        )
        problems = "\n".join(auditor.audit(data))
        self.assertIn("produces an empty generated type name", problems)
        self.assertIn("produces an empty TypeScript wrapper name", problems)

    def test_generated_interface_cannot_shadow_transport_alias(self) -> None:
        data = load_contract(
            'version = 1\nname = "editor"\n[[commands]]\nname = "editor_bridge"\n'
        )
        self.assertIn(
            "generated TypeScript interface collides with alias: EditorBridgeRequest",
            "\n".join(auditor.audit(data)),
        )


if __name__ == "__main__":
    unittest.main()

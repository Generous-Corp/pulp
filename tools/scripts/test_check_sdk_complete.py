#!/usr/bin/env python3

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

import check_sdk_complete as complete


class CheckSdkCompleteTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.prefix = self.root / "sdk"
        self.source = self.root / "source"
        runtime = self.prefix / "bin/browser_capture-v1"
        source_runtime = self.source / "tools/import-design/browser_capture"
        runtime.mkdir(parents=True)
        source_runtime.mkdir(parents=True)
        (self.prefix / "bin/pulp-import-design").write_text("importer", encoding="utf-8")
        (source_runtime / "capture.mjs").write_text("capture", encoding="utf-8")
        (source_runtime / "interaction_plan_protocol.json").write_text(
            '{"version":1}\n', encoding="utf-8"
        )
        (runtime / "interaction_plan_protocol.json").write_text(
            '{"version":1}\n', encoding="utf-8"
        )
        (source_runtime / "runtime_manifest.txt").write_text(
            "capture.mjs\ninteraction_plan_protocol.json\n", encoding="utf-8"
        )
        (runtime / "capture.mjs").write_text("capture", encoding="utf-8")
        (source_runtime / "interaction_plan_protocol.json").write_text("protocol", encoding="utf-8")
        (runtime / "interaction_plan_protocol.json").write_text("protocol", encoding="utf-8")
        (source_runtime / "runtime_manifest.txt").write_text(
            "capture.mjs\ninteraction_plan_protocol.json\n", encoding="utf-8")
        (runtime / "runtime_manifest.txt").write_text(
            "capture.mjs\ninteraction_plan_protocol.json\n", encoding="utf-8")
        source_contract = self.source / "tools/import-design/jsx-runtime/materialized_binding_contract.mjs"
        source_contract.parent.mkdir(parents=True)
        source_contract.write_text("contract", encoding="utf-8")
        installed_contract = self.prefix / "bin/jsx-runtime/materialized_binding_contract.mjs"
        installed_contract.parent.mkdir(parents=True)
        installed_contract.write_text("contract", encoding="utf-8")
        (runtime / "node").write_text("node", encoding="utf-8")
        (runtime / "node.LICENSE").write_text("license", encoding="utf-8")
        matrix = self.source / "tools/scripts/release_product_matrix.json"
        matrix.parent.mkdir(parents=True)
        matrix.write_text(
            json.dumps(
                {
                    "node_runtime_floor": "0.813.1",
                    "materialized_binding_contract_floor": "0.918.0",
                }
            ), encoding="utf-8"
        )

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def test_node_is_optional_before_floor(self) -> None:
        (self.prefix / "version.txt").write_text("0.813.0\n", encoding="utf-8")
        self.assertEqual(complete.check(self.prefix, self.source), [])

    def test_node_is_required_at_floor(self) -> None:
        (self.prefix / "version.txt").write_text("0.813.1\n", encoding="utf-8")
        (self.prefix / "bin/browser_capture-v1/node").unlink()
        (self.prefix / "bin/browser_capture-v1/node.LICENSE").unlink()
        problems = complete.check(self.prefix, self.source)
        self.assertTrue(any("Node runtime" in problem for problem in problems))
        self.assertTrue(any("Node license" in problem for problem in problems))

    def test_materialized_contract_is_required_at_floor(self) -> None:
        (self.prefix / "version.txt").write_text("0.918.0\n", encoding="utf-8")
        (self.prefix / "bin/jsx-runtime/materialized_binding_contract.mjs").unlink()
        problems = complete.check(self.prefix, self.source)
        self.assertTrue(any("materialized binding contract" in problem for problem in problems))

    def test_manifested_json_runtime_asset_stale_bytes_are_rejected(self) -> None:
        (self.prefix / "version.txt").write_text("0.918.0\n", encoding="utf-8")
        (self.prefix / "bin/browser_capture-v1/interaction_plan_protocol.json").write_text(
            "stale", encoding="utf-8"
        )
        problems = complete.check(self.prefix, self.source)
        self.assertTrue(any("interaction_plan_protocol.json" in problem for problem in problems))

    def test_materialized_contract_stale_bytes_are_rejected(self) -> None:
        (self.prefix / "version.txt").write_text("0.918.0\n", encoding="utf-8")
        (self.prefix / "bin/jsx-runtime/materialized_binding_contract.mjs").write_text(
            "stale", encoding="utf-8"
        )
        problems = complete.check(self.prefix, self.source)
        self.assertTrue(any("contract is STALE" in problem for problem in problems))

    def test_manifest_json_stale_bytes_are_rejected(self) -> None:
        (self.prefix / "version.txt").write_text("0.918.0\n", encoding="utf-8")
        (self.prefix / "bin/browser_capture-v1/interaction_plan_protocol.json").write_text(
            '{"version":0}\n', encoding="utf-8"
        )
        problems = complete.check(self.prefix, self.source)
        self.assertTrue(
            any("interaction_plan_protocol.json" in problem for problem in problems)
        )

    def test_historical_matrix_without_floor_keeps_node_optional(self) -> None:
        (self.prefix / "version.txt").write_text("0.790.1\n", encoding="utf-8")
        matrix = self.source / "tools/scripts/release_product_matrix.json"
        matrix.write_text("{}", encoding="utf-8")
        self.assertEqual(complete.check(self.prefix, self.source), [])


if __name__ == "__main__":
    unittest.main()

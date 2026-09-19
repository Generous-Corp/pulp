#!/usr/bin/env python3
"""Exact sample-region catalog routes and additive schema negative controls."""
from __future__ import annotations

import copy
import json
import pathlib
import unittest

import dsp_capability_registry as dsp
import json_schema_lite

ROOT = pathlib.Path(__file__).resolve().parents[2]


def route_problems(dsp_document: dict, forge_document: dict) -> list[str]:
    kernels = [row for catalog in dsp_document["catalogs"] for row in catalog["nodes"]
               if "sample_region_v1" in row]
    routes = [row for node in forge_document["nodes"] for row in node["realizations"]
              if "sample_region_v1" in row]
    problems = []
    for kernel in kernels:
        identity = (kernel["key"], kernel["type_version"], kernel["sample_kernel_version"])
        matched = [row for row in routes if (row["sample_region_v1"]["dsp_key"],
                   row["type_version"], row["sample_kernel_version"]) == identity]
        if len(matched) != 1:
            problems.append(f"DSP realization must have exactly one Forge route: {identity}")
    for route in routes:
        meta = route["sample_region_v1"]
        matched = [row for row in kernels if (row["key"], row["type_version"],
                   row["sample_kernel_version"]) == (meta["dsp_key"], route["type_version"],
                                                       route["sample_kernel_version"])]
        if len(matched) != 1:
            problems.append(f"Forge route must resolve exactly one DSP realization: {meta}")
            continue
        kernel = matched[0]
        if route["type_id"] != kernel["type_id"] or meta["builder_role"] != kernel["sample_region_v1"]["role"]:
            problems.append("Forge role/type identity differs from DSP realization")
        if meta["type_version"] != kernel["type_version"] or meta["sample_kernel_version"] != kernel["sample_kernel_version"]:
            problems.append("Forge nested exact versions differ from DSP realization")
    return problems


class SampleRegionCatalogTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dsp = dsp.extract(ROOT)
        cls.forge = json.loads((ROOT / "docs/status/forge-catalog.json").read_text())
        cls.schemas = {name: json.loads((ROOT / f"docs/status/{name}.schema.json").read_text())
                       for name in ("dsp-capabilities", "forge-catalog")}

    def test_exact_bidirectional_routes(self):
        self.assertEqual(route_problems(self.dsp, self.forge), [])
        self.assertEqual(len(dsp.sample_region_rows(ROOT)), 7)

    def test_missing_duplicate_and_wrong_version_routes_refuse(self):
        source = copy.deepcopy(self.forge)
        region = next(node for node in source["nodes"] if "sample_region_v1" in node["realizations"][0])
        region["realizations"].append(copy.deepcopy(region["realizations"][0]))
        self.assertTrue(route_problems(self.dsp, source))
        region["realizations"].clear()
        self.assertTrue(route_problems(self.dsp, source))
        source = copy.deepcopy(self.forge)
        region = next(node for node in source["nodes"] if "sample_region_v1" in node["realizations"][0])
        region["realizations"][0]["sample_kernel_version"] = 2
        self.assertTrue(route_problems(self.dsp, source))

    def test_closed_metadata_and_legacy_readers(self):
        for name, document in (("dsp-capabilities", self.dsp), ("forge-catalog", self.forge)):
            schema = self.schemas[name]
            self.assertEqual(json_schema_lite.validate(document, schema), [])
            metadata = schema["$defs"]["sample_region_v1"]
            if name == "dsp-capabilities":
                row = copy.deepcopy(dsp.sample_region_rows(ROOT)[0]["sample_region_v1"])
            else:
                row = copy.deepcopy(next(node["realizations"][0]["sample_region_v1"]
                                    for node in self.forge["nodes"]
                                    if "sample_region_v1" in node["realizations"][0]))
            row["unknown_field"] = True
            self.assertTrue(json_schema_lite.validate(row, metadata))
            self.assertEqual(json_schema_lite.validate({"type_id": "legacy.node", "mode": "default"},
                             {"$defs": schema["$defs"], "$ref": "#/$defs/realization"}), [])

    def test_boundaries_cannot_be_palette_nodes(self):
        schema = self.schemas["forge-catalog"]["$defs"]["sample_region_v1"]
        for node in self.forge["nodes"]:
            for row in node["realizations"]:
                if "sample_region_v1" not in row:
                    continue
                meta = copy.deepcopy(row["sample_region_v1"])
                meta["placement"] = "normal_node" if meta["placement"] == "region_builder_only" else "region_builder_only"
                self.assertTrue(json_schema_lite.validate(meta, schema))


if __name__ == "__main__":
    unittest.main()

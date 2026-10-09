#!/usr/bin/env python3
"""Regression coverage for the out-of-line Forge dynamics capability map."""

from __future__ import annotations

import importlib.util
import json
import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/scripts/dsp_capability_registry.py"
IMPLEMENTATION = ROOT / "core/host/src/forge_dynamics_catalog.cpp"
DESCRIPTOR = ROOT / "core/host/include/pulp/host/detail/forge_dynamics_catalog_descriptor.hpp"
assert IMPLEMENTATION.is_file()
assert DESCRIPTOR.is_file()
spec = importlib.util.spec_from_file_location("dsp_capability_registry", SCRIPT)
assert spec and spec.loader
registry = importlib.util.module_from_spec(spec)
spec.loader.exec_module(registry)


projection = registry.extract(ROOT)
assert len(projection["catalogs"]) == 30
assert sum(len(c["type_ids"]) for c in projection["catalogs"]) == 115
assert sum(len(n["baked_params"]) for c in projection["catalogs"] for n in c["nodes"]) == 400

dynamics = next(
    c for c in projection["catalogs"]
    if c["header"] == "core/host/include/pulp/host/forge_dynamics_catalog.hpp"
)
expected = {
    "make_feedforward_compressor_node": [
        "kThresholdDb", "kRatio", "kKneeDb", "kAttackMs", "kReleaseMs",
        "kDetectorMode", "kRmsWindowMs", "kProgramDependent", "kMakeupDb",
        "kAutoMakeup", "kStereoLink",
    ],
    "make_node": ["kCeilingDbtp", "kReleaseMs"],
    "make_vca_compressor_node": 9,
    "make_fet_compressor_node": 8,
    "make_diode_bridge_compressor_node": 10,
}
actual = {
    node["factory"]: [param["id"].split("::")[-1] for param in node["baked_params"]]
    for node in dynamics["nodes"]
}
assert set(actual) == set(expected)
for factory, requirement in expected.items():
    assert actual[factory] == requirement if isinstance(requirement, list) else len(actual[factory]) == requirement
assert sum(map(len, actual.values())) == 40
assert not any(c["header"].endswith("forge_dynamics_catalog_descriptor.hpp") for c in projection["catalogs"])

snapshot = json.loads((ROOT / "docs/status/dsp-capabilities.json").read_text())
assert projection == snapshot, "generated projection differs from committed snapshot"
print("dsp capability registry dynamics regression: passed (5 factories, 40 params)")

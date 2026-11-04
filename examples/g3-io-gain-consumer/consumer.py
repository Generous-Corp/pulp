"""Run after installing tools/graph in a clean environment."""

import copy
import json
from pathlib import Path

from pulp_graph_identity import IdentityError, diff, fingerprint, parse_document

fixtures = Path(__file__).resolve().parent / "fixtures"
accepted = parse_document((fixtures / "accepted-omitted-defaults.json").read_bytes())
golden = (fixtures / "g3-io-gain-goldens.jsonl").read_bytes().splitlines()[1]
result = fingerprint(accepted)
assert result["payload"] == golden
assert result["digest"] == "sha256:2222eca305c4af8fe076923e5bb00b0cf0c9db9065a45f8cb78c5f64e6bb4e25"
empty = fingerprint(parse_document((fixtures / "empty-authored.json").read_bytes()))
assert empty["payload"] == (fixtures / "g3-io-gain-goldens.jsonl").read_bytes().splitlines()[0]
assert empty["digest"] == "sha256:f6c20f9b5be41fd7ac76a5b08747f192a6a3c673daf5e5bf93409cfc84c78bb6"
assert diff(accepted, accepted)["equal"] is True
changed_gain = copy.deepcopy(accepted)
changed_gain["nodes"]["gain"]["config"]["gain"] = "f32:3f800001"
assert diff(accepted, changed_gain)["equal"] is False
swapped = copy.deepcopy(accepted)
for node in swapped["nodes"].values():
    for direction in ("in", "out"):
        if "audio" in node["ports"][direction]:
            node["ports"][direction]["audio"]["channels"] = ["R", "L"]
for interface in swapped["interfaces"]:
    interface["channels"] = ["R", "L"]
for edge in swapped["edges"].values():
    edge["channel_map"] = [["R", "R"], ["L", "L"]]
assert diff(accepted, swapped)["equal"] is False
for fixture, code in (("reject-fan-in", "fan_in_unsupported"),
                      ("reject-cycle", "cycle_unsupported"),
                      ("reject-interface-channel-order", "interface_channel_order_mismatch")):
    try:
        fingerprint(parse_document((fixtures / (fixture + ".json")).read_bytes()))
    except IdentityError as error:
        assert error.code == code, (fixture, error.code)
    else:
        raise AssertionError(f"{fixture} unexpectedly accepted")
print(json.dumps({"status": "passed", "digest": result["digest"]}, sort_keys=True))

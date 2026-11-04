"""Independent planning vectors and refusal fixtures for the public package."""

import copy
import hashlib
import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "graph"))
from pulp_graph_identity import IdentityError, diff, fingerprint, manifest, parse_document
from pulp_graph_identity.identity import _gain

FIXTURES = Path(__file__).resolve().parents[1] / "examples" / "g3-io-gain-consumer" / "fixtures"
GOLDENS = FIXTURES / "g3-io-gain-goldens.jsonl"
INPUTS = FIXTURES


class IdentityVectors(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.accepted = parse_document((INPUTS / "accepted-omitted-defaults.json").read_bytes())
        cls.goldens = GOLDENS.read_bytes().splitlines()

    def test_complete_frozen_vectors(self):
        self.assertEqual(len(self.goldens), 2)
        empty = {"document_schema": "pulp.graph.authored-io-gain.v1"}
        for source, expected, digest in (
            (empty, self.goldens[0], "f6c20f9b5be41fd7ac76a5b08747f192a6a3c673daf5e5bf93409cfc84c78bb6"),
            (self.accepted, self.goldens[1], "2222eca305c4af8fe076923e5bb00b0cf0c9db9065a45f8cb78c5f64e6bb4e25"),
        ):
            actual = fingerprint(copy.deepcopy(source))
            self.assertEqual(actual["payload"], expected)
            self.assertEqual(actual["digest"], "sha256:" + digest)
            self.assertEqual(hashlib.sha256(b"pulp.graph.semantic.v1\0" + expected).hexdigest(), digest)

    def test_targeted_primary_refusals(self):
        for fixture, code in (("reject-fan-in", "fan_in_unsupported"),
                              ("reject-cycle", "cycle_unsupported"),
                              ("reject-interface-channel-order", "interface_channel_order_mismatch")):
            with self.subTest(fixture=fixture), self.assertRaises(IdentityError) as caught:
                fingerprint(parse_document((INPUTS / (fixture + ".json")).read_bytes()))
            self.assertEqual(caught.exception.code, code)

    def test_gain_default_and_order_mutations(self):
        base = copy.deepcopy(self.accepted)
        changed = copy.deepcopy(base)
        changed["nodes"]["gain"]["config"]["gain"] = "f32:3f800001"
        result = diff(base, changed)
        self.assertEqual(result["status"], "different")
        self.assertIn(["nodes", "gain", "config", "gain"], [item["path"] for item in result["changes"]])
        changed = copy.deepcopy(base)
        changed["nodes"]["gain"]["ports"]["in"]["audio"]["channels"] = ["R", "L"]
        with self.assertRaises(IdentityError) as caught:
            fingerprint(changed)
        self.assertEqual(caught.exception.code, "edge_channel_mismatch")

    def test_duplicate_keys_and_legacy_refusal(self):
        with self.assertRaises(IdentityError) as caught:
            parse_document(b'{"document_schema":"pulp.graph.authored-io-gain.v1","nodes":{},"nodes":{}}')
        self.assertEqual(caught.exception.code, "duplicate_json_key")
        for version in (1, 2, 3):
            with self.assertRaises(IdentityError) as caught:
                fingerprint({"format_version": version, "nodes": [{"id": 1}]})
            self.assertEqual(caught.exception.code, "logical_identity_missing")

    def test_closed_profile_refusals(self):
        mutations = (
            (lambda d: d["nodes"]["gain"].update({"plugin": "example"}), "unsupported_semantic_payload"),
            (lambda d: d["nodes"]["gain"].update({"kind": "processor"}), "unsupported_semantic_payload"),
            (lambda d: d["edges"][next(iter(d["edges"]))].update({"feedback": True}), "unsupported_semantic_payload"),
            (lambda d: d["interfaces"][0].update({"kind": "midi"}), "unsupported_semantic_payload"),
            (lambda d: d["nodes"]["gain"]["config"].update({"gain": "f32:7f800000"}), "malformed_float"),
            (lambda d: d["nodes"]["gain"]["config"].update({"gain": "f32:7fc00000"}), "malformed_float"),
            (lambda d: d["nodes"]["gain"]["ports"]["out"]["audio"].update({"channels": ["R", "L"]}), "edge_channel_mismatch"),
            (lambda d: d["nodes"].update({"extra": copy.deepcopy(d["nodes"]["gain"])}), "path_degree_invalid"),
        )
        for mutate, code in mutations:
            with self.subTest(code=code):
                document = copy.deepcopy(self.accepted)
                mutate(document)
                with self.assertRaises(IdentityError) as caught:
                    fingerprint(document)
                self.assertEqual(caught.exception.code, code)

    def test_signed_zero_and_numeric_spellings(self):
        values = ("1", "1.0", "1e0", "0", "-0")
        bits = []
        for value in values:
            document = copy.deepcopy(self.accepted)
            document["nodes"]["gain"]["config"]["gain"] = parse_document(value)
            bits.append(manifest(document)["nodes"]["gain"]["config"]["gain"])
        self.assertEqual(bits, ["f32:3f800000"] * 3 + ["f32:00000000", "f32:80000000"])

    def test_float_overflow_boundary(self):
        self.assertEqual(_gain(parse_document("340282356779733661637539395458142568447")), "f32:7f7fffff")
        with self.assertRaises(IdentityError) as caught:
            _gain(parse_document("340282356779733661637539395458142568448"))
        self.assertEqual(caught.exception.code, "malformed_float")

    def test_degree_precedes_missing_interface_in_any_node_order(self):
        for reverse in (False, True):
            document = copy.deepcopy(self.accepted)
            document["interfaces"] = []
            document["nodes"]["zz-disconnected"] = copy.deepcopy(document["nodes"]["gain"])
            if reverse:
                document["nodes"] = dict(reversed(list(document["nodes"].items())))
            with self.assertRaises(IdentityError) as caught:
                fingerprint(document)
            self.assertEqual(caught.exception.code, "path_degree_invalid")
            self.assertEqual(caught.exception.path, ["zz-disconnected"])

    def test_long_complete_path(self):
        document = copy.deepcopy(self.accepted)
        template = document["nodes"].pop("gain")
        document["nodes"].update({f"g{i:04d}": copy.deepcopy(template) for i in range(1100)})
        document["edges"] = {}
        path = ["input"] + [f"g{i:04d}" for i in range(1100)] + ["output"]
        for source, destination in zip(path, path[1:]):
            src = {"direction": "out", "kind": "audio", "node": source, "port": "audio"}
            dst = {"direction": "in", "kind": "audio", "node": destination, "port": "audio"}
            key = json.dumps(["audio", src, dst, None], sort_keys=True, separators=(",", ":"))
            document["edges"][key] = {"lane": "audio", "source": src, "destination": dst,
                                       "channel_map": [["L", "L"], ["R", "R"]]}
        self.assertEqual(len(fingerprint(document)["digest"]), 71)


if __name__ == "__main__":
    unittest.main()

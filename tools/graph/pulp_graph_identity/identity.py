"""Closed G-3 D1/D2 authored profile. No runtime graph or legacy deserialization."""

from __future__ import annotations

import hashlib
import copy
import json
import re
import struct
import unicodedata
from decimal import Decimal
from fractions import Fraction

DOMAIN = b"pulp.graph.semantic.v1\0"
SCHEMA = "pulp.graph.authored-io-gain.v1"
PROFILE = "pulp.graph.identity.v1"
POLICIES = {"feedback": "forbid", "event_order": "none", "rate_conversion": "none", "seed": None, "state_retention": "none", "fault": "fail"}


class IdentityError(ValueError):
    def __init__(self, code: str, path=()):
        super().__init__(code)
        self.code = code
        self.path = list(path)


def canonical(value) -> bytes:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")


def _pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise IdentityError("duplicate_json_key", (key,))
        result[key] = value
    return result


def parse_document(data: bytes | str):
    try:
        if isinstance(data, bytes):
            data = data.decode("utf-8", "strict")
        return json.loads(data, object_pairs_hook=_pairs, parse_float=Decimal, parse_int=Decimal,
                          parse_constant=lambda _: (_ for _ in ()).throw(IdentityError("malformed_float")))
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise IdentityError("invalid_json") from exc


def _shape(obj, keys, required=(), path=()):
    if not isinstance(obj, dict):
        raise IdentityError("unsupported_semantic_payload", path)
    if set(obj) - set(keys):
        raise IdentityError("unsupported_semantic_payload", (*path, sorted(set(obj) - set(keys))[0]))
    if set(required) - set(obj):
        raise IdentityError("unsupported_semantic_payload", (*path, sorted(set(required) - set(obj))[0]))


def _key(value, path):
    if not isinstance(value, str) or not value or not value.strip() or any(unicodedata.category(c) in ("Cc", "Cs") for c in value):
        raise IdentityError("invalid_logical_key", path)
    if unicodedata.normalize("NFC", value) != value:
        raise IdentityError("non_nfc_key", path)
    return value


def _channels(value, path):
    if not isinstance(value, list) or not value:
        raise IdentityError("unsupported_semantic_payload", path)
    result = [_key(v, (*path, i)) for i, v in enumerate(value)]
    if len(set(result)) != len(result):
        raise IdentityError("duplicate_channel_key", path)
    return result


def _gain(value):
    if isinstance(value, bool):
        raise IdentityError("malformed_float")
    if isinstance(value, str):
        if not re.fullmatch(r"f32:[0-9a-f]{8}", value):
            raise IdentityError("malformed_float")
        bits = int(value[4:], 16)
        if bits & 0x7f800000 == 0x7f800000:
            raise IdentityError("malformed_float")
        return value
    if not isinstance(value, (int, Decimal)):
        raise IdentityError("malformed_float")
    try:
        exact = abs(Fraction(value))
        negative = value.is_signed() if isinstance(value, Decimal) else value < 0
        def represented(bits):
            return Fraction.from_float(struct.unpack(">f", bits.to_bytes(4, "big"))[0])
        maximum = represented(0x7f7fffff)
        previous = represented(0x7f7ffffe)
        if exact >= maximum + (maximum - previous) / 2:
            raise IdentityError("malformed_float")
        low, high = 0, 0x7f7fffff
        while low < high:
            middle = (low + high) // 2
            if represented(middle) < exact:
                low = middle + 1
            else:
                high = middle
        candidates = (low - 1, low) if low else (0,)
        bits = min(candidates, key=lambda candidate: (abs(represented(candidate) - exact), candidate & 1))
        if negative:
            bits |= 0x80000000
    except (OverflowError, ValueError) as exc:
        raise IdentityError("malformed_float") from exc
    return f"f32:{bits:08x}"


def _endpoint(value, path):
    _shape(value, ("node", "direction", "kind", "port"), ("node", "direction", "kind", "port"), path)
    _key(value["node"], (*path, "node"))
    _key(value["port"], (*path, "port"))
    if value["direction"] not in ("in", "out") or value["kind"] != "audio":
        raise IdentityError("unsupported_semantic_payload", path)
    return value


def _port(value, path):
    _shape(value, ("kind", "channels", "contract"), ("kind", "channels", "contract"), path)
    if value["kind"] != "audio" or value["contract"] != "audio-port.v1":
        raise IdentityError("unsupported_semantic_payload", path)
    _channels(value["channels"], (*path, "channels"))


def manifest(doc):
    if unicodedata.unidata_version != "15.1.0":
        raise IdentityError("normalization_profile_mismatch")
    if not isinstance(doc, dict):
        raise IdentityError("unsupported_semantic_payload")
    doc = copy.deepcopy(doc)
    if "format_version" in doc:
        raise IdentityError("unsupported_document_version" if "document_schema" in doc or doc["format_version"] not in (1, 2, 3) else "logical_identity_missing")
    _shape(doc, ("document_schema", "profile", "nodes", "edges", "interfaces", "assets", "policies"))
    if doc.get("document_schema") != SCHEMA:
        raise IdentityError("unsupported_document_version")
    authored_policies = doc.get("policies", {})
    _shape(authored_policies, POLICIES, path=("policies",))
    root = {"profile": doc.get("profile", PROFILE), "nodes": doc.get("nodes", {}), "edges": doc.get("edges", {}), "interfaces": doc.get("interfaces", []), "assets": doc.get("assets", {}), "policies": {**POLICIES, **authored_policies}}
    if root["profile"] != PROFILE:
        raise IdentityError("unsupported_semantic_payload", ("profile",))
    _shape(root["policies"], POLICIES, POLICIES, ("policies",))
    if root["policies"] != POLICIES or root["assets"] != {}:
        raise IdentityError("unsupported_semantic_payload")
    nodes, edges, interfaces = root["nodes"], root["edges"], root["interfaces"]
    if not isinstance(nodes, dict) or not isinstance(edges, dict) or not isinstance(interfaces, list):
        raise IdentityError("unsupported_semantic_payload")
    for name, node in nodes.items():
        _key(name, ("nodes", name))
        path = ("nodes", name)
        _shape(node, ("kind", "kind_contract", "implementation", "config", "initial_state", "ports", "parameter_bindings", "policies"), ("kind", "kind_contract", "implementation", "config", "initial_state", "ports", "parameter_bindings", "policies"), path)
        kind = node["kind"]
        if kind not in ("audio_input", "audio_output", "gain"):
            raise IdentityError("unsupported_semantic_payload", (*path, "kind"))
        revision = "gain.v1" if kind == "gain" else "io.v1"
        implementation = "pulp.builtin." + kind.replace("_", "-") + ".v1"
        if node["kind_contract"] != revision or node["implementation"] != implementation or node["initial_state"] != {"mode": "default", "revision": revision} or node["parameter_bindings"] != {} or node["policies"] != {}:
            raise IdentityError("unsupported_semantic_payload", path)
        config = node["config"]
        _shape(config, ("gain",) if kind == "gain" else (), path=(*path, "config"))
        if kind == "gain":
            config["gain"] = _gain(config.get("gain", "f32:3f800000"))
        _shape(node["ports"], ("in", "out"), ("in", "out"), (*path, "ports"))
        for direction in ("in", "out"):
            ports = node["ports"][direction]
            active = kind == "gain" or (kind == "audio_input" and direction == "out") or (kind == "audio_output" and direction == "in")
            if not isinstance(ports, dict) or set(ports) != ({"audio"} if active else set()):
                raise IdentityError("unsupported_semantic_payload", (*path, "ports", direction))
            if active:
                _port(ports["audio"], (*path, "ports", direction, "audio"))
        if kind == "gain" and node["ports"]["in"]["audio"]["channels"] != node["ports"]["out"]["audio"]["channels"]:
            raise IdentityError("edge_channel_mismatch", path)
    bindings = {name: [] for name in nodes}
    seen_interfaces = set()
    for index, item in enumerate(interfaces):
        path = ("interfaces", index)
        _shape(item, ("key", "role", "kind", "endpoint", "channels"), ("key", "role", "kind", "endpoint", "channels"), path)
        key = _key(item["key"], (*path, "key"))
        if key in seen_interfaces:
            raise IdentityError("duplicate_interface_key", path)
        seen_interfaces.add(key)
        endpoint = _endpoint(item["endpoint"], (*path, "endpoint"))
        role = item["role"]
        if role not in ("input", "output") or item["kind"] != "audio" or endpoint["direction"] != ("out" if role == "input" else "in"):
            raise IdentityError("unsupported_semantic_payload", path)
        name = endpoint["node"]
        if name not in nodes or nodes[name]["kind"] != "audio_" + role or endpoint["port"] != "audio":
            raise IdentityError("unsupported_semantic_payload", path)
        bindings[name].append(index)
        if _channels(item["channels"], (*path, "channels")) != nodes[name]["ports"][endpoint["direction"]]["audio"]["channels"]:
            raise IdentityError("interface_channel_order_mismatch", path)
    indeg = {name: 0 for name in nodes}
    outdeg = {name: 0 for name in nodes}
    for edge_key, edge in edges.items():
        path = ("edges", edge_key)
        _shape(edge, ("lane", "source", "destination", "semantic_order", "feedback", "channel_map"), ("lane", "source", "destination", "channel_map"), path)
        edge.setdefault("feedback", False)
        edge.setdefault("semantic_order", None)
        if edge["lane"] != "audio" or edge["feedback"] is not False or edge["semantic_order"] is not None:
            raise IdentityError("unsupported_semantic_payload", path)
        src, dst = _endpoint(edge["source"], (*path, "source")), _endpoint(edge["destination"], (*path, "destination"))
        if src["direction"] != "out" or dst["direction"] != "in" or edge_key != canonical(["audio", src, dst, None]).decode():
            raise IdentityError("unsupported_semantic_payload", path)
        a, b = src["node"], dst["node"]
        if a not in nodes or b not in nodes or src["port"] != "audio" or dst["port"] != "audio":
            raise IdentityError("unsupported_semantic_payload", path)
        source_channels = nodes[a]["ports"]["out"].get("audio", {}).get("channels")
        dest_channels = nodes[b]["ports"]["in"].get("audio", {}).get("channels")
        if source_channels is None or dest_channels is None or source_channels != dest_channels or edge["channel_map"] != [[c, c] for c in source_channels]:
            raise IdentityError("edge_channel_mismatch", path)
        outdeg[a] += 1
        indeg[b] += 1
    if any(v > 1 for v in indeg.values()):
        raise IdentityError("fan_in_unsupported", (min(k for k, v in indeg.items() if v > 1),))
    if any(v > 1 for v in outdeg.values()):
        raise IdentityError("split_unsupported", (min(k for k, v in outdeg.items() if v > 1),))
    adjacency = {name: [] for name in nodes}
    for edge in edges.values():
        adjacency[edge["source"]["node"]].append(edge["destination"]["node"])
    visited, visiting = set(), set()
    for start in sorted(nodes):
        stack = [(start, False)]
        while stack:
            name, leaving = stack.pop()
            if leaving:
                visiting.remove(name)
                visited.add(name)
                continue
            if name in visiting:
                raise IdentityError("cycle_unsupported", (name,))
            if name in visited:
                continue
            visiting.add(name)
            stack.append((name, True))
            stack.extend((target, False) for target in reversed(adjacency[name]))
    for name in sorted(nodes):
        node = nodes[name]
        kind = node["kind"]
        expected = (0, 1) if kind == "audio_input" else (1, 0) if kind == "audio_output" else (1, 1)
        if (indeg[name], outdeg[name]) != expected:
            raise IdentityError("path_degree_invalid", (name,))
    for name in sorted(nodes):
        node = nodes[name]
        kind = node["kind"]
        if kind != "gain" and len(bindings[name]) != 1:
            raise IdentityError("incomplete_path", (name,))
    return root


def fingerprint(document):
    payload = canonical(manifest(document))
    return {"profile": PROFILE, "domain": "semantic", "payload": payload, "digest": "sha256:" + hashlib.sha256(DOMAIN + payload).hexdigest()}


def _changes(before, after, path=()):
    if before == after:
        return []
    if isinstance(before, dict) and isinstance(after, dict):
        result = []
        for key in sorted(set(before) | set(after)):
            child = (*path, key)
            if key not in before:
                result.append({"op": "add", "path": list(child), "before": None, "after": after[key], "reason": "semantic_value_changed"})
            elif key not in after:
                result.append({"op": "remove", "path": list(child), "before": before[key], "after": None, "reason": "semantic_value_changed"})
            else:
                result += _changes(before[key], after[key], child)
        return result
    return [{"op": "replace", "path": list(path), "before": before, "after": after, "reason": "semantic_value_changed"}]


def diff(before, after):
    first, second = fingerprint(before), fingerprint(after)
    left, right = json.loads(first["payload"]), json.loads(second["payload"])
    changes = _changes(left, right)
    changes.sort(key=lambda item: (canonical(item["path"]), item["op"]))
    return {"schema": "pulp.graph.diff.v1", "domain": "semantic", "profile": PROFILE,
            "status": "equal" if first["payload"] == second["payload"] else "different",
            "before": first["digest"], "after": second["digest"],
            "equal": first["payload"] == second["payload"], "changes": changes, "diagnostics": []}

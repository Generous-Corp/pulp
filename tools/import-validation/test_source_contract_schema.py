#!/usr/bin/env python3
"""Conformance test for the shared source-contract-v0 schema.

Current goal: one serialized `source-contract` schema that both the C++
importer overlay (`source_contract_overlay.node_route_rows`) and the JS audit
summary (`inputs.sourceAuditSummary`) can be validated against in tests, so
native route readiness stops depending on two parallel inference models that can
silently disagree.

This test:
  1. loads and structurally validates the schema file itself;
  2. validates a representative importer-emitted overlay against the per-node
     schema (`node_route_row`);
  3. validates the audit `sourceAuditSummary` materiality block against the
     materiality schema;
  4. proves importer and audit AGREE on the shared golden expectations by
     running both through the REAL frontend-IR consumers
     (`frontend_ir_routes`/`frontend_ir_sources`) and asserting the overlapping
     count keys land on the same values.

stdlib only. No `jsonschema` dependency (it is not vendored); a small explicit
validator interprets the subset of draft-2020-12 keywords the schema uses, in
the spirit of tools/scripts/frontend_ir_validation.py.
"""

from __future__ import annotations

import importlib.util
import json
import pathlib
import re
import sys
import unittest


REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
SCHEMA_PATH = REPO_ROOT / "tools" / "import-validation" / "schemas" / "source-contract-v0.schema.json"
FIXTURE_DIR = REPO_ROOT / "tools" / "import-validation" / "fixtures" / "source-contract-v0"
SCRIPTS_DIR = REPO_ROOT / "tools" / "scripts"


def _load_consumer(name: str):
    """Load a frontend_ir_* consumer module from tools/scripts by file path.

    These modules import each other by bare name, so tools/scripts must be on
    sys.path while they load.
    """
    if str(SCRIPTS_DIR) not in sys.path:
        sys.path.insert(0, str(SCRIPTS_DIR))
    spec = importlib.util.spec_from_file_location(name, SCRIPTS_DIR / f"{name}.py")
    assert spec and spec.loader, f"could not load {name}"
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def load_json(path: pathlib.Path) -> dict:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise AssertionError(f"{path} did not contain a JSON object")
    return data


class SchemaError(AssertionError):
    pass


class MiniValidator:
    """A small validator for the draft-2020-12 keyword subset this schema uses.

    Supported: type, const, enum, required, properties, additionalProperties,
    patternProperties, items, minimum, minLength, oneOf, and local $ref into
    #/$defs/. Keys with a leading underscore in instances (fixture `_comment`s)
    are tolerated by `additionalProperties: true` exactly like any other extra.
    """

    def __init__(self, schema: dict) -> None:
        self.root = schema
        self.defs = schema.get("$defs", {})

    def _resolve(self, schema: dict) -> dict:
        ref = schema.get("$ref")
        if not ref:
            return schema
        if not ref.startswith("#/$defs/"):
            raise SchemaError(f"unsupported $ref: {ref}")
        name = ref[len("#/$defs/"):]
        if name not in self.defs:
            raise SchemaError(f"unknown $def: {name}")
        return self.defs[name]

    def validate(self, instance, schema: dict, path: str = "$") -> None:
        schema = self._resolve(schema)

        if "oneOf" in schema:
            matches = []
            errors = []
            for index, branch in enumerate(schema["oneOf"]):
                try:
                    self.validate(instance, branch, path)
                    matches.append(index)
                except AssertionError as exc:  # SchemaError is an AssertionError
                    errors.append(f"  branch {index}: {exc}")
            if len(matches) != 1:
                joined = "\n".join(errors) if errors else ""
                raise SchemaError(
                    f"{path}: expected exactly one oneOf branch to match, matched {matches}\n{joined}"
                )
            return

        expected_type = schema.get("type")
        if expected_type == "object":
            if not isinstance(instance, dict):
                raise SchemaError(f"{path}: expected object, got {type(instance).__name__}")
            self._validate_object(instance, schema, path)
        elif expected_type == "array":
            if not isinstance(instance, list):
                raise SchemaError(f"{path}: expected array, got {type(instance).__name__}")
            item_schema = schema.get("items")
            if item_schema is not None:
                for index, item in enumerate(instance):
                    self.validate(item, item_schema, f"{path}[{index}]")
        elif expected_type == "string":
            if not isinstance(instance, str):
                raise SchemaError(f"{path}: expected string, got {type(instance).__name__}")
            if "minLength" in schema and len(instance) < schema["minLength"]:
                raise SchemaError(f"{path}: string shorter than minLength {schema['minLength']}")
        elif expected_type == "integer":
            if not isinstance(instance, int) or isinstance(instance, bool):
                raise SchemaError(f"{path}: expected integer, got {type(instance).__name__}")
            if "minimum" in schema and instance < schema["minimum"]:
                raise SchemaError(f"{path}: integer below minimum {schema['minimum']}")
        elif expected_type == "number":
            if not isinstance(instance, (int, float)) or isinstance(instance, bool):
                raise SchemaError(f"{path}: expected number, got {type(instance).__name__}")

        if "const" in schema and instance != schema["const"]:
            raise SchemaError(f"{path}: expected const {schema['const']!r}, got {instance!r}")
        if "enum" in schema and instance not in schema["enum"]:
            raise SchemaError(f"{path}: {instance!r} not in enum {schema['enum']}")

    def _validate_object(self, instance: dict, schema: dict, path: str) -> None:
        for key in schema.get("required", []):
            if key not in instance:
                raise SchemaError(f"{path}: missing required property {key!r}")

        props = schema.get("properties", {})
        for key, value in instance.items():
            if key in props:
                self.validate(value, props[key], f"{path}.{key}")
                continue
            matched_pattern = False
            for pattern, pattern_schema in schema.get("patternProperties", {}).items():
                if re.search(pattern, key):
                    self.validate(value, pattern_schema, f"{path}.{key}")
                    matched_pattern = True
                    break
            if matched_pattern:
                continue
            additional = schema.get("additionalProperties", True)
            if additional is False:
                raise SchemaError(f"{path}: additional property {key!r} not permitted")
            if isinstance(additional, dict):
                self.validate(value, additional, f"{path}.{key}")


class SourceContractSchemaTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.schema = load_json(SCHEMA_PATH)
        cls.validator = MiniValidator(cls.schema)
        cls.importer = load_json(FIXTURE_DIR / "importer-overlay.json")
        cls.audit = load_json(FIXTURE_DIR / "audit-summary.json")
        cls.golden = load_json(FIXTURE_DIR / "golden-expectations.json")
        cls.routes = _load_consumer("frontend_ir_routes")
        cls.sources = _load_consumer("frontend_ir_sources")

    # 1. the schema file itself is structurally valid / loadable -------------
    def test_schema_is_structurally_valid(self) -> None:
        self.assertEqual(self.schema["$schema"], "https://json-schema.org/draft/2020-12/schema")
        self.assertEqual(self.schema["schema"], "pulp-source-contract-v0")
        self.assertIn("node_route_row", self.schema["$defs"])
        self.assertIn("materiality", self.schema["$defs"])
        self.assertIn("audit_summary", self.schema["$defs"])
        self.assertEqual(len(self.schema["oneOf"]), 2)
        # Every $ref must resolve.
        self._assert_refs_resolve(self.schema, self.schema.get("$defs", {}))

    def _assert_refs_resolve(self, node, defs) -> None:
        if isinstance(node, dict):
            ref = node.get("$ref")
            if isinstance(ref, str):
                self.assertTrue(ref.startswith("#/$defs/"), f"unsupported $ref {ref}")
                self.assertIn(ref[len("#/$defs/"):], defs, f"dangling $ref {ref}")
            for value in node.values():
                self._assert_refs_resolve(value, defs)
        elif isinstance(node, list):
            for value in node:
                self._assert_refs_resolve(value, defs)

    # 2. importer overlay conforms to the per-node schema -------------------
    def test_importer_overlay_conforms(self) -> None:
        self.validator.validate(self.importer, self.schema, "$")
        # Each row conforms to the per-node sub-schema specifically.
        row_schema = self.schema["$defs"]["node_route_row"]
        for index, row in enumerate(self.importer["node_route_rows"]):
            self.validator.validate(row, row_schema, f"node_route_rows[{index}]")

    def test_importer_overlay_rejects_row_without_route_type(self) -> None:
        broken = json.loads(json.dumps(self.importer))
        del broken["node_route_rows"][0]["route_type"]
        with self.assertRaises(AssertionError):
            self.validator.validate(broken, self.schema, "$")

    # 3. audit materiality block conforms to the materiality schema ---------
    def test_audit_summary_conforms(self) -> None:
        self.validator.validate(self.audit, self.schema, "$")
        materiality_schema = self.schema["$defs"]["materiality"]
        self.validator.validate(
            self.audit["audit_summary"]["materiality"], materiality_schema, "materiality"
        )

    def test_audit_materiality_rejects_negative_count(self) -> None:
        broken = json.loads(json.dumps(self.audit))
        broken["audit_summary"]["materiality"]["event_contracts"] = -1
        materiality_schema = self.schema["$defs"]["materiality"]
        with self.assertRaises(AssertionError):
            self.validator.validate(broken["audit_summary"]["materiality"], materiality_schema, "m")

    # 4. importer and audit AGREE on the shared golden expectations ---------
    def test_importer_and_audit_agree_on_shared_contract(self) -> None:
        """The 'two parallel inference models can no longer disagree' proof.

        Derive counts from the importer overlay via the real route/source
        consumers and from the audit summary via the same count_map consumer,
        then assert both land on the golden shared_counts.
        """
        rows = self.importer["node_route_rows"]
        shared = self.golden["shared_counts"]
        importer_keys = self.golden["importer_count_keys"]
        audit_keys = self.golden["audit_materiality_keys"]

        # Importer side: run the real consumers over the overlay rows.
        route_counts = self.routes.route_counts({"source_contract_overlay": {"node_route_rows": rows}}, rows)
        primitive_counts = self.routes.primitive_counts(rows)
        importer_counts = {**route_counts, **primitive_counts}

        # Audit side: run the real count_map over the sourceAuditSummary block.
        audit_summary = self.audit["audit_summary"]
        audit_counts = self.sources.count_map(audit_summary)

        # Node-level agreement: ids, spans, semantic roles, routes.
        derived_nodes = []
        for index, row in enumerate(rows):
            derived_nodes.append({
                "id": self.routes.row_node_id(row, index),
                "source_path": row.get("stable_source_path"),
                "source_line": row.get("source_line"),
                "semantic_role": self.routes.semantic_role(row),
                "route_type": self.routes.route_name(row.get("route_type")),
            })
        self.assertEqual(derived_nodes, self.golden["nodes"],
                         "importer-derived node identity/spans/roles drifted from golden")

        # Count agreement: shared_counts == importer-derived == audit-derived.
        for logical, expected in shared.items():
            with self.subTest(count=logical):
                importer_key = importer_keys.get(logical)
                if importer_key is not None:
                    self.assertEqual(
                        importer_counts.get(importer_key, 0), expected,
                        f"importer count {importer_key!r} != golden {logical}={expected}",
                    )
                audit_key = audit_keys.get(logical)
                if audit_key is not None:
                    # count_map prefixes materiality keys with 'materiality_'.
                    self.assertEqual(
                        audit_counts.get(f"materiality_{audit_key}", 0), expected,
                        f"audit materiality {audit_key!r} != golden {logical}={expected}",
                    )

        # Direct importer-vs-audit cross-check on the overlapping evidence:
        # the importer's per-row event/state tallies equal the audit's
        # materiality tallies for the same design.
        self.assertEqual(
            importer_counts.get("with_event_contracts", 0),
            audit_counts.get("materiality_event_contracts", 0),
            "importer event-contract count disagrees with audit materiality",
        )
        self.assertEqual(
            importer_counts.get("with_state_contracts", 0),
            audit_counts.get("materiality_set_state_events", 0),
            "importer state-contract count disagrees with audit materiality",
        )


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Conformance test for the framework-agnostic project-import IR v0 schema.

This is the SDK substrate's contract for project importers (add-on tools that
read an existing plugin project read-only and propose a Pulp migration
scaffold). The IR is project-shaped and deliberately distinct from the
design-import source-contract (tools/import-validation).

The test:
  1. structurally loads + sanity-checks the schema itself;
  2. validates the committed vendor-NEUTRAL example fixture against it;
  3. proves the validator REJECTS missing-required / bad-enum / wrong-type docs;
  4. enforces the vendor-agnostic rule: no vendor names appear in the schema or
     the mainline fixtures (vendor identity is runtime DATA only — plan §16.2).

stdlib only — no `jsonschema` dependency (it is not vendored). A compact
recursive validator interprets the subset of draft-2020-12 keywords the schema
uses, in the spirit of tools/import-validation/test_source_contract_schema.py.
"""
from __future__ import annotations

import json
import pathlib
import unittest

HERE = pathlib.Path(__file__).resolve().parent
SCHEMA_PATH = HERE / "schemas" / "project-import-ir-v0.schema.json"
FIXTURE_DIR = HERE / "fixtures" / "project-import-ir-v0"
SPI_SCHEMA_PATH = HERE / "schemas" / "import-spi-v0.schema.json"
SPI_FIXTURE_DIR = HERE / "fixtures" / "import-spi-v0"

# SPI companion fixtures validate against a specific self-contained $def (these
# carry no cross-file $ref, so the stdlib local-ref validator is sufficient).
SPI_FIXTURE_DEFS = {
    "plan": "pulp_import_plan",
    "manifest": "emission_manifest",
    "compat": "compat_matrix",
}

# Vendor names must never appear in SDK schema/fixtures — identity is runtime
# data only. This guard list is the mainline tripwire for plan §16.2.
FORBIDDEN_VENDOR_TOKENS = ("juce", "iplug", "steinberg", "projucer", "wdl")


# --- compact stdlib validator (subset of draft-2020-12) --------------------

_TYPE_CHECKS = {
    "object": lambda v: isinstance(v, dict),
    "array": lambda v: isinstance(v, list),
    "string": lambda v: isinstance(v, str),
    "integer": lambda v: isinstance(v, int) and not isinstance(v, bool),
    "number": lambda v: isinstance(v, (int, float)) and not isinstance(v, bool),
    "boolean": lambda v: isinstance(v, bool),
    "null": lambda v: v is None,
}


def _resolve(root: dict, ref: str) -> dict:
    assert ref.startswith("#/"), f"only local $ref supported: {ref}"
    node = root
    for part in ref[2:].split("/"):
        node = node[part]
    return node


def validate(value, schema: dict, root: dict, path: str, errors: list[str]) -> None:
    if "$ref" in schema:
        validate(value, _resolve(root, schema["$ref"]), root, path, errors)
        return

    t = schema.get("type")
    if t is not None:
        types = t if isinstance(t, list) else [t]
        if not any(_TYPE_CHECKS[tt](value) for tt in types):
            errors.append(f"{path}: expected type {types}, got {type(value).__name__}")
            return  # type wrong → don't cascade

    if "enum" in schema and value not in schema["enum"]:
        errors.append(f"{path}: {value!r} not in enum {schema['enum']}")

    if isinstance(value, (int, float)) and not isinstance(value, bool):
        if "minimum" in schema and value < schema["minimum"]:
            errors.append(f"{path}: {value} < minimum {schema['minimum']}")
        if "maximum" in schema and value > schema["maximum"]:
            errors.append(f"{path}: {value} > maximum {schema['maximum']}")

    if isinstance(value, dict):
        for req in schema.get("required", []):
            if req not in value:
                errors.append(f"{path}: missing required '{req}'")
        props = schema.get("properties", {})
        if schema.get("additionalProperties") is False:
            for k in value:
                if k not in props:
                    errors.append(f"{path}: unexpected property '{k}'")
        for k, sub in props.items():
            if k in value:
                validate(value[k], sub, root, f"{path}.{k}", errors)

    if isinstance(value, list) and "items" in schema:
        for i, item in enumerate(value):
            validate(item, schema["items"], root, f"{path}[{i}]", errors)


def errors_for(doc, schema) -> list[str]:
    errs: list[str] = []
    validate(doc, schema, schema, "$", errs)
    return errs


def errors_against_def(doc, schema, def_name: str) -> list[str]:
    errs: list[str] = []
    validate(doc, {"$ref": f"#/$defs/{def_name}"}, schema, "$", errs)
    return errs


# --- tests -----------------------------------------------------------------

class ProjectImportIRSchemaTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.schema = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))

    def test_schema_loads_and_is_shaped(self):
        self.assertEqual(self.schema.get("type"), "object")
        self.assertIn("$defs", self.schema)
        self.assertIn("schema", self.schema.get("required", []))

    def test_example_fixtures_validate(self):
        fixtures = sorted(FIXTURE_DIR.glob("*.json"))
        self.assertTrue(fixtures, "no fixtures found")
        for f in fixtures:
            doc = json.loads(f.read_text(encoding="utf-8"))
            errs = errors_for(doc, self.schema)
            self.assertEqual(errs, [], f"{f.name} did not validate: {errs[:5]}")

    def test_missing_required_is_rejected(self):
        doc = json.loads((FIXTURE_DIR / "example-effect.json").read_text())
        doc.pop("schema")
        self.assertTrue(errors_for(doc, self.schema), "missing 'schema' should fail")

    def test_bad_enum_is_rejected(self):
        doc = json.loads((FIXTURE_DIR / "example-effect.json").read_text())
        doc["metadata"]["pulp_category"] = "NotACategory"
        errs = errors_for(doc, self.schema)
        self.assertTrue(any("enum" in e for e in errs), errs)

    def test_wrong_type_is_rejected(self):
        doc = json.loads((FIXTURE_DIR / "example-effect.json").read_text())
        doc["parameters"] = {"not": "a list"}
        errs = errors_for(doc, self.schema)
        self.assertTrue(any("parameters" in e for e in errs), errs)

    def test_spi_schema_loads_and_is_shaped(self):
        spi = json.loads(SPI_SCHEMA_PATH.read_text(encoding="utf-8"))
        self.assertEqual(spi["$defs"]["request"]["properties"]["verb"]["enum"],
                         ["detect", "analyze", "plan", "emit"])
        for d in ("pulp_import_plan", "emission_manifest", "compat_matrix"):
            self.assertIn(d, spi["$defs"])

    def test_spi_companion_fixtures_validate(self):
        spi = json.loads(SPI_SCHEMA_PATH.read_text(encoding="utf-8"))
        found = sorted(SPI_FIXTURE_DIR.glob("*.json"))
        self.assertTrue(found, "no SPI fixtures found")
        for f in found:
            def_name = SPI_FIXTURE_DEFS.get(f.stem)
            self.assertIsNotNone(def_name, f"no $def mapping for SPI fixture {f.name}")
            doc = json.loads(f.read_text(encoding="utf-8"))
            errs = errors_against_def(doc, spi, def_name)
            self.assertEqual(errs, [], f"{f.name} did not validate against {def_name}: {errs[:5]}")

    def test_spi_bad_enum_is_rejected(self):
        spi = json.loads(SPI_SCHEMA_PATH.read_text(encoding="utf-8"))
        doc = json.loads((SPI_FIXTURE_DIR / "compat.json").read_text())
        doc["features"][0]["status"] = "mostly-supported"
        errs = errors_against_def(doc, spi, "compat_matrix")
        self.assertTrue(any("enum" in e for e in errs), errs)

    def test_no_vendor_names_in_schema_or_fixtures(self):
        files = [SCHEMA_PATH, SPI_SCHEMA_PATH]
        files += sorted(FIXTURE_DIR.glob("*.json")) + sorted(SPI_FIXTURE_DIR.glob("*.json"))
        for f in files:
            blob = f.read_text(encoding="utf-8").lower()
            for tok in FORBIDDEN_VENDOR_TOKENS:
                self.assertNotIn(tok, blob,
                                 f"vendor token {tok!r} leaked into {f.name} — SDK must stay vendor-agnostic (plan §16.2)")


if __name__ == "__main__":
    unittest.main()

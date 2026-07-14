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
  4. enforces the vendor-agnostic rule: no vendor names appear in the schema,
     importer README, or mainline fixtures (vendor identity is runtime DATA only
     — plan §16.2).

stdlib only — no `jsonschema` dependency (it is not vendored). A compact
recursive validator interprets the subset of draft-2020-12 keywords the schema
uses, in the spirit of tools/import-validation/test_source_contract_schema.py.
"""
from __future__ import annotations

import json
import pathlib
import unittest

HERE = pathlib.Path(__file__).resolve().parent
README_PATH = HERE / "README.md"
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

# Vendor names must never appear in SDK schema, fixtures, or importer README —
# identity is runtime DATA only.
#
# That guarantee is enforced by tools/scripts/framework_neutrality_check.py, which
# scans this whole tree (schemas, fixtures and README included) in CI. The token
# list lives THERE, once. It used to be duplicated here as a literal tuple, which
# made this file both a second source of truth AND an instance of the very thing
# it was screening for: the SDK spelling out which frameworks it anticipates.
FORBIDDEN_NEUTRAL_IMPL_TOKENS = (
    "<pulp/",
    "statetree",
    "propertyvalue",
    "statestore",
    "hostparamsurface",
    "modulationlane",
    "listenertoken",
    "syncedclone",
)
NEUTRAL_SCHEMA_DEFS = (
    "neutral_state_channel",
    "parameter_capabilities",
    "parameter_value_semantics",
    "parameter_binding",
)


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


def _json_blob(value) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":")).lower()


def _neutral_contract_blobs(doc: dict) -> list[tuple[str, str]]:
    blobs: list[tuple[str, str]] = []
    channel = doc.get("state_model", {}).get("neutral_state_channel")
    if channel is not None:
        blobs.append(("state_model.neutral_state_channel", _json_blob(channel)))
    for i, param in enumerate(doc.get("parameters", [])):
        for key in ("capabilities", "value_semantics", "binding"):
            if key in param:
                blobs.append((f"parameters[{i}].{key}", _json_blob(param[key])))
    return blobs


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

    def test_neutral_binding_fixture_covers_state_and_param_semantics(self):
        doc = json.loads((FIXTURE_DIR / "example-effect.json").read_text())
        channel = doc["state_model"]["neutral_state_channel"]
        self.assertEqual(doc["state_model"]["classification"], "tree-like")
        self.assertEqual(channel["snapshot"], "supported")
        self.assertEqual(channel["per_key_subscription"], "supported")
        self.assertFalse(channel["implementation_type_leak"])

        params = {p["id"]: p for p in doc["parameters"]}
        gain = params["gain"]
        self.assertFalse(gain["capabilities"]["modulatable"])
        self.assertEqual(gain["value_semantics"]["base_effective_split"],
                         "synthesized-zero-modulation")
        self.assertEqual(gain["value_semantics"]["effective_read"],
                         "base-plus-modulation-sum")
        self.assertEqual(gain["binding"]["stable_source_key"], "gain")

        cutoff = params["filter_cutoff"]
        self.assertTrue(cutoff["capabilities"]["modulatable"])
        self.assertEqual(cutoff["value_semantics"]["base_effective_split"], "native")
        self.assertIn("internal-modulator",
                      cutoff["value_semantics"]["modulation_write_sources"])

    def test_neutral_binding_bad_enums_are_rejected(self):
        doc = json.loads((FIXTURE_DIR / "example-effect.json").read_text())
        doc["state_model"]["neutral_state_channel"]["deltas"] = "yes"
        errs = errors_for(doc, self.schema)
        self.assertTrue(any("neutral_state_channel.deltas" in e and "enum" in e
                            for e in errs), errs)

        doc = json.loads((FIXTURE_DIR / "example-effect.json").read_text())
        doc["state_model"]["neutral_state_channel"]["implementation_type_leak"] = True
        errs = errors_for(doc, self.schema)
        self.assertTrue(any("implementation_type_leak" in e and "enum" in e
                            for e in errs), errs)

        doc = json.loads((FIXTURE_DIR / "example-effect.json").read_text())
        doc["parameters"][0]["value_semantics"]["base_effective_split"] = "collapsed"
        errs = errors_for(doc, self.schema)
        self.assertTrue(any("base_effective_split" in e and "enum" in e
                            for e in errs), errs)

        doc = json.loads((FIXTURE_DIR / "example-effect.json").read_text())
        doc["parameters"][0]["capabilities"]["modulatable"] = "sometimes"
        errs = errors_for(doc, self.schema)
        self.assertTrue(any("capabilities.modulatable" in e and "expected type" in e
                            for e in errs), errs)

    def test_legacy_state_classifier_still_validates(self):
        doc = json.loads((FIXTURE_DIR / "example-effect.json").read_text())
        doc["state_model"]["classification"] = "apvts-like"
        errs = errors_for(doc, self.schema)
        self.assertEqual(errs, [], errs)

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

    def test_neutral_contract_does_not_leak_pulp_runtime_types(self):
        for def_name in NEUTRAL_SCHEMA_DEFS:
            blob = _json_blob(self.schema["$defs"][def_name])
            for tok in FORBIDDEN_NEUTRAL_IMPL_TOKENS:
                self.assertNotIn(
                    tok, blob,
                    f"implementation type token {tok!r} leaked into schema $defs/{def_name}"
                )

        for fixture in sorted(FIXTURE_DIR.glob("*.json")):
            doc = json.loads(fixture.read_text(encoding="utf-8"))
            for path, blob in _neutral_contract_blobs(doc):
                for tok in FORBIDDEN_NEUTRAL_IMPL_TOKENS:
                    self.assertNotIn(
                        tok, blob,
                        f"implementation type token {tok!r} leaked into {fixture.name}:{path}"
                    )


if __name__ == "__main__":
    unittest.main()

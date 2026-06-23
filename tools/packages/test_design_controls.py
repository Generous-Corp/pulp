#!/usr/bin/env python3
"""P8 — schema coverage for the custom-control package category. A package that
provides design-import custom controls declares them under `design_controls`;
each entry maps a Figma identity (component_set_key / name_prefix) to the
`factory_id` the package registers via register_design_control_factory. These
tests pin the `design-control` sub-schema added to registry-schema.json."""
from __future__ import annotations
import json
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCHEMA = json.loads((ROOT / "tools" / "packages" / "registry-schema.json").read_text())
DC_SCHEMA = SCHEMA["definitions"]["design-control"]


class DesignControlSchemaTest(unittest.TestCase):
    def setUp(self) -> None:
        try:
            import jsonschema  # noqa: F401
        except ImportError:
            self.skipTest("jsonschema not installed")

    def _errors(self, obj) -> list[str]:
        import jsonschema
        return [e.message for e in
                jsonschema.Draft7Validator(DC_SCHEMA).iter_errors(obj)]

    # The schema's design-control shape is FLAT — identity fields sit at the top
    # level (factory_id + component_set_key? + name_prefix?), matching the TS
    # DesignControlEntry the resolver consumes and the library-manifest.json
    # convention. (A prior nested `match:{...}` shape silently broke the TS
    # resolver, which reads e.component_set_key directly — see the lockstep
    # round-trip test below.)
    def test_valid_by_component_key(self) -> None:
        self.assertEqual(self._errors({
            "factory_id": "acme.spinner",
            "component_set_key": "1:2abc",
            "description": "A 12-position rotary selector.",
        }), [])

    def test_valid_by_name_prefix(self) -> None:
        self.assertEqual(self._errors({
            "factory_id": "acme.spinner",
            "name_prefix": "Acme / Spinner",
        }), [])

    def test_factory_id_is_required(self) -> None:
        errs = self._errors({"name_prefix": "Acme / Spinner"})
        self.assertTrue(any("factory_id" in e for e in errs), errs)

    def test_identity_is_required(self) -> None:
        # factory_id alone can't identify any node → rejected (anyOf requires
        # component_set_key OR name_prefix).
        errs = self._errors({"factory_id": "acme.spinner"})
        self.assertTrue(errs, "an entry with no identity field should be rejected")

    def test_no_extra_properties(self) -> None:
        errs = self._errors({
            "factory_id": "x", "name_prefix": "p", "bogus": 1})
        self.assertTrue(any("bogus" in e or "additional" in e.lower() for e in errs), errs)

    def test_schema_matches_ts_resolver_shape(self) -> None:
        # Lockstep guard: the exact flat shape the TS DesignControlEntry /
        # library-registry.test.ts feeds the resolver MUST be schema-valid.
        # If the schema is re-nested (or a field renamed) this fails, catching
        # the schema↔TS drift that previously merged silently.
        ts_entry = {
            "factory_id": "acme.spinner",
            "component_set_key": "1:2abc",
            "name_prefix": "Acme / Spinner",
        }
        self.assertEqual(self._errors(ts_entry), [],
                         "schema must accept the flat DesignControlEntry shape")

    def test_package_schema_exposes_design_controls_array(self) -> None:
        # The package definition carries the optional design_controls array
        # pointing at this sub-schema.
        pkg_props = SCHEMA["definitions"]["package"]["properties"]
        self.assertIn("design_controls", pkg_props)
        self.assertEqual(pkg_props["design_controls"]["items"]["$ref"],
                         "#/definitions/design-control")


if __name__ == "__main__":
    unittest.main()

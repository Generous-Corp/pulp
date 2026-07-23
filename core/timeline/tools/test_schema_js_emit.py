#!/usr/bin/env python3
"""Tests for schema_js_emit.py and the JS-facade drift gate.

Covers what a projection surface must prove:

  * determinism — two emissions from one manifest are byte-identical;
  * completeness — every manifest `$defs` type becomes a frozen descriptor and
    every field becomes a field entry in the committed module;
  * kind mapping — each SchemaValueKind (and a `$ref`) projects to the expected
    JS runtime type;
  * confirm-the-failure — the shared drift gate PASSES when the committed module
    matches a fresh emission and FAILS (nonzero) when it is mutated, otherwise
    the gate proves nothing.

Runs without a C++ build: the generator is a pure function of the JSON manifest.
If `node` is available it additionally proves the emitted module parses, imports,
and is deeply frozen; otherwise that leg is skipped (not failed).
"""

from __future__ import annotations

import importlib.util
import json
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

_THIS = Path(__file__).resolve()
_TOOLS_DIR = _THIS.parent
_REPO_ROOT = _THIS.parents[3]
_GENERATOR = _TOOLS_DIR / "schema_js_emit.py"
_MANIFEST = _REPO_ROOT / "core" / "timeline" / "schema" / "timeline_schema.json"
_ARTIFACT = _REPO_ROOT / "core" / "timeline" / "schema" / "timeline_facade.js"
_DRIFT_CHECK = _REPO_ROOT / "tools" / "scripts" / "schema_drift_check.py"


def _load_generator():
    spec = importlib.util.spec_from_file_location("schema_js_emit", _GENERATOR)
    module = importlib.util.module_from_spec(spec)
    assert spec and spec.loader
    spec.loader.exec_module(module)
    return module


gen = _load_generator()


def _run_gate(artifact: Path) -> int:
    """Run the shared drift gate against `artifact`, generating from `_MANIFEST`."""
    emit_cmd = " ".join(
        shlex.quote(p)
        for p in [sys.executable, str(_GENERATOR), "--manifest", str(_MANIFEST)]
    )
    cmd = [
        sys.executable,
        str(_DRIFT_CHECK),
        "--artifact",
        str(artifact),
        "--emit-cmd",
        emit_cmd,
    ]
    return subprocess.run(cmd, capture_output=True).returncode


def main() -> int:
    failures: list[str] = []

    def check(name: str, condition: bool) -> None:
        if condition:
            print(f"ok - {name}")
        else:
            failures.append(name)
            print(f"not ok - {name}")

    manifest = json.loads(_MANIFEST.read_bytes())
    defs = manifest["$defs"]

    # --- determinism -------------------------------------------------------
    first = gen.generate(manifest)
    second = gen.generate(manifest)
    check("two emissions are byte-identical", first == second)
    check("emission is non-empty", bool(first))
    check("emission ends with a single trailing newline", first.endswith("\n") and not first.endswith("\n\n"))

    # --- completeness against the committed artifact -----------------------
    committed = _ARTIFACT.read_text()
    check("committed artifact matches a fresh emission", committed == first)

    all_types = True
    all_fields = True
    for schema_name, type_def in defs.items():
        # The schema name appears as a frozen descriptor key and in the sorted
        # name list; both use the raw string literal.
        if committed.count(json.dumps(schema_name)) < 2:
            all_types = False
        for field_name in type_def.get("properties", {}):
            if f"name: {json.dumps(field_name)}," not in committed:
                all_fields = False
    check("every manifest type is projected to a descriptor", all_types)
    check("every manifest field is projected to a field entry", all_fields)

    # --- kind mapping (synthetic manifest exercising every kind + $ref) ----
    synthetic = {
        "x-pulp-manifest-version": 1,
        "$defs": {
            "pulp.test.target": {
                "type": "object",
                "properties": {"flag": {"type": "boolean", "x-pulp-kind": "Boolean"}},
            },
            "pulp.test.all_kinds": {
                "type": "object",
                "x-pulp-domain": "Document",
                "x-pulp-current-version": 2,
                "properties": {
                    "b": {"type": "boolean", "x-pulp-kind": "Boolean"},
                    "u32": {"type": "integer", "x-pulp-kind": "U32"},
                    "i64": {"type": "string", "x-pulp-kind": "I64String"},
                    "u64": {"type": "string", "x-pulp-kind": "U64String"},
                    "s": {"type": "string", "x-pulp-kind": "String"},
                    "obj": {"type": "object", "x-pulp-kind": "Object"},
                    "arr": {"type": "array", "x-pulp-kind": "Array"},
                    "ref_obj": {
                        "type": "object",
                        "x-pulp-kind": "Object",
                        "$ref": "#/$defs/pulp.test.target",
                    },
                },
                "required": ["b", "u32"],
            },
        },
    }
    expect_js_type = {
        "b": "boolean",
        "u32": "number",
        "i64": "string",
        "u64": "string",
        "s": "string",
        "obj": "object",
        "arr": "array",
        "ref_obj": "object",
    }
    props = synthetic["$defs"]["pulp.test.all_kinds"]["properties"]
    required = set(synthetic["$defs"]["pulp.test.all_kinds"]["required"])
    for field_name, want in expect_js_type.items():
        obj = gen._field_object(field_name, props[field_name], required)
        check(f"kind maps {field_name!r} -> jsType {want!r}", f'jsType: "{want}"' in obj)
    # A $ref field records the referenced schema type.
    check(
        "$ref field records the referenced type",
        'ref: "pulp.test.target"' in gen._field_object("ref_obj", props["ref_obj"], required),
    )
    # Required vs optional.
    check("required field marked required: true", "required: true" in gen._field_object("b", props["b"], required))
    check("non-required field marked required: false", "required: false" in gen._field_object("s", props["s"], required))

    # Empty-props type emits an empty frozen field array.
    empty_js = gen.generate(
        {"x-pulp-manifest-version": 1, "$defs": {"pulp.test.empty": {"type": "object", "properties": {}}}}
    )
    check("empty type emits an empty frozen field array", "fields: Object.freeze([])," in empty_js)

    # --- confirm-the-failure via the shared drift gate ---------------------
    check("gate PASSES when committed artifact is in sync", _run_gate(_ARTIFACT) == 0)
    with tempfile.TemporaryDirectory() as tmp:
        mutated = Path(tmp) / "timeline_facade.js"
        mutated.write_text(committed + "// drift\n")
        check("gate FAILS when committed artifact is mutated", _run_gate(mutated) == 1)

    # --- optional: prove the emitted module parses + is frozen (node) ------
    node = shutil.which("node")
    if node is None:
        print("ok - node runtime check skipped (node not installed)")
    else:
        script = (
            "import { timelineSchema, timelineSchemaTypeNames } from %s;"
            "const names = timelineSchemaTypeNames;"
            "const ok = names.length > 0"
            " && names.every(n => timelineSchema[n] && timelineSchema[n].schemaType === n)"
            " && Object.isFrozen(timelineSchema)"
            " && names.every(n => Object.isFrozen(timelineSchema[n])"
            "    && Object.isFrozen(timelineSchema[n].fields));"
            "process.exit(ok ? 0 : 1);"
        ) % json.dumps(str(_ARTIFACT))
        rc = subprocess.run([node, "--input-type=module", "-e", script], capture_output=True).returncode
        check("emitted module parses, imports, and is deeply frozen (node)", rc == 0)

    if failures:
        print(f"\nFAILED: {len(failures)} case(s): {', '.join(failures)}", file=sys.stderr)
        return 1
    print("\nall schema_js_emit cases passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

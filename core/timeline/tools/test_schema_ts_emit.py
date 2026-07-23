#!/usr/bin/env python3
"""Tests for schema_ts_emit.py and the TypeScript-surface drift gate.

Covers the three things a projection surface must prove:

  * determinism — two emissions from one manifest are byte-identical;
  * completeness — every manifest `$defs` type becomes an interface and every
    field becomes a property in the committed artifact;
  * kind mapping — each SchemaValueKind (and a `$ref`) projects to the expected
    TS type;
  * confirm-the-failure — the shared drift gate PASSES when the committed
    `.d.ts` matches a fresh emission and FAILS (nonzero) when it is mutated,
    otherwise the gate proves nothing.

Runs without a C++ build: the generator is a pure function of the JSON manifest.
"""

from __future__ import annotations

import importlib.util
import json
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

_THIS = Path(__file__).resolve()
_TOOLS_DIR = _THIS.parent
_REPO_ROOT = _THIS.parents[3]
_GENERATOR = _TOOLS_DIR / "schema_ts_emit.py"
_MANIFEST = _REPO_ROOT / "core" / "timeline" / "schema" / "timeline_schema.json"
_ARTIFACT = _REPO_ROOT / "core" / "timeline" / "schema" / "timeline_types.d.ts"
_DRIFT_CHECK = _REPO_ROOT / "tools" / "scripts" / "schema_drift_check.py"


def _load_generator():
    spec = importlib.util.spec_from_file_location("schema_ts_emit", _GENERATOR)
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

    all_projected = True
    all_fields_present = True
    for schema_name, type_def in defs.items():
        iface = gen._interface_name(schema_name)
        if f"export interface {iface}" not in committed:
            all_projected = False
        # Name union + name->interface map must reference every type.
        if json.dumps(schema_name) not in committed:
            all_projected = False
        for field_name in type_def.get("properties", {}):
            if gen._property_name(field_name) not in committed:
                all_fields_present = False
    check("every manifest type is projected to an interface", all_projected)
    check("every manifest field is projected to a property", all_fields_present)

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
                    "ref_arr": {
                        "type": "array",
                        "x-pulp-kind": "Array",
                        "$ref": "#/$defs/pulp.test.target",
                    },
                },
                "required": ["b", "u32"],
            },
        },
    }
    expect = {
        "b": "boolean",
        "u32": "number | string",
        "i64": "number | string",
        "u64": "number | string",
        "s": "string",
        "obj": "Record<string, unknown>",
        "arr": "readonly unknown[]",
        "ref_obj": "PulpTestTarget",
        "ref_arr": "readonly PulpTestTarget[]",
    }
    props = synthetic["$defs"]["pulp.test.all_kinds"]["properties"]
    for field_name, want in expect.items():
        got = gen._field_type(props[field_name])
        check(f"kind maps {field_name!r} -> {want!r}", got == want)

    synthetic_ts = gen.generate(synthetic)
    # Required vs optional marker.
    check("required field has no '?'", "b: boolean;" in synthetic_ts)
    check("non-required field is optional", "s?: string;" in synthetic_ts)
    # Empty-props type emits a marker interface, not a body.
    empty_ts = gen.generate(
        {"x-pulp-manifest-version": 1, "$defs": {"pulp.test.empty": {"type": "object", "properties": {}}}}
    )
    check("empty type emits a marker interface", "export interface PulpTestEmpty {}" in empty_ts)

    # Collision guard: two names that PascalCase to the same identifier fail.
    collide = {
        "x-pulp-manifest-version": 1,
        "$defs": {
            "a.b_c": {"type": "object", "properties": {}},
            "a.b.c": {"type": "object", "properties": {}},
        },
    }
    collided = False
    try:
        gen.generate(collide)
    except gen.ManifestError:
        collided = True
    check("interface-name collision is rejected", collided)

    # --- confirm-the-failure via the shared drift gate ---------------------
    check("gate PASSES when committed artifact is in sync", _run_gate(_ARTIFACT) == 0)
    with tempfile.TemporaryDirectory() as tmp:
        mutated = Path(tmp) / "timeline_types.d.ts"
        mutated.write_text(_ARTIFACT.read_text() + "// drift\n")
        check("gate FAILS when committed artifact is mutated", _run_gate(mutated) == 1)

    if failures:
        print(f"\nFAILED: {len(failures)} case(s): {', '.join(failures)}", file=sys.stderr)
        return 1
    print("\nall schema_ts_emit cases passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

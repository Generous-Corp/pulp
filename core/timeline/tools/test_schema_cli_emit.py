#!/usr/bin/env python3
"""Tests for schema_cli_emit.py and the CLI-verb-surface drift gate.

Covers what a projection surface must prove:

  * determinism — two emissions from one manifest are byte-identical;
  * completeness — every manifest `$defs` type becomes a verb and every field
    becomes a flag in the committed artifact;
  * kind mapping — each SchemaValueKind (and a `$ref`) projects to the expected
    CLI value type;
  * confirm-the-failure — the shared drift gate PASSES when the committed verb
    JSON matches a fresh emission and FAILS (nonzero) when it is mutated,
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
_GENERATOR = _TOOLS_DIR / "schema_cli_emit.py"
_MANIFEST = _REPO_ROOT / "core" / "timeline" / "schema" / "timeline_schema.json"
_ARTIFACT = _REPO_ROOT / "core" / "timeline" / "schema" / "timeline_cli_verbs.json"
_DRIFT_CHECK = _REPO_ROOT / "tools" / "scripts" / "schema_drift_check.py"


def _load_generator():
    spec = importlib.util.spec_from_file_location("schema_cli_emit", _GENERATOR)
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
    # The emission must itself be valid JSON.
    parsed = json.loads(first)
    check("emission is valid JSON", isinstance(parsed, dict) and "verbs" in parsed)

    # --- completeness against the committed artifact -----------------------
    committed_text = _ARTIFACT.read_text()
    check("committed artifact matches a fresh emission", committed_text == first)

    committed = json.loads(committed_text)
    verbs_by_type = {v["schema_type"]: v for v in committed["verbs"]}
    check("verb count equals manifest type count", len(committed["verbs"]) == len(defs))

    all_types_projected = True
    all_fields_projected = True
    for schema_name, type_def in defs.items():
        verb = verbs_by_type.get(schema_name)
        if verb is None:
            all_types_projected = False
            continue
        flag_fields = {f["schema_field"] for f in verb["flags"]}
        for field_name in type_def.get("properties", {}):
            if field_name not in flag_fields:
                all_fields_projected = False
    check("every manifest type is projected to a verb", all_types_projected)
    check("every manifest field is projected to a flag", all_fields_projected)

    # Verbs and flags are name-sorted (the byte-stability contract).
    verb_names = [v["name"] for v in committed["verbs"]]
    check("verbs are name-sorted", verb_names == sorted(verb_names))
    flags_sorted = all(
        [f["name"] for f in v["flags"]] == sorted(f["name"] for f in v["flags"])
        for v in committed["verbs"]
    )
    check("flags are name-sorted within each verb", flags_sorted)

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
    expect_value_type = {
        "b": "bool",
        "u32": "uint",
        "i64": "int",
        "u64": "uint",
        "s": "string",
        "obj": "json",
        "arr": "json",
        "ref_obj": "json",
    }
    props = synthetic["$defs"]["pulp.test.all_kinds"]["properties"]
    required = set(synthetic["$defs"]["pulp.test.all_kinds"]["required"])
    for field_name, want in expect_value_type.items():
        got = gen._flag(field_name, props[field_name], required)["value_type"]
        check(f"kind maps {field_name!r} -> value_type {want!r}", got == want)

    # A $ref flag records its referenced schema type.
    ref_flag = gen._flag("ref_obj", props["ref_obj"], required)
    check("$ref flag records the referenced type", ref_flag.get("ref") == "pulp.test.target")
    # Required vs optional is carried per flag.
    check("required flag is marked required", gen._flag("b", props["b"], required)["required"] is True)
    check("non-required flag is marked optional", gen._flag("s", props["s"], required)["required"] is False)

    # Verb-name projection: prefix dropped, hierarchy ':' , words kebab-cased.
    check(
        "verb name drops prefix, uses ':' and kebab-case",
        gen._verb_name("pulp.timeline.automation_target.device_parameter")
        == "timeline:automation-target:device-parameter",
    )

    # Empty-props type emits a verb with zero flags.
    empty_ts = gen.generate(
        {"x-pulp-manifest-version": 1, "$defs": {"pulp.test.empty": {"type": "object", "properties": {}}}}
    )
    empty_doc = json.loads(empty_ts)
    check("empty type emits a verb with no flags", empty_doc["verbs"][0]["flags"] == [])

    # Collision guard: two names projecting to the same verb token fail.
    collide = {
        "x-pulp-manifest-version": 1,
        "$defs": {
            "pulp.a_b": {"type": "object", "properties": {}},
            "pulp.a-b": {"type": "object", "properties": {}},
        },
    }
    collided = False
    try:
        gen.generate(collide)
    except gen.ManifestError:
        collided = True
    check("verb collision is rejected", collided)

    # --- confirm-the-failure via the shared drift gate ---------------------
    check("gate PASSES when committed artifact is in sync", _run_gate(_ARTIFACT) == 0)
    with tempfile.TemporaryDirectory() as tmp:
        mutated = Path(tmp) / "timeline_cli_verbs.json"
        mutated.write_text(committed_text.replace('"uint"', '"int"', 1))
        check("gate FAILS when committed artifact is mutated", _run_gate(mutated) == 1)

    if failures:
        print(f"\nFAILED: {len(failures)} case(s): {', '.join(failures)}", file=sys.stderr)
        return 1
    print("\nall schema_cli_emit cases passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

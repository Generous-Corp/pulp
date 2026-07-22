#!/usr/bin/env python3
"""Generate the timeline JS facade from the schema manifest.

Thin, deterministic projection: reads the canonical JSON-Schema manifest
(core/timeline/schema/timeline_schema.json) and emits an ES module that exports
a frozen runtime description of every schema type — its domain, version, and
fields (each with the JS runtime type of its value, derived from the lossless
`x-pulp-kind`). This is the runtime-JS counterpart to the compile-time `.d.ts`:
the JS engine imports it directly, no JSON parse step. It is a pure function of
the manifest — it never touches the SchemaRegistry — so the facade is a
projection of the JSON manifest, not a second source of truth.

    schema_js_emit.py                 # emit to stdout from the default manifest
    schema_js_emit.py --manifest <f>  # read a specific manifest
    schema_js_emit.py --out <f>       # write to a file instead of stdout

The committed module is guarded by its own drift gate (the shared
tools/scripts/schema_drift_check.py, wired as the `timeline-schema-js-drift`
ctest): it must match a fresh emission from the manifest, so a manifest change
that is not re-projected here is caught.

Output is byte-stable: schema types and their fields are name-sorted and every
object literal is emitted in a fixed key order, so the same manifest always
yields identical bytes. A trailing newline is appended so the committed artifact
is POSIX-friendly and matches the drift gate's byte-exact comparison.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

# Repo-relative default manifest, resolved from this file's location
# (<repo>/core/timeline/tools/schema_js_emit.py). The subsystem-local generator
# lives beside the JSON, TS, and CLI emitters; the committed manifest is its
# input.
_THIS = Path(__file__).resolve()
_REPO_ROOT = _THIS.parents[3]
DEFAULT_MANIFEST = _REPO_ROOT / "core" / "timeline" / "schema" / "timeline_schema.json"

_INDENT = "  "

# SchemaValueKind (carried losslessly as `x-pulp-kind`) -> the JS runtime type of
# the field's value (what `typeof`/Array.isArray sees). Unlike the compile-time
# `.d.ts` (which widens the numeric kinds to `number | string`), the runtime
# facade reports the actual carried type: `U32` is a JS number, while the 64-bit
# kinds are strings (carried as strings to survive JS number precision). A field
# `$ref` keeps its structural type (`object`/`array`) and records the referenced
# schema type alongside.
_KIND_TO_JS = {
    "Boolean": "boolean",
    "U32": "number",
    "I64String": "string",
    "U64String": "string",
    "String": "string",
    "Object": "object",
    "Array": "array",
}


class ManifestError(RuntimeError):
    """Raised when the manifest is missing required structure."""


def _js_string(value: str) -> str:
    """A JS string literal (double-quoted, JSON-escaped)."""
    return json.dumps(value, ensure_ascii=False)


def _ref_target(ref: str) -> str:
    """`#/$defs/pulp.timeline.track` -> `pulp.timeline.track`."""
    prefix = "#/$defs/"
    if not ref.startswith(prefix):
        raise ManifestError(f"unsupported $ref form: {ref!r}")
    return ref[len(prefix):]


def _field_object(field_name: str, field: dict, required: set[str]) -> str:
    """One frozen field descriptor as a single-line JS object literal."""
    kind = field.get("x-pulp-kind")
    if kind is None:
        raise ManifestError(f"field is missing x-pulp-kind: {field!r}")
    js_type = _KIND_TO_JS.get(kind)
    if js_type is None:
        raise ManifestError(f"unmapped x-pulp-kind: {kind!r}")
    parts = [
        f"name: {_js_string(field_name)}",
        f"kind: {_js_string(kind)}",
        f"jsType: {_js_string(js_type)}",
        f"required: {'true' if field_name in required else 'false'}",
    ]
    ref = field.get("$ref")
    if ref is not None:
        parts.append(f"ref: {_js_string(_ref_target(ref))}")
    return "Object.freeze({ " + ", ".join(parts) + " })"


def _type_object(schema_name: str, type_def: dict) -> list[str]:
    domain = type_def.get("x-pulp-domain", "Document")
    version = type_def.get("x-pulp-current-version", 1)
    properties = type_def.get("properties", {})
    required = set(type_def.get("required", []))

    lines = [f"{_INDENT}{_js_string(schema_name)}: Object.freeze({{"]
    lines.append(f"{_INDENT * 2}schemaType: {_js_string(schema_name)},")
    lines.append(f"{_INDENT * 2}domain: {_js_string(domain)},")
    lines.append(f"{_INDENT * 2}version: {version},")
    if not properties:
        lines.append(f"{_INDENT * 2}fields: Object.freeze([]),")
    else:
        lines.append(f"{_INDENT * 2}fields: Object.freeze([")
        for field_name in sorted(properties):
            lines.append(f"{_INDENT * 3}{_field_object(field_name, properties[field_name], required)},")
        lines.append(f"{_INDENT * 2}]),")
    lines.append(f"{_INDENT}}}),")
    return lines


def generate(manifest: dict) -> str:
    defs = manifest.get("$defs")
    if not isinstance(defs, dict):
        raise ManifestError("manifest has no $defs object")
    if not defs:
        raise ManifestError("manifest $defs is empty")

    manifest_version = manifest.get("x-pulp-manifest-version", 1)
    schema_names = sorted(defs)

    lines = [
        "// Generated by schema_js_emit.py from core/timeline/schema/timeline_schema.json.",
        "// Do not edit by hand: this file is a projection of the timeline schema manifest,",
        "// regenerated and drift-checked in CI. Change the SchemaRegistry (and regenerate",
        "// the manifest) instead of editing this file.",
        f"// Manifest version: {manifest_version}",
        "",
        f"export const timelineSchemaManifestVersion = {manifest_version};",
        "",
        "// Frozen runtime descriptor for every timeline schema type, keyed by schema name.",
        "export const timelineSchema = Object.freeze({",
    ]
    for name in schema_names:
        lines.extend(_type_object(name, defs[name]))
    lines.append("});")
    lines.append("")
    lines.append("// Every registered timeline schema-type name, sorted.")
    lines.append("export const timelineSchemaTypeNames = Object.freeze([")
    for name in schema_names:
        lines.append(f"{_INDENT}{_js_string(name)},")
    lines.append("]);")

    return "\n".join(lines) + "\n"


def _load_manifest(path: Path) -> dict:
    try:
        data = json.loads(path.read_bytes())
    except FileNotFoundError as exc:
        raise ManifestError(f"manifest not found: {path}") from exc
    except json.JSONDecodeError as exc:
        raise ManifestError(f"manifest is not valid JSON ({path}): {exc}") from exc
    if not isinstance(data, dict):
        raise ManifestError(f"manifest root is not an object: {path}")
    return data


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--manifest",
        type=Path,
        default=DEFAULT_MANIFEST,
        help="path to the committed schema manifest (timeline_schema.json)",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=None,
        help="write the JS module here instead of stdout",
    )
    args = parser.parse_args(argv)

    try:
        manifest = _load_manifest(args.manifest)
        text = generate(manifest)
    except ManifestError as exc:
        print(f"schema-js-emit: {exc}", file=sys.stderr)
        return 2

    data = text.encode("utf-8")
    if args.out is not None:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_bytes(data)
    else:
        sys.stdout.buffer.write(data)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

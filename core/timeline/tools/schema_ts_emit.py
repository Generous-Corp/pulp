#!/usr/bin/env python3
"""Generate TypeScript type definitions from the timeline schema manifest.

Thin, deterministic projection: reads the canonical JSON-Schema manifest
(core/timeline/schema/timeline_schema.json) and emits a `.d.ts` declaring one
`export interface` per registered schema type, plus a name union and a
name->interface map. This is a pure function of the manifest — it never touches
the SchemaRegistry — so the TS surface is a projection of the JSON manifest, not
a second source of truth.

    schema_ts_emit.py                 # emit to stdout from the default manifest
    schema_ts_emit.py --manifest <f>  # read a specific manifest
    schema_ts_emit.py --out <f>       # write to a file instead of stdout

The `.d.ts` is guarded by its own drift gate (the shared
tools/scripts/schema_drift_check.py, wired as the `timeline-schema-ts-drift`
ctest): the committed file must match a fresh emission from the manifest, so a
manifest change that is not re-projected here is caught.

Output is byte-stable: interfaces and their properties are name-sorted, so the
same manifest always yields identical bytes regardless of `$defs` iteration
order. The generator appends a trailing newline so the committed artifact is
POSIX-friendly and matches the drift gate's byte-exact comparison.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

# Repo-relative default manifest, resolved from this file's location
# (<repo>/core/timeline/tools/schema_ts_emit.py). The subsystem-local generator
# lives beside the JSON emitter; the committed manifest is its input.
_THIS = Path(__file__).resolve()
_REPO_ROOT = _THIS.parents[3]
DEFAULT_MANIFEST = _REPO_ROOT / "core" / "timeline" / "schema" / "timeline_schema.json"

_INDENT = "  "
_VALID_TS_IDENT = re.compile(r"^[A-Za-z_$][A-Za-z0-9_$]*$")

# SchemaValueKind (carried losslessly as `x-pulp-kind`) -> TypeScript type. The
# 64-bit-in-string kinds and U32 project to `number | string`: the wire form is
# a JSON string for the 64-bit kinds (to survive JS number precision) while a
# richer runtime may hand back a number, so the union accepts both. Object and
# Array without a `$ref` have no element schema in the manifest, so they widen to
# the safest structural type. A `$ref` on a field overrides the kind mapping
# with the referenced interface (see `_field_type`).
_KIND_TO_TS = {
    "Boolean": "boolean",
    "U32": "number | string",
    "I64String": "number | string",
    "U64String": "number | string",
    "String": "string",
    "Object": "Record<string, unknown>",
    "Array": "readonly unknown[]",
}


class ManifestError(RuntimeError):
    """Raised when the manifest is missing required structure."""


def _interface_name(schema_name: str) -> str:
    """`pulp.timeline.content.media` -> `PulpTimelineContentMedia`.

    Splits on the schema-name separators (`.` and `_`) and PascalCases each
    segment, yielding a stable, valid TS identifier.
    """
    segments = re.split(r"[._]", schema_name)
    name = "".join(segment[:1].upper() + segment[1:] for segment in segments if segment)
    if not name:
        raise ManifestError(f"schema type name projects to an empty TS name: {schema_name!r}")
    return name


def _property_name(field_name: str) -> str:
    """A valid TS identifier is emitted bare; anything else is quoted."""
    if _VALID_TS_IDENT.match(field_name):
        return field_name
    return json.dumps(field_name)


def _ref_target(ref: str) -> str:
    """`#/$defs/pulp.timeline.track` -> `pulp.timeline.track`."""
    prefix = "#/$defs/"
    if not ref.startswith(prefix):
        raise ManifestError(f"unsupported $ref form: {ref!r}")
    return ref[len(prefix):]


def _field_type(field: dict) -> str:
    """Project one field schema to a TS type string.

    A `$ref` names another registered type, so it wins over the raw kind: an
    `Object` field with a `$ref` is the referenced interface; an `Array` field
    with a `$ref` is an array of it.
    """
    kind = field.get("x-pulp-kind")
    if kind is None:
        raise ManifestError(f"field is missing x-pulp-kind: {field!r}")
    ref = field.get("$ref")
    if ref is not None:
        target = _interface_name(_ref_target(ref))
        return f"readonly {target}[]" if kind == "Array" else target
    ts = _KIND_TO_TS.get(kind)
    if ts is None:
        raise ManifestError(f"unmapped x-pulp-kind: {kind!r}")
    return ts


def _emit_interface(schema_name: str, type_def: dict) -> str:
    iface = _interface_name(schema_name)
    domain = type_def.get("x-pulp-domain", "Document")
    version = type_def.get("x-pulp-current-version", 1)
    properties = type_def.get("properties", {})
    required = set(type_def.get("required", []))

    lines = [f"/** `{schema_name}` — domain {domain}, schema version {version}. */"]
    if not properties:
        # Empty schema type (e.g. an empty content payload): a marker interface.
        lines.append(f"export interface {iface} {{}}")
        return "\n".join(lines)

    lines.append(f"export interface {iface} {{")
    for field_name in sorted(properties):
        optional = "" if field_name in required else "?"
        prop = _property_name(field_name)
        ts_type = _field_type(properties[field_name])
        lines.append(f"{_INDENT}{prop}{optional}: {ts_type};")
    lines.append("}")
    return "\n".join(lines)


def generate(manifest: dict) -> str:
    defs = manifest.get("$defs")
    if not isinstance(defs, dict):
        raise ManifestError("manifest has no $defs object")
    if not defs:
        raise ManifestError("manifest $defs is empty")

    manifest_version = manifest.get("x-pulp-manifest-version", 1)
    schema_names = sorted(defs)

    # Guard the PascalCase projection against collisions so two distinct schema
    # names can never share one interface (the drift gate would otherwise hide a
    # silently-dropped type behind a stable byte count).
    seen: dict[str, str] = {}
    for name in schema_names:
        iface = _interface_name(name)
        if iface in seen:
            raise ManifestError(
                f"TS interface-name collision: {name!r} and {seen[iface]!r} both -> {iface!r}"
            )
        seen[iface] = name

    header = "\n".join(
        [
            "// Generated by schema_ts_emit.py from core/timeline/schema/timeline_schema.json.",
            "// Do not edit by hand: this file is a projection of the timeline schema manifest,",
            "// regenerated and drift-checked in CI. Change the SchemaRegistry (and regenerate",
            "// the manifest) instead of editing this file.",
            f"// Manifest version: {manifest_version}",
        ]
    )
    blocks: list[str] = [header]

    for name in schema_names:
        blocks.append(_emit_interface(name, defs[name]))

    # A string-literal union of every registered schema-type name.
    union_lines = ["/** Every schema-type name registered in the timeline manifest. */"]
    union_lines.append("export type TimelineSchemaTypeName =")
    for i, name in enumerate(schema_names):
        terminator = ";" if i == len(schema_names) - 1 else ""
        union_lines.append(f'{_INDENT}| {json.dumps(name)}{terminator}')
    blocks.append("\n".join(union_lines))

    # A map from each schema-type name to its generated interface.
    map_lines = ["/** Maps each schema-type name to its generated interface. */"]
    map_lines.append("export interface TimelineSchemaTypeMap {")
    for name in schema_names:
        map_lines.append(f"{_INDENT}{json.dumps(name)}: {_interface_name(name)};")
    map_lines.append("}")
    blocks.append("\n".join(map_lines))

    # Blank line between blocks; single trailing newline at EOF.
    return "\n\n".join(blocks) + "\n"


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
        help="write the .d.ts here instead of stdout",
    )
    args = parser.parse_args(argv)

    try:
        manifest = _load_manifest(args.manifest)
        text = generate(manifest)
    except ManifestError as exc:
        print(f"schema-ts-emit: {exc}", file=sys.stderr)
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

#!/usr/bin/env python3
"""Generate CLI verb definitions from the timeline schema manifest.

Thin, deterministic projection: reads the canonical JSON-Schema manifest
(core/timeline/schema/timeline_schema.json) and emits a JSON verb table — one
verb per registered schema type, each carrying the type's domain, version, and a
flag per field (its CLI value type derived from the lossless `x-pulp-kind`). This
is a pure function of the manifest — it never touches the SchemaRegistry — so the
CLI surface is a projection of the JSON manifest, not a second source of truth.

    schema_cli_emit.py                 # emit to stdout from the default manifest
    schema_cli_emit.py --manifest <f>  # read a specific manifest
    schema_cli_emit.py --out <f>       # write to a file instead of stdout

The artifact is the manifest-derived *definition* of the timeline CLI verbs; how
the `pulp` CLI mounts and executes them is a separate downstream integration. The
committed file is guarded by its own drift gate (the shared
tools/scripts/schema_drift_check.py, wired as the `timeline-schema-cli-drift`
ctest): it must match a fresh emission from the manifest, so a manifest change
that is not re-projected here is caught.

Output is byte-stable: verbs and their flags are name-sorted and all object keys
are emitted sorted, so the same manifest always yields identical bytes regardless
of `$defs` iteration order. A trailing newline is appended so the committed
artifact is POSIX-friendly and matches the drift gate's byte-exact comparison.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

# Repo-relative default manifest, resolved from this file's location
# (<repo>/core/timeline/tools/schema_cli_emit.py). The subsystem-local generator
# lives beside the JSON and TS emitters; the committed manifest is its input.
_THIS = Path(__file__).resolve()
_REPO_ROOT = _THIS.parents[3]
DEFAULT_MANIFEST = _REPO_ROOT / "core" / "timeline" / "schema" / "timeline_schema.json"

GENERATOR_ID = "schema-cli-emit"

# SchemaValueKind (carried losslessly as `x-pulp-kind`) -> CLI flag value type.
# `bool` is a switch; the 64-bit-in-string kinds keep their signedness (`int` vs
# `uint`) even though the wire form is a string; `Object`/`Array` are passed as
# structured `json`. A field `$ref` is also structured `json`, and the referenced
# schema type is recorded alongside so the CLI can validate the payload.
_KIND_TO_CLI = {
    "Boolean": "bool",
    "U32": "uint",
    "I64String": "int",
    "U64String": "uint",
    "String": "string",
    "Object": "json",
    "Array": "json",
}


class ManifestError(RuntimeError):
    """Raised when the manifest is missing required structure."""


def _verb_name(schema_name: str) -> str:
    """`pulp.timeline.automation_target.device_parameter` -> `timeline:automation-target:device-parameter`.

    Drops the `pulp.` vendor prefix, joins the dotted hierarchy segments with `:`,
    and kebab-cases the snake_case within each segment. The two separators are
    distinct (`:` for hierarchy, `-` for word breaks), so the projection stays
    collision-free — the caller still guards it explicitly.
    """
    name = schema_name
    if name.startswith("pulp."):
        name = name[len("pulp."):]
    segments = [seg.replace("_", "-") for seg in name.split(".") if seg]
    if not segments:
        raise ManifestError(f"schema type name projects to an empty verb: {schema_name!r}")
    return ":".join(segments)


def _flag_name(field_name: str) -> str:
    """`content_hash` -> `content-hash` (kebab-case CLI flag, no leading dashes)."""
    return field_name.replace("_", "-")


def _ref_target(ref: str) -> str:
    """`#/$defs/pulp.timeline.track` -> `pulp.timeline.track`."""
    prefix = "#/$defs/"
    if not ref.startswith(prefix):
        raise ManifestError(f"unsupported $ref form: {ref!r}")
    return ref[len(prefix):]


def _flag(field_name: str, field: dict, required: set[str]) -> dict:
    kind = field.get("x-pulp-kind")
    if kind is None:
        raise ManifestError(f"field is missing x-pulp-kind: {field!r}")
    value_type = _KIND_TO_CLI.get(kind)
    if value_type is None:
        raise ManifestError(f"unmapped x-pulp-kind: {kind!r}")
    flag = {
        "name": _flag_name(field_name),
        "schema_field": field_name,
        "kind": kind,
        "value_type": value_type,
        "required": field_name in required,
    }
    ref = field.get("$ref")
    if ref is not None:
        # A referenced field is a structured payload validated against the
        # referenced schema type; carry the target so the CLI can enforce it.
        flag["value_type"] = "json"
        flag["ref"] = _ref_target(ref)
    return flag


def generate(manifest: dict) -> str:
    defs = manifest.get("$defs")
    if not isinstance(defs, dict):
        raise ManifestError("manifest has no $defs object")
    if not defs:
        raise ManifestError("manifest $defs is empty")

    manifest_version = manifest.get("x-pulp-manifest-version", 1)

    # Guard the verb-token projection against collisions so two distinct schema
    # names can never share one verb (the drift gate would otherwise hide a
    # silently-dropped type behind stable bytes).
    seen: dict[str, str] = {}
    verbs = []
    for schema_name in sorted(defs):
        verb = _verb_name(schema_name)
        if verb in seen:
            raise ManifestError(
                f"CLI verb collision: {schema_name!r} and {seen[verb]!r} both -> {verb!r}"
            )
        seen[verb] = schema_name

        type_def = defs[schema_name]
        required = set(type_def.get("required", []))
        properties = type_def.get("properties", {})
        flags = [_flag(field_name, properties[field_name], required) for field_name in properties]
        # Sort by the emitted flag name (not the raw field name): the kebab-case
        # transform can reorder relative to the snake_case source, and the
        # byte-stability contract is that the *output* is name-sorted.
        flags.sort(key=lambda f: f["name"])
        verbs.append(
            {
                "name": verb,
                "schema_type": schema_name,
                "domain": type_def.get("x-pulp-domain", "Document"),
                "version": type_def.get("x-pulp-current-version", 1),
                "flags": flags,
            }
        )
    # Likewise order verbs by their emitted token, not the source schema name.
    verbs.sort(key=lambda v: v["name"])

    document = {
        "x-pulp-generator": GENERATOR_ID,
        "x-pulp-manifest-version": manifest_version,
        "verbs": verbs,
    }
    # sort_keys makes every object's key order deterministic; the verb and flag
    # arrays are already name-sorted above. A trailing newline keeps the file
    # POSIX-friendly and byte-stable.
    return json.dumps(document, indent=2, sort_keys=True, ensure_ascii=False) + "\n"


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
        help="write the verb JSON here instead of stdout",
    )
    args = parser.parse_args(argv)

    try:
        manifest = _load_manifest(args.manifest)
        text = generate(manifest)
    except ManifestError as exc:
        print(f"schema-cli-emit: {exc}", file=sys.stderr)
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

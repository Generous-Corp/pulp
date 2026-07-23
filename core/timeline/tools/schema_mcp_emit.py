#!/usr/bin/env python3
"""Generate timeline MCP tool definitions from the schema manifest.

This is a deterministic projection of
core/timeline/schema/timeline_schema.json into the fixed five-operation MCP
surface. The operation set is an engine API decision; its drift-sensitive type
vocabularies come from the manifest:

  * project_open lists every Document-domain type;
  * command_apply constrains its envelope to Command-domain types;
  * validate lists every Diagnostic-domain type.

The generator reads the committed manifest rather than linking the timeline
library, preserving the repository's registry -> manifest -> surface ownership
chain. Output is canonical compact JSON with a trailing newline.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

_THIS = Path(__file__).resolve()
_REPO_ROOT = _THIS.parents[3]
DEFAULT_MANIFEST = _REPO_ROOT / "core" / "timeline" / "schema" / "timeline_schema.json"

GENERATOR_ID = "schema-mcp-emit"
TOOL_DEFS_VERSION = 1


class ManifestError(RuntimeError):
    """Raised when the manifest cannot produce a valid MCP projection."""


def _domain_type_names(defs: dict, domain: str) -> list[str]:
    names: list[str] = []
    for schema_name, type_def in defs.items():
        if not isinstance(schema_name, str) or not isinstance(type_def, dict):
            raise ManifestError("manifest $defs must map string names to objects")
        type_domain = type_def.get("x-pulp-domain", "Document")
        if not isinstance(type_domain, str):
            raise ManifestError(f"{schema_name!r} has a non-string x-pulp-domain")
        if type_domain == domain:
            names.append(schema_name)
    return sorted(names)


def _project_property() -> dict:
    return {
        "type": "string",
        "description": "Path to a timeline project document, or the inline project JSON.",
    }


def _sample_rate_property() -> dict:
    return {
        "type": "integer",
        "minimum": 1,
        "maximum": 4_294_967_295,
        "description": "Render sample rate in Hz. Defaults to 48000.",
    }


def _command_envelope(command_types: list[str]) -> dict:
    type_name_schema = {"type": "string", "not": {}}
    if command_types:
        type_name_schema = {"type": "string", "enum": command_types}
    return {
        "type": "object",
        "properties": {
            "type_name": type_name_schema,
            "version": {"type": "integer", "minimum": 1},
            "data": {"type": "object"},
        },
        "required": ["data", "type_name", "version"],
        "additionalProperties": False,
    }


def _input_schema(properties: dict, required: list[str]) -> dict:
    return {
        "type": "object",
        "properties": properties,
        "required": required,
        "additionalProperties": False,
    }


def generate(manifest: dict) -> str:
    defs = manifest.get("$defs")
    if not isinstance(defs, dict):
        raise ManifestError("manifest has no $defs object")
    if not defs:
        raise ManifestError("manifest $defs is empty")

    manifest_version = manifest.get("x-pulp-manifest-version", 1)
    if not isinstance(manifest_version, int) or isinstance(manifest_version, bool):
        raise ManifestError("manifest version must be an integer")

    command_types = _domain_type_names(defs, "Command")
    diagnostic_types = _domain_type_names(defs, "Diagnostic")
    document_types = _domain_type_names(defs, "Document")

    tools = [
        {
            "name": "pulp_timeline_project_open",
            "description": "Open a timeline project document and return its parsed, validated state.",
            "inputSchema": _input_schema({"project": _project_property()}, ["project"]),
            "x-pulp-operation": "project.open",
            "x-pulp-document-types": document_types,
        },
        {
            "name": "pulp_timeline_command_apply",
            "description": (
                "Apply an ordered list of typed timeline commands to a project. "
                "The command vocabulary is the API; each command is a manifest-derived envelope."
            ),
            "inputSchema": _input_schema(
                {
                    "project": _project_property(),
                    "commands": {
                        "type": "array",
                        "minItems": 1,
                        "items": _command_envelope(command_types),
                    },
                },
                ["commands", "project"],
            ),
            "x-pulp-operation": "command.apply",
            "x-pulp-command-types": command_types,
        },
        {
            "name": "pulp_timeline_validate",
            "description": "Validate a project and return structured diagnostics.",
            "inputSchema": _input_schema({"project": _project_property()}, ["project"]),
            "x-pulp-operation": "project.validate",
            "x-pulp-diagnostic-types": diagnostic_types,
        },
        {
            "name": "pulp_timeline_explain",
            "description": (
                "Compile a project and dump the playback program "
                "(renderers, cursors, arbitration, PDC offsets) as structured JSON."
            ),
            "inputSchema": _input_schema(
                {
                    "project": _project_property(),
                    "sample_rate": _sample_rate_property(),
                },
                ["project"],
            ),
            "x-pulp-operation": "project.explain",
        },
        {
            "name": "pulp_timeline_render",
            "description": "Offline-render a project to an audio file.",
            "inputSchema": _input_schema(
                {
                    "project": _project_property(),
                    "output": {
                        "type": "string",
                        "description": "Destination audio file path.",
                    },
                    "sample_rate": _sample_rate_property(),
                },
                ["output", "project"],
            ),
            "x-pulp-operation": "project.render",
        },
    ]

    document = {
        "x-pulp-generator": GENERATOR_ID,
        "x-pulp-manifest-version": TOOL_DEFS_VERSION,
        "x-pulp-schema-manifest-version": manifest_version,
        "x-pulp-source": "timeline_schema.json",
        "tools": tools,
    }
    return json.dumps(document, sort_keys=True, separators=(",", ":"), ensure_ascii=False) + "\n"


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
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--out", type=Path, default=None)
    args = parser.parse_args(argv)

    try:
        text = generate(_load_manifest(args.manifest))
    except ManifestError as exc:
        print(f"schema-mcp-emit: {exc}", file=sys.stderr)
        return 2

    data = text.encode("utf-8")
    if args.out is None:
        sys.stdout.buffer.write(data)
    else:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_bytes(data)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

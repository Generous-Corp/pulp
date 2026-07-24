#!/usr/bin/env python3
"""Tests for schema_mcp_emit.py and its shared drift gate."""

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
_GENERATOR = _TOOLS_DIR / "schema_mcp_emit.py"
_MANIFEST = _REPO_ROOT / "core" / "timeline" / "schema" / "timeline_schema.json"
_ARTIFACT = _REPO_ROOT / "core" / "timeline" / "schema" / "timeline_mcp_tools.json"
_DRIFT_CHECK = _REPO_ROOT / "tools" / "scripts" / "schema_drift_check.py"


def _load_generator():
    spec = importlib.util.spec_from_file_location("schema_mcp_emit", _GENERATOR)
    module = importlib.util.module_from_spec(spec)
    assert spec and spec.loader
    spec.loader.exec_module(module)
    return module


gen = _load_generator()


def _run_gate(artifact: Path) -> int:
    emit_cmd = " ".join(
        shlex.quote(part)
        for part in [sys.executable, str(_GENERATOR), "--manifest", str(_MANIFEST)]
    )
    return subprocess.run(
        [
            sys.executable,
            str(_DRIFT_CHECK),
            "--artifact",
            str(artifact),
            "--emit-cmd",
            emit_cmd,
        ],
        capture_output=True,
    ).returncode


def _tool(document: dict, name: str) -> dict:
    return next(tool for tool in document["tools"] if tool["name"] == name)


def main() -> int:
    failures: list[str] = []

    def check(name: str, condition: bool) -> None:
        print(f"{'ok' if condition else 'not ok'} - {name}")
        if not condition:
            failures.append(name)

    manifest = json.loads(_MANIFEST.read_bytes())
    first = gen.generate(manifest)
    second = gen.generate(manifest)
    committed = _ARTIFACT.read_text()
    document = json.loads(first)

    check("two emissions are byte-identical", first == second)
    check("emission ends with one newline", first.endswith("\n") and not first.endswith("\n\n"))
    check("committed artifact matches a fresh emission", committed == first)

    expected_names = [
        "pulp_timeline_project_open",
        "pulp_timeline_command_apply",
        "pulp_timeline_validate",
        "pulp_timeline_explain",
        "pulp_timeline_render",
    ]
    check("surface has exactly the five operations", [tool["name"] for tool in document["tools"]] == expected_names)
    check(
        "every operation carries a closed object input schema",
        all(
            tool["inputSchema"]["type"] == "object"
            and tool["inputSchema"]["additionalProperties"] is False
            for tool in document["tools"]
        ),
    )
    explain_rate = _tool(document, "pulp_timeline_explain")["inputSchema"]["properties"][
        "sample_rate"
    ]
    render_rate = _tool(document, "pulp_timeline_render")["inputSchema"]["properties"][
        "sample_rate"
    ]
    check(
        "explain and render expose the same supported sample-rate override",
        explain_rate == render_rate
        and explain_rate["minimum"] == 1
        and explain_rate["maximum"] == gen.MAX_COMPILED_SAMPLE_RATE,
    )

    defs = manifest["$defs"]
    expected_documents = sorted(
        name
        for name, type_def in defs.items()
        if type_def.get("x-pulp-domain", "Document") == "Document"
    )
    project_open = _tool(document, "pulp_timeline_project_open")
    check(
        "project-open vocabulary contains every document type",
        project_open["x-pulp-document-types"] == expected_documents,
    )

    command_apply = _tool(document, "pulp_timeline_command_apply")
    command_name_schema = command_apply["inputSchema"]["properties"]["commands"]["items"]["properties"]["type_name"]
    expected_commands = sorted(
        name
        for name, type_def in defs.items()
        if type_def.get("x-pulp-domain", "Document") == "Command"
    )
    expected_command_schema = (
        {"type": "string", "enum": expected_commands}
        if expected_commands
        else {"type": "string", "not": {}}
    )
    check(
        "command vocabulary contains every command type",
        command_apply["x-pulp-command-types"] == expected_commands
        and command_name_schema == expected_command_schema,
    )
    check(
        "command versions exclude the invalid zero identity",
        command_apply["inputSchema"]["properties"]["commands"]["items"]["properties"]["version"]["minimum"]
        == 1,
    )
    check(
        "command batches exclude empty transactions",
        command_apply["inputSchema"]["properties"]["commands"]["minItems"] == 1,
    )

    no_commands = json.loads(
        gen.generate(
            {
                "x-pulp-manifest-version": 1,
                "$defs": {
                    "pulp.test.document": {
                        "x-pulp-domain": "Document",
                        "properties": {},
                    }
                },
            }
        )
    )
    no_command_apply = _tool(no_commands, "pulp_timeline_command_apply")
    no_command_schema = no_command_apply["inputSchema"]["properties"]["commands"]["items"]["properties"]["type_name"]
    check(
        "empty command vocabulary uses an MCP-compatible reject-all schema",
        no_command_schema == {"type": "string", "not": {}},
    )
    check(
        "command-envelope property schemas are object-valued",
        all(
            isinstance(schema, dict)
            for schema in no_command_apply["inputSchema"]["properties"]["commands"]["items"]["properties"].values()
        ),
    )

    synthetic = {
        "x-pulp-manifest-version": 7,
        "$defs": {
            "pulp.test.document": {"x-pulp-domain": "Document", "properties": {}},
            "pulp.test.command.z": {"x-pulp-domain": "Command", "properties": {}},
            "pulp.test.command.a": {"x-pulp-domain": "Command", "properties": {}},
            "pulp.test.diagnostic": {"x-pulp-domain": "Diagnostic", "properties": {}},
        },
    }
    grown = json.loads(gen.generate(synthetic))
    grown_apply = _tool(grown, "pulp_timeline_command_apply")
    grown_enum = grown_apply["inputSchema"]["properties"]["commands"]["items"]["properties"]["type_name"]["enum"]
    check(
        "command vocabulary is manifest-derived and sorted",
        grown_enum == ["pulp.test.command.a", "pulp.test.command.z"]
        and grown_apply["x-pulp-command-types"] == grown_enum,
    )
    check(
        "diagnostic vocabulary is manifest-derived",
        _tool(grown, "pulp_timeline_validate")["x-pulp-diagnostic-types"]
        == ["pulp.test.diagnostic"],
    )
    check(
        "document vocabulary is manifest-derived",
        _tool(grown, "pulp_timeline_project_open")["x-pulp-document-types"]
        == ["pulp.test.document"],
    )
    check("source manifest version is carried", grown["x-pulp-schema-manifest-version"] == 7)

    check("gate passes for the committed artifact", _run_gate(_ARTIFACT) == 0)
    with tempfile.TemporaryDirectory() as tmp:
        mutated = Path(tmp) / "timeline_mcp_tools.json"
        mutated.write_text(committed + "drift\n")
        check("gate fails for a mutated artifact", _run_gate(mutated) == 1)

    try:
        gen.generate({"$defs": {}})
    except gen.ManifestError:
        empty_failed = True
    else:
        empty_failed = False
    check("empty manifest fails closed", empty_failed)

    if failures:
        print(f"\nFAILED: {len(failures)} case(s): {', '.join(failures)}", file=sys.stderr)
        return 1
    print("\nall schema_mcp_emit cases passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

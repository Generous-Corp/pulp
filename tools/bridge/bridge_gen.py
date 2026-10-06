#!/usr/bin/env python3
"""Generate the typed EditorBridge contract from one deterministic TOML file.

The generator deliberately covers the part of the bridge contract that is
already stable in Pulp: scalar request/response fields, command names and
publication frame names.  Handler bodies remain hand-written and are
registered with ``EditorBridge::add_handler``.  This keeps the first adoption
slice useful without pretending that every existing plugin message has the
same payload shape.

``bridge.toml`` is the source of truth.  Generated C++, TypeScript and
documentation are checked in beside it for this seed contract.  Consumers can
pass explicit output paths when generating a plugin-local contract.
"""

from __future__ import annotations

import argparse
import difflib
import re
import sys
import tomllib
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "tools/bridge/bridge.toml"
OUTPUTS = {
    "cpp": ROOT / "tools/bridge/generated_editor_bridge.hpp",
    "ts": ROOT / "tools/bridge/generated_editor_bridge.ts",
    "docs": ROOT / "docs/reference/generated-editor-bridge-contract.md",
}
SCALARS = {"string": "std::string", "number": "double", "boolean": "bool"}
TS_SCALARS = {"string": "string", "number": "number", "boolean": "boolean"}
IDENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def _require_identifier(value: Any, label: str) -> str:
    if not isinstance(value, str) or IDENT.fullmatch(value) is None:
        raise ValueError(f"{label} must be an identifier")
    return value


def _field_list(value: Any, label: str) -> list[dict[str, str]]:
    if value is None:
        return []
    if not isinstance(value, list):
        raise ValueError(f"{label} must be an array")
    result: list[dict[str, str]] = []
    names: set[str] = set()
    for index, field in enumerate(value):
        if not isinstance(field, dict):
            raise ValueError(f"{label}[{index}] must be a table")
        name = _require_identifier(field.get("name"), f"{label}[{index}].name")
        if name in names:
            raise ValueError(f"duplicate field {label}.{name}")
        names.add(name)
        field_type = field.get("type")
        if field_type not in SCALARS:
            raise ValueError(f"unsupported field type in {label}.{name}: {field_type}")
        result.append({"name": name, "type": str(field_type)})
    return result


def _rows(value: Any, section: str, *, fields_key: str) -> list[dict[str, Any]]:
    if value is None:
        return []
    if not isinstance(value, list):
        raise ValueError(f"{section} must be an array")
    result: list[dict[str, Any]] = []
    names: set[str] = set()
    for index, row in enumerate(value):
        if not isinstance(row, dict):
            raise ValueError(f"{section}[{index}] must be a table")
        name = _require_identifier(row.get("name"), f"{section}[{index}].name")
        if name in names:
            raise ValueError(f"duplicate {section} name: {name}")
        names.add(name)
        fields = row.get(fields_key, row.get("fields", []))
        parsed: dict[str, Any] = {"name": name}
        parsed[fields_key] = _field_list(fields, f"{section}.{name}.{fields_key}")
        if section == "commands":
            parsed["response"] = _field_list(row.get("response", []), f"{section}.{name}.response")
        result.append(parsed)
    return sorted(result, key=lambda row: row["name"])


def load_contract(path: Path = SOURCE) -> dict[str, Any]:
    try:
        raw = tomllib.loads(path.read_text(encoding="utf-8"))
    except (OSError, tomllib.TOMLDecodeError) as exc:
        raise ValueError(f"cannot read bridge contract {path}: {exc}") from exc

    if raw.get("version") != 1:
        raise ValueError("bridge contract version must be 1")
    name = _require_identifier(raw.get("name"), "bridge contract name")
    commands = _rows(raw.get("commands", []), "commands", fields_key="request")
    publications = _rows(raw.get("publications", []), "publications", fields_key="fields")
    all_names = [row["name"] for row in commands + publications]
    if len(all_names) != len(set(all_names)):
        raise ValueError("command and publication names must be unique")
    if not commands:
        raise ValueError("bridge contract must declare at least one command")
    return {"version": 1, "name": name, "commands": commands, "publications": publications}


def fields(row: dict[str, Any]) -> list[dict[str, str]]:
    return row.get("request", row.get("fields", []))


def type_name(name: str) -> str:
    return "".join(part[:1].upper() + part[1:] for part in name.split("_"))


def camel_name(name: str) -> str:
    parts = name.split("_")
    return parts[0] + "".join(part[:1].upper() + part[1:] for part in parts[1:])


def cpp_struct(name: str, row_fields: list[dict[str, str]]) -> str:
    body = "\n".join(f"    {SCALARS[field['type']]} {field['name']};" for field in row_fields)
    return f"struct {name} {{\n{body or '    // empty payload'}\n}};"


def render_cpp(data: dict[str, Any]) -> str:
    commands = data["commands"]
    publications = data["publications"]
    rows = sorted(
        [(row, "Command") for row in commands] + [(row, "Publication") for row in publications],
        key=lambda item: item[0]["name"],
    )
    structs: list[str] = []
    for row in commands:
        title = type_name(row["name"])
        structs.append(cpp_struct(f"{title}Request", row["request"]))
        structs.append(cpp_struct(f"{title}Response", row["response"]))
    for row in publications:
        structs.append(cpp_struct(f"{type_name(row['name'])}Frame", row["fields"]))

    entries = []
    for row, direction in rows:
        title = type_name(row["name"])
        payload = f"{title}Request" if direction == "Command" else f"{title}Frame"
        response = f"{title}Response" if direction == "Command" else ""
        entries.append(
            f'    {{"{row["name"]}", Direction::{direction}, "{payload}", "{response}"}},'
        )
    command_names = ", ".join(f'"{row["name"]}"' for row in commands)
    publication_names = ", ".join(f'"{row["name"]}"' for row in publications)
    return (
        "// Generated by tools/bridge/bridge_gen.py; do not edit.\n"
        "#pragma once\n\n"
        "#include <array>\n"
        "#include <string>\n"
        "#include <string_view>\n\n"
        "namespace pulp::view::editor_bridge_contract {\n\n"
        f"inline constexpr int kVersion = {data['version']};\n"
        f"inline constexpr std::string_view kName = \"{data['name']}\";\n\n"
        "enum class Direction { Command, Publication };\n"
        "struct HandlerSpec {\n"
        "    std::string_view name;\n"
        "    Direction direction;\n"
        "    std::string_view payload_type;\n"
        "    std::string_view response_type;\n"
        "};\n\n"
        + "\n\n".join(structs)
        + "\n\n"
        + f"inline constexpr std::array<HandlerSpec, {len(entries)}> kHandlers{{{{\n"
        + "\n".join(entries)
        + "\n}};\n\n"
        + f"inline constexpr std::array<std::string_view, {len(commands)}> kCommandNames{{{{{command_names}}}}};\n"
        + f"inline constexpr std::array<std::string_view, {len(publications)}> kPublicationNames{{{{{publication_names}}}}};\n\n"
        + "} // namespace pulp::view::editor_bridge_contract\n"
    )


def ts_interface(name: str, row_fields: list[dict[str, str]]) -> str:
    body = "\n".join(f"  {field['name']}: {TS_SCALARS[field['type']]}" for field in row_fields)
    return f"export interface {name} {{\n{body or '  // empty payload'}\n}}\n"


def render_ts(data: dict[str, Any]) -> str:
    commands = data["commands"]
    publications = data["publications"]
    chunks = ["// Generated by tools/bridge/bridge_gen.py; do not edit.\n"]
    for row in commands:
        title = type_name(row["name"])
        chunks.append(ts_interface(f"{title}Request", row["request"]))
        chunks.append(ts_interface(f"{title}Response", row["response"]))
    for row in publications:
        chunks.append(ts_interface(f"{type_name(row['name'])}Frame", row["fields"]))

    command_union = " | ".join(
        f'{{ type: "{row["name"]}"; payload: {type_name(row["name"])}Request }}' for row in commands
    )
    publication_union = " | ".join(f'"{row["name"]}"' for row in publications) or "never"
    command_literals = " | ".join(f'"{row["name"]}"' for row in commands)
    chunks.extend(
        [
            f"export type EditorBridgeCommand = {command_literals};\n",
            f"export type EditorBridgePublication = {publication_union};\n",
            f"export type EditorBridgeRequest = {command_union};\n",
            "export type EditorBridgeResponse<T extends object> = { ok: boolean } & Partial<T>;\n",
            "export type EditorBridgeResult<T extends object> = EditorBridgeResponse<T> | Promise<EditorBridgeResponse<T>>;\n",
            "export type EditorBridgeTransport = (request: EditorBridgeRequest) => unknown | Promise<unknown>;\n",
        ]
    )
    for row in commands:
        title = type_name(row["name"])
        args = ", ".join(f"{field['name']}: {TS_SCALARS[field['type']]}" for field in row["request"])
        payload = ", ".join(f"{field['name']}: {field['name']}" for field in row["request"])
        chunks.append(
            f"export function {camel_name(row['name'])}(transport: EditorBridgeTransport, {args}): EditorBridgeResult<{title}Response> {{\n"
            f"  return transport({{ type: \"{row['name']}\", payload: {{ {payload} }} }}) as EditorBridgeResult<{title}Response>;\n"
            "}\n"
        )
    return "\n".join(chunks)


def payload_text(row: dict[str, Any], key: str) -> str:
    row_fields = row.get(key, [])
    return ", ".join(f"`{field['name']}: {field['type']}`" for field in row_fields) or "(empty)"


def render_docs(data: dict[str, Any]) -> str:
    lines = [
        "<!-- Generated by tools/bridge/bridge_gen.py; do not edit. -->",
        "",
        f"# {data['name'].title()} bridge contract",
        "",
        "The TOML contract is the source of truth. Handler bodies remain registered with `EditorBridge::add_handler`; the generated table makes names and scalar payload shapes reviewable and deterministic.",
        "",
        "## Commands",
        "",
        "| Name | Request | Response |",
        "| --- | --- | --- |",
    ]
    for row in data["commands"]:
        lines.append(f"| `{row['name']}` | {payload_text(row, 'request')} | {payload_text(row, 'response')} |")
    lines += ["", "## Publications", "", "| Name | Frame |", "| --- | --- |"]
    for row in data["publications"]:
        lines.append(f"| `{row['name']}` | {payload_text(row, 'fields')} |")
    lines += [
        "",
        "`EditorBridge::handlers()` returns inbound command names in lexicographic order. A parity test compares that snapshot with generated `kCommandNames`; publications are outbound frames and are intentionally not registered as inbound handlers.",
        "",
        "Run `python3 tools/bridge/bridge_gen.py --check` to reject generated drift.",
        "",
    ]
    return "\n".join(lines)


def render(data: dict[str, Any], outputs: dict[str, Path] | None = None) -> dict[Path, str]:
    paths = outputs or OUTPUTS
    return {
        paths["cpp"]: render_cpp(data),
        paths["ts"]: render_ts(data),
        paths["docs"]: render_docs(data),
    }


def check(outputs: dict[Path, str]) -> bool:
    ok = True
    for path, expected in outputs.items():
        actual = path.read_text(encoding="utf-8") if path.exists() else ""
        if actual != expected:
            ok = False
            sys.stderr.writelines(
                difflib.unified_diff(
                    actual.splitlines(True),
                    expected.splitlines(True),
                    fromfile=str(path),
                    tofile=f"{path} (generated)",
                )
            )
    return ok


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="fail when checked-in output differs")
    parser.add_argument("--source", type=Path, default=SOURCE)
    parser.add_argument("--cpp", type=Path, default=OUTPUTS["cpp"])
    parser.add_argument("--ts", type=Path, default=OUTPUTS["ts"])
    parser.add_argument("--docs", type=Path, default=OUTPUTS["docs"])
    args = parser.parse_args(argv)
    outputs = {"cpp": args.cpp, "ts": args.ts, "docs": args.docs}
    try:
        generated = render(load_contract(args.source), outputs)
        if args.check:
            return 0 if check(generated) else 1
        for path, content in generated.items():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")
    except ValueError as exc:
        parser.error(str(exc))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

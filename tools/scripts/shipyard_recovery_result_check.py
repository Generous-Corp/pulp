#!/usr/bin/env python3
"""Validate a model-produced recovery result against its committed schema.

The Codex lane gets schema enforcement for free from ``codex exec
--output-schema``. The Claude fallback lane emits its structured object through
``claude --json-schema``, which validates upstream but leaves no trusted local
proof, so the worker re-checks the payload here before any downstream step reads
it.

This module deliberately implements the small schema subset it needs instead of
importing a shared helper. Every path under ``tools/scripts/shipyard_recovery_``
is inside the recovery fence in ``shipyard_recovery_repair.py``, so a fenced
repair model can never edit this file. A validator living outside that fence
could be weakened by the very model it constrains.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any


class ResultError(ValueError):
    """Raised when a payload does not satisfy its schema."""


def _fail(message: str) -> None:
    raise ResultError(message)


def _check_string(value: Any, schema: dict[str, Any], where: str) -> None:
    if not isinstance(value, str):
        _fail(f"{where} must be a string")
    enum = schema.get("enum")
    if enum is not None and value not in enum:
        _fail(f"{where} must be one of {sorted(enum)}")
    minimum = schema.get("minLength")
    if minimum is not None and len(value) < minimum:
        _fail(f"{where} must be at least {minimum} characters")
    maximum = schema.get("maxLength")
    if maximum is not None and len(value) > maximum:
        _fail(f"{where} must be at most {maximum} characters")


def _check_array(value: Any, schema: dict[str, Any], where: str) -> None:
    if not isinstance(value, list):
        _fail(f"{where} must be an array")
    maximum = schema.get("maxItems")
    if maximum is not None and len(value) > maximum:
        _fail(f"{where} must have at most {maximum} items")
    items = schema.get("items")
    if isinstance(items, dict):
        for index, item in enumerate(value):
            _check_value(item, items, f"{where}[{index}]")


def _check_value(value: Any, schema: dict[str, Any], where: str) -> None:
    declared = schema.get("type")
    if declared == "string":
        _check_string(value, schema, where)
    elif declared == "array":
        _check_array(value, schema, where)
    elif declared == "object":
        validate(value, schema, where)
    elif declared is not None:
        _fail(f"{where} has unsupported schema type {declared!r}")


def validate(payload: Any, schema: dict[str, Any], where: str = "result") -> None:
    """Validate ``payload`` against the supported JSON Schema subset."""
    if not isinstance(payload, dict):
        _fail(f"{where} must be a JSON object")
    properties = schema.get("properties", {})
    for name in schema.get("required", []):
        if name not in payload:
            _fail(f"{where} is missing required field {name!r}")
    if schema.get("additionalProperties") is False:
        for name in payload:
            if name not in properties:
                _fail(f"{where} has unexpected field {name!r}")
    for name, subschema in properties.items():
        if name in payload and isinstance(subschema, dict):
            _check_value(payload[name], subschema, f"{where}.{name}")


def load_structured_output(path: Path) -> Any:
    """Extract the structured object from a Claude ``--output-format json`` run.

    Accepts either the raw structured object (the Codex lane's shape) or the
    Claude envelope, which carries the object in ``structured_output`` and a
    JSON-encoded copy in ``result``.
    """
    with path.open(encoding="utf-8") as handle:
        document = json.load(handle)
    if not isinstance(document, dict):
        _fail("model output must be a JSON object")
    if "structured_output" in document:
        structured = document["structured_output"]
        if structured is None:
            _fail("model returned no structured output")
        return structured
    if "result" in document and "type" in document and "usage" in document:
        raw = document["result"]
        if not isinstance(raw, str):
            _fail("model envelope carried a non-string result")
        try:
            return json.loads(raw)
        except json.JSONDecodeError as error:
            _fail(f"model envelope result was not valid JSON: {error}")
    return document


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--schema", required=True, type=Path)
    parser.add_argument("--result", required=True, type=Path)
    parser.add_argument(
        "--output",
        type=Path,
        help="write the extracted structured object here (defaults to --result)",
    )
    arguments = parser.parse_args(argv)

    with arguments.schema.open(encoding="utf-8") as handle:
        schema = json.load(handle)

    payload = load_structured_output(arguments.result)
    validate(payload, schema)

    destination = arguments.output or arguments.result
    destination.write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except ResultError as error:
        print(f"recovery result rejected: {error}", file=sys.stderr)
        sys.exit(1)

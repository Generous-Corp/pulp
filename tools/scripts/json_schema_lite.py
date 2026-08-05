#!/usr/bin/env python3
"""A tiny, dependency-free JSON Schema validator.

Pulp's gate scripts run on PEP-668 Python where `pip install jsonschema`
is not available, so a `$schema` pointer next to a config file has
historically been decoration: editors honour it, nothing in CI does.

This module validates a document against the subset of JSON Schema that
Pulp's own config schemas use. The subset is deliberate, and the
important property is how it fails: a keyword this validator does not
implement raises `UnsupportedKeyword` instead of being skipped. A schema
author therefore cannot silently write a constraint that is never
checked — the schema either validates for real or the run errors out.

Supported keywords:
    type, const, enum, required, properties, additionalProperties,
    propertyNames, minProperties, maxProperties, items, minItems,
    maxItems, uniqueItems, minLength, maxLength, pattern

Ignored (annotation-only) keywords:
    $schema, $id, title, description, $comment, examples, default

`pattern` is compiled with Python `re`. Keep schema patterns inside the
common ECMA/Python subset so an editor's validator and this one agree.
"""

from __future__ import annotations

import re
from typing import Any

_ANNOTATIONS = frozenset(
    {"$schema", "$id", "title", "description", "$comment", "examples", "default"}
)

_SUPPORTED = frozenset(
    {
        "type",
        "const",
        "enum",
        "required",
        "properties",
        "additionalProperties",
        "propertyNames",
        "minProperties",
        "maxProperties",
        "items",
        "minItems",
        "maxItems",
        "uniqueItems",
        "minLength",
        "maxLength",
        "pattern",
    }
)

_TYPES: dict[str, Any] = {
    "object": dict,
    "array": list,
    "string": str,
    "boolean": bool,
    "null": type(None),
}


class UnsupportedKeyword(Exception):
    """A schema used a keyword this validator does not implement."""


def _type_ok(value: Any, name: str) -> bool:
    if name == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if name == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    expected = _TYPES.get(name)
    if expected is None:
        raise UnsupportedKeyword(f"unknown type name: {name!r}")
    if expected is dict or expected is list or expected is str:
        return isinstance(value, expected)
    return isinstance(value, expected)


def validate(document: Any, schema: Any, path: str = "$") -> list[str]:
    """Return a list of human-readable violations (empty means valid).

    Raises `UnsupportedKeyword` if the schema uses a keyword outside the
    documented subset — a loud failure, never a silent skip.
    """
    if schema is True:
        return []
    if schema is False:
        return [f"{path}: schema forbids any value here"]
    if not isinstance(schema, dict):
        raise UnsupportedKeyword(f"{path}: schema must be an object or boolean")

    for key in schema:
        if key in _ANNOTATIONS or key in _SUPPORTED:
            continue
        raise UnsupportedKeyword(
            f"{path}: schema keyword {key!r} is not implemented by "
            "json_schema_lite. Implement it (and test it) or express the "
            "constraint with a supported keyword — do not leave it "
            "unchecked."
        )

    errors: list[str] = []

    if "type" in schema:
        names = schema["type"]
        names = [names] if isinstance(names, str) else list(names)
        if not any(_type_ok(document, n) for n in names):
            errors.append(
                f"{path}: expected type {'/'.join(names)}, got "
                f"{type(document).__name__}"
            )
            # Every remaining keyword assumes the right type.
            return errors

    if "const" in schema and document != schema["const"]:
        errors.append(f"{path}: expected const {schema['const']!r}, got {document!r}")

    if "enum" in schema and document not in schema["enum"]:
        errors.append(f"{path}: {document!r} is not one of {schema['enum']!r}")

    if isinstance(document, str):
        errors.extend(_validate_string(document, schema, path))
    elif isinstance(document, list):
        errors.extend(_validate_array(document, schema, path))
    elif isinstance(document, dict):
        errors.extend(_validate_object(document, schema, path))

    return errors


def _validate_string(document: str, schema: dict, path: str) -> list[str]:
    errors: list[str] = []
    if "minLength" in schema and len(document) < schema["minLength"]:
        errors.append(
            f"{path}: string shorter than minLength {schema['minLength']}"
        )
    if "maxLength" in schema and len(document) > schema["maxLength"]:
        errors.append(f"{path}: string longer than maxLength {schema['maxLength']}")
    if "pattern" in schema and not re.search(schema["pattern"], document):
        errors.append(
            f"{path}: {document!r} does not match pattern {schema['pattern']!r}"
        )
    return errors


def _validate_array(document: list, schema: dict, path: str) -> list[str]:
    errors: list[str] = []
    if "minItems" in schema and len(document) < schema["minItems"]:
        errors.append(f"{path}: fewer than minItems {schema['minItems']}")
    if "maxItems" in schema and len(document) > schema["maxItems"]:
        errors.append(f"{path}: more than maxItems {schema['maxItems']}")
    if schema.get("uniqueItems") and len(
        {repr(i) for i in document}
    ) != len(document):
        errors.append(f"{path}: array items are not unique")
    if "items" in schema:
        for i, item in enumerate(document):
            errors.extend(validate(item, schema["items"], f"{path}[{i}]"))
    return errors


def _validate_object(document: dict, schema: dict, path: str) -> list[str]:
    errors: list[str] = []
    if "minProperties" in schema and len(document) < schema["minProperties"]:
        errors.append(f"{path}: fewer than minProperties {schema['minProperties']}")
    if "maxProperties" in schema and len(document) > schema["maxProperties"]:
        errors.append(f"{path}: more than maxProperties {schema['maxProperties']}")

    for key in schema.get("required", []):
        if key not in document:
            errors.append(f"{path}: missing required property {key!r}")

    props = schema.get("properties", {})
    for key, value in document.items():
        child = f"{path}.{key}"
        if "propertyNames" in schema:
            errors.extend(validate(key, schema["propertyNames"], f"{child} (name)"))
        if key in props:
            errors.extend(validate(value, props[key], child))
            continue
        if "additionalProperties" in schema:
            extra = schema["additionalProperties"]
            if extra is False:
                errors.append(f"{path}: unexpected property {key!r}")
            else:
                errors.extend(validate(value, extra, child))
    return errors

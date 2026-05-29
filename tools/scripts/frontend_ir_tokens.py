#!/usr/bin/env python3
"""Resolve token references for PulpFrontendIR reports from static source contracts."""

from __future__ import annotations

import pathlib
import re
from typing import Any


OBJECT_RE = re.compile(
    r"\b(?:const|let|var)\s+([A-Za-z_$][A-Za-z0-9_$]*)\s*=\s*\{(?P<body>.*?)\}\s*;",
    re.DOTALL,
)
PROPERTY_RE = re.compile(
    r"(?:^|,)\s*(?:([A-Za-z_$][A-Za-z0-9_$]*)|[\"']([^\"']+)[\"'])\s*:\s*([\"'])(.*?)\3\s*(?=,|$)",
    re.DOTALL,
)
TOKEN_REF_RE = re.compile(r"^([A-Za-z_$][A-Za-z0-9_$]*)\.([A-Za-z_$][A-Za-z0-9_$]*)$")
HEX_COLOR_RE = re.compile(r"^#(?:[0-9a-fA-F]{3,4}|[0-9a-fA-F]{6}|[0-9a-fA-F]{8})$")
COLOR_FUNC_RE = re.compile(r"^(?:rgb|rgba|hsl|hsla)\(", re.IGNORECASE)


def local_source_path(source_path: str, repo_root: pathlib.Path) -> pathlib.Path | None:
    if not source_path:
        return None
    candidate = pathlib.Path(source_path)
    if not candidate.is_absolute():
        candidate = repo_root / candidate
    try:
        if candidate.is_file():
            return candidate
    except OSError:
        return None
    return None


def line_for_offset(text: str, offset: int) -> int:
    return text.count("\n", 0, max(offset, 0)) + 1


def infer_resolved_type(value: str) -> str:
    stripped = value.strip()
    if HEX_COLOR_RE.fullmatch(stripped) or COLOR_FUNC_RE.match(stripped):
        return "color"
    return "string"


def source_static_string_tokens(source_text: str) -> dict[str, dict[str, str]]:
    resolved: dict[str, dict[str, str]] = {}
    for object_match in OBJECT_RE.finditer(source_text):
        object_name = object_match.group(1)
        body = object_match.group("body")
        object_start = object_match.start("body")
        for prop_match in PROPERTY_RE.finditer(body):
            prop_name = prop_match.group(1) or prop_match.group(2)
            if not prop_name:
                continue
            value = prop_match.group(4)
            key = f"{object_name}.{prop_name}"
            resolved[key] = {
                "resolved_value": value,
                "resolved_type": infer_resolved_type(value),
                "source_object": object_name,
                "source_property": prop_name,
                "source_line": str(line_for_offset(source_text, object_start + prop_match.start())),
            }
    return resolved


def resolve_source_token_refs(tokens: dict[str, dict[str, Any]], source_path: str, repo_root: pathlib.Path) -> int:
    path = local_source_path(source_path, repo_root)
    if path is None:
        return 0
    try:
        source_text = path.read_text(encoding="utf-8")
    except OSError:
        return 0

    static_tokens = source_static_string_tokens(source_text)
    resolved_count = 0
    for token_key, token in tokens.items():
        if not isinstance(token, dict) or "resolved_value" in token:
            continue
        if TOKEN_REF_RE.fullmatch(token_key) is None:
            continue
        resolved = static_tokens.get(token_key)
        if resolved is None:
            continue
        token["resolved_value"] = resolved["resolved_value"]
        token["resolved_type"] = resolved["resolved_type"]
        source_identity = token.setdefault("source_identity", {})
        if isinstance(source_identity, dict):
            source_identity["resolved_from"] = "source_static_const_object"
            source_identity["source_object"] = resolved["source_object"]
            source_identity["source_property"] = resolved["source_property"]
            source_identity["source_line"] = resolved["source_line"]
        resolved_count += 1
    return resolved_count

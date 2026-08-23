"""Installed Forge Modular public-header dependency closure."""

from __future__ import annotations

import json
import os
import re


_PULP_INCLUDE = re.compile(r'^\s*#\s*include\s*<(pulp/[^>]+)>', re.MULTILINE)


def _includes_in(path: str) -> list[str]:
    with open(path, encoding="utf-8", errors="ignore") as source:
        return _PULP_INCLUDE.findall(source.read())


def _completed_prefix_headers(source: str) -> set[str]:
    headers: set[str] = set()
    for line in source.splitlines():
        include = _PULP_INCLUDE.match(line)
        if include:
            headers.add(include.group(1))
            continue
        if not line.strip() and headers:
            continue
        break
    return headers


def complete_known_public_headers(root: str, source: str) -> tuple[str, list[str]]:
    """Add public headers for curated Pulp symbols used by generated source."""
    registry = os.path.join(root, "tools", "rack", "knowledge", "module",
                            "dsp-primitives.json")
    try:
        with open(registry, encoding="utf-8") as stream:
            capabilities = json.load(stream).get("capabilities", [])
    except (OSError, json.JSONDecodeError, AttributeError):
        return source, []

    completed = _completed_prefix_headers(source)
    required: set[str] = set()
    for row in capabilities:
        symbol = row.get("class")
        include = row.get("include")
        if not isinstance(symbol, str) or not isinstance(include, str):
            continue
        header = include if include.startswith("pulp/") else f"pulp/signal/{include}"
        use = re.search(
            rf"(?<![A-Za-z0-9_]){re.escape(symbol)}(?:\b|\s*<)", source)
        if use is None:
            continue
        if header not in completed:
            required.add(header)

    added = sorted(required)
    if not added:
        return source, []
    block = "".join(f"#include <{header}>\n" for header in added)
    return block + "\n" + source, added


def missing_public_header_dependencies(
        root: str, components: list[str]) -> list[str]:
    """Return missing Pulp headers reachable by module sources or DSP APIs.

    A generated module may use every header in the curated module-DSP
    registry. The installed pack may also retain generated sources from an
    earlier run. Walking both roots before provider resolution keeps a public
    include change from becoming a paid compiler failure.
    """
    headers: dict[str, str] = {}
    for component in components:
        include_root = os.path.join(root, component, "include")
        if not os.path.isdir(include_root):
            continue
        for directory, _, filenames in os.walk(include_root):
            for filename in filenames:
                path = os.path.join(directory, filename)
                headers[os.path.relpath(path, include_root)] = path

    seeds: list[tuple[str, str]] = []
    registry = os.path.join(root, "tools", "rack", "knowledge", "module",
                            "dsp-primitives.json")
    try:
        with open(registry, encoding="utf-8") as source:
            capabilities = json.load(source).get("capabilities", [])
        seeds.extend(
            (f"pulp/signal/{row['include']}", registry)
            for row in capabilities
            if isinstance(row.get("include"), str))
    except (OSError, json.JSONDecodeError, AttributeError):
        # The generator owns the primary missing/malformed-registry diagnostic.
        pass

    source_root = os.path.join(root, "examples", "forge-modular", "src")
    if os.path.isdir(source_root):
        for directory, _, filenames in os.walk(source_root):
            for filename in filenames:
                path = os.path.join(directory, filename)
                try:
                    seeds.extend((header, path) for header in _includes_in(path))
                except OSError:
                    continue

    missing: dict[str, str] = {}
    pending = list(seeds)
    visited: set[str] = set()
    while pending:
        header, required_by = pending.pop()
        if header in visited:
            continue
        visited.add(header)
        path = headers.get(header)
        if path is None:
            missing.setdefault(header, required_by)
            continue
        try:
            pending.extend((dependency, path) for dependency in _includes_in(path))
        except OSError:
            missing.setdefault(header, required_by)

    return [f"{header} (required by {required_by})"
            for header, required_by in sorted(missing.items())]

#!/usr/bin/env python3
"""Validate a GPU recipe discoverability catalog without publishing planned work."""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
from typing import Any

import json_schema_lite


ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_SCHEMA = ROOT / "docs/status/gpu-recipes.schema.json"
FORBIDDEN_COMMAND_TOKENS = frozenset(
    {
        "--host",
        "--newest",
        "--port",
        "pulp-inspect",
    }
)


def _safe_relative(value: str) -> bool:
    if not value or "\\" in value:
        return False
    path = pathlib.PurePosixPath(value)
    return not path.is_absolute() and all(part not in {"", ".", ".."} for part in path.parts)


def _sorted_unique(values: list[str]) -> bool:
    return values == sorted(set(values))


def validate_document(document: Any, schema: Any) -> list[str]:
    """Return closed-schema and cross-field catalog violations."""

    problems = list(json_schema_lite.validate(document, schema))
    if not isinstance(document, dict):
        return problems or ["catalog root must be an object"]
    recipes = document.get("recipes")
    if not isinstance(recipes, list):
        return problems or ["recipes must be an array"]

    ids = [recipe.get("id") for recipe in recipes if isinstance(recipe, dict)]
    if ids != sorted(ids) or len(ids) != len(set(ids)):
        problems.append("recipes must be bytewise sorted by unique id")

    for index, recipe in enumerate(recipes):
        if not isinstance(recipe, dict):
            continue
        prefix = f"recipes[{index}]"
        recipe_id = recipe.get("id")
        for field in ("title", "summary", "evidence_schema"):
            value = recipe.get(field)
            if isinstance(value, str) and value != value.strip():
                problems.append(f"{prefix}.{field} must be trimmed")

        for field in ("symptoms", "docs", "skills"):
            values = recipe.get(field)
            if isinstance(values, list) and all(isinstance(value, str) for value in values):
                if not _sorted_unique(values):
                    problems.append(f"{prefix}.{field} must be bytewise sorted and unique")

        docs = recipe.get("docs")
        if isinstance(docs, list):
            for value in docs:
                if isinstance(value, str) and not _safe_relative(value):
                    problems.append(f"{prefix}.docs contains an unsafe path: {value!r}")

        scaffold = recipe.get("scaffold_template")
        if isinstance(scaffold, str) and not _safe_relative(scaffold):
            problems.append(f"{prefix}.scaffold_template is not a safe relative path")

        entrypoints = recipe.get("entrypoints")
        if not isinstance(entrypoints, dict):
            continue
        cli = entrypoints.get("cli")
        command = cli.get("command") if isinstance(cli, dict) else None
        if isinstance(command, list) and all(isinstance(token, str) for token in command):
            if not command or command[0] != "pulp":
                problems.append(f"{prefix}.entrypoints.cli.command must begin with 'pulp'")
            if "--json" not in command:
                problems.append(f"{prefix}.entrypoints.cli.command must expose typed --json output")
            forbidden = FORBIDDEN_COMMAND_TOKENS.intersection(command)
            if forbidden or command[:2] == ["pulp", "inspect"]:
                problems.append(
                    f"{prefix}.entrypoints.cli.command uses a retired live selector or authority path"
                )

        mcp = entrypoints.get("mcp")
        if isinstance(mcp, dict) and mcp.get("recipe_id") != recipe_id:
            problems.append(f"{prefix}.entrypoints.mcp.recipe_id must equal the catalog recipe id")

        control = entrypoints.get("control")
        kind = recipe.get("kind")
        if kind == "offline" and control is not None:
            problems.append(f"{prefix} is offline and must not declare a live control operation")
        if kind == "live" and not isinstance(control, dict):
            problems.append(f"{prefix} is live and must declare an exact control operation")

    return problems


def load_and_validate(catalog_path: pathlib.Path, schema_path: pathlib.Path = DEFAULT_SCHEMA) -> dict[str, Any]:
    """Load JSON-compatible YAML bytes and reject every schema or semantic violation."""

    catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
    schema = json.loads(schema_path.read_text(encoding="utf-8"))
    problems = validate_document(catalog, schema)
    if problems:
        raise ValueError("\n".join(problems))
    return catalog


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--catalog", type=pathlib.Path, required=True)
    parser.add_argument("--schema", type=pathlib.Path, default=DEFAULT_SCHEMA)
    args = parser.parse_args(argv)
    try:
        document = load_and_validate(args.catalog, args.schema)
    except (OSError, json.JSONDecodeError, ValueError, json_schema_lite.UnsupportedKeyword) as exc:
        print(f"gpu-recipe-catalog: INVALID: {exc}", file=sys.stderr)
        return 1
    print(
        "gpu-recipe-catalog: OK: "
        f"revision {document['catalog_revision']}, {len(document['recipes'])} recipes"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

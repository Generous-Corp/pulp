#!/usr/bin/env python3
"""Validate a GPU recipe discoverability catalog without publishing planned work."""

from __future__ import annotations

import argparse
import importlib.util
import json
import pathlib
import re
import sys
from typing import Any

import json_schema_lite


ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_CATALOG = ROOT / "docs/status/gpu-recipes.yaml"
DEFAULT_SCHEMA = ROOT / "docs/status/gpu-recipes.schema.json"
DEFAULT_REGISTRY_HEADER = (
    ROOT / "tools/cli/gpu_probe/include/pulp_tooling/gpu_probe/probe_result.hpp"
)
DEFAULT_HANDOFF = ROOT / "docs/status/gpu-vellum-handoff.yaml"
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

        availability = recipe.get("availability")
        if isinstance(availability, dict):
            features = availability.get("required_features")
            if isinstance(features, list) and all(isinstance(value, str) for value in features):
                if not _sorted_unique(features):
                    problems.append(
                        f"{prefix}.availability.required_features must be bytewise sorted and unique"
                    )

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
            if command[:4] == ["pulp", "gpu", "probe", "--recipe"]:
                if len(command) < 5 or command[4] != recipe_id:
                    problems.append(
                        f"{prefix}.entrypoints.cli.command recipe id must equal the catalog recipe id"
                    )
                if not isinstance(recipe.get("native_registry_index"), int):
                    problems.append(
                        f"{prefix}.native_registry_index must bind a native probe recipe"
                    )
            elif recipe.get("native_registry_index") is not None:
                problems.append(
                    f"{prefix}.native_registry_index must be null for a non-probe recipe"
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


def _parse_registry_body(body: str) -> list[str]:
    ids = re.findall(r'std::string_view\{"([a-z][a-z0-9.-]+)"\}', body)
    remainder = re.sub(r'std::string_view\{"[a-z][a-z0-9.-]+"\}', "", body)
    if remainder.replace(",", "").strip() or not ids or len(ids) != len(set(ids)):
        raise ValueError("native kRecipeIds registry has an unsupported or duplicate entry")
    return ids


def probe_registry_variants(header_text: str) -> dict[frozenset[str], list[str]]:
    """Read the default and optional Three.js+V8 native recipe registries."""

    conditional = re.search(
        r"#if\s+PULP_GPU_PROBE_THREEJS_CALLABLE\s*"
        r"inline\s+constexpr\s+std::array\s+kRecipeIds\s*\{(?P<enabled>.*?)\};\s*"
        r"#else\s*"
        r"inline\s+constexpr\s+std::array\s+kRecipeIds\s*\{(?P<default>.*?)\};\s*"
        r"#endif",
        header_text,
        flags=re.DOTALL,
    )
    matches = list(
        re.finditer(
            r"inline\s+constexpr\s+std::array\s+kRecipeIds\s*\{(?P<body>.*?)\};",
            header_text,
            flags=re.DOTALL,
        )
    )
    if conditional:
        if len(matches) != 2:
            raise ValueError("conditional native kRecipeIds registry is ambiguous")
        return {
            frozenset(): _parse_registry_body(conditional.group("default")),
            frozenset({"pinned-threejs-runtime", "v8"}): _parse_registry_body(
                conditional.group("enabled")
            ),
        }
    if len(matches) != 1:
        raise ValueError("native kRecipeIds registry was not found exactly once")
    return {frozenset(): _parse_registry_body(matches[0].group("body"))}


def probe_registry_ids(header_text: str) -> list[str]:
    """Return the native default-build recipe sequence."""

    return probe_registry_variants(header_text)[frozenset()]


def catalog_probe_ids(
    document: dict[str, Any], enabled_features: frozenset[str] = frozenset()
) -> list[str]:
    """Return catalog rows that project the native `pulp gpu probe` registry."""

    indexed_ids = []
    for recipe in document["recipes"]:
        command = recipe["entrypoints"]["cli"]["command"]
        required = frozenset(recipe["availability"]["required_features"])
        if (
            command[:4] == ["pulp", "gpu", "probe", "--recipe"]
            and required.issubset(enabled_features)
        ):
            indexed_ids.append((recipe["native_registry_index"], recipe["id"]))
    indices = [index for index, _ in indexed_ids]
    if sorted(indices) != list(range(len(indexed_ids))):
        raise ValueError("catalog native_registry_index values must be unique and contiguous")
    return [recipe_id for _, recipe_id in sorted(indexed_ids)]


def validate_registry_projection(
    document: dict[str, Any],
    registry_ids: list[str],
    enabled_features: frozenset[str] = frozenset(),
) -> list[str]:
    """Require exact equality so a newly callable recipe cannot remain undiscoverable."""

    catalog_ids = catalog_probe_ids(document, enabled_features)
    native_ids = registry_ids
    if catalog_ids == native_ids:
        return []
    missing = sorted(set(native_ids) - set(catalog_ids))
    extra = sorted(set(catalog_ids) - set(native_ids))
    return [
        "catalog/native probe registry drift: "
        f"features={sorted(enabled_features)} "
        f"missing={missing or '[]'} extra={extra or '[]'}; "
        "every kRecipeIds entry must be added to gpu-recipes.yaml explicitly"
    ]


def validate_registry_variants(
    document: dict[str, Any], variants: dict[frozenset[str], list[str]]
) -> list[str]:
    """Validate every native capability variant exposed by the source registry."""

    problems = []
    for features, ids in variants.items():
        problems.extend(validate_registry_projection(document, ids, features))
    return problems


def validate_repository_references(document: dict[str, Any], root: pathlib.Path) -> list[str]:
    """Reject stale docs, skill mappings, or scaffold paths in the source catalog."""

    problems = []
    for recipe in document["recipes"]:
        prefix = f"recipe {recipe['id']}"
        for relative in recipe["docs"]:
            if not (root / relative).is_file():
                problems.append(f"{prefix} references missing doc {relative!r}")
        for skill in recipe["skills"]:
            skill_path = root / ".agents/skills" / skill / "SKILL.md"
            if not skill_path.is_file():
                problems.append(f"{prefix} references missing skill {skill!r}")
        scaffold = recipe["scaffold_template"]
        if scaffold is not None and not (root / scaffold).is_dir():
            problems.append(f"{prefix} references missing scaffold {scaffold!r}")
    return problems


def validate_handoff(document: Any) -> list[str]:
    """Validate the exact, non-authorizing Vellum restart/deletion inventory."""

    if not isinstance(document, dict):
        return ["handoff root must be an object"]
    required = {
        "schema",
        "catalog",
        "ownership_projection",
        "expansion_id",
        "route_set_sha256",
        "boundary_change_authorized",
        "cutover_trigger",
        "entries",
        "b0_required_inputs",
        "stop_rules",
    }
    problems = []
    if set(document) != required:
        problems.append("handoff top-level fields must match the closed v1 contract")
    if document.get("schema") != "pulp.gpu-vellum-handoff.v1":
        problems.append("handoff schema must be pulp.gpu-vellum-handoff.v1")
    if document.get("boundary_change_authorized") is not False:
        problems.append("horizon-A handoff must not authorize an ownership boundary change")
    route_digest = document.get("route_set_sha256")
    if not isinstance(route_digest, str) or not re.fullmatch(r"[0-9a-f]{64}", route_digest):
        problems.append("handoff route_set_sha256 must be lowercase SHA-256")

    entries = document.get("entries")
    if not isinstance(entries, list) or not entries:
        problems.append("handoff entries must be a nonempty array")
        return problems
    expected_fields = {
        "id",
        "current_owner",
        "paths",
        "cutover_action",
        "vellum_phase",
        "delete_after",
        "reason",
    }
    ids = []
    for index, entry in enumerate(entries):
        if not isinstance(entry, dict) or set(entry) != expected_fields:
            problems.append(f"handoff entries[{index}] fields must match the closed v1 contract")
            continue
        ids.append(entry["id"])
        paths = entry["paths"]
        if not isinstance(paths, list) or paths != sorted(set(paths)):
            problems.append(f"handoff entries[{index}].paths must be bytewise sorted and unique")
        for path in paths if isinstance(paths, list) else []:
            if not isinstance(path, str) or not _safe_relative(path):
                problems.append(f"handoff entries[{index}] contains unsafe path {path!r}")
        owner = entry["current_owner"]
        action = entry["cutover_action"]
        phase = entry["vellum_phase"]
        delete_after = entry["delete_after"]
        allowed = {
            "retain-pulp": ("Generous-Corp/pulp", None, False),
            "retain-pulp-followup": ("Generous-Corp/pulp", None, False),
            "replace-pulp-adapter-consumption": ("Generous-Corp/vellum", "B1", True),
            "adopt-when-proven": ("Generous-Corp/vellum", "B2", True),
            "adopt-only-if-a3-evidence-queues": ("Generous-Corp/vellum", "B4", True),
            "adopt-only-if-a4-evidence-queues": ("Generous-Corp/vellum", "B5", True),
            "move-generic-guidance": ("Generous-Corp/vellum", "B6", True),
        }
        if action not in allowed:
            problems.append(f"handoff entries[{index}] has an unknown cutover action")
        else:
            expected_owner, expected_phase, needs_delete_gate = allowed[action]
            if owner != expected_owner or phase != expected_phase:
                problems.append(
                    f"handoff entries[{index}] owner/phase does not match cutover action"
                )
            if needs_delete_gate and not isinstance(delete_after, str):
                problems.append(
                    f"handoff entries[{index}] delete-after nullability does not match action"
                )
            elif not needs_delete_gate and delete_after is not None:
                problems.append(
                    f"handoff entries[{index}] delete-after must be null for retained Pulp work"
                )
            elif isinstance(delete_after, str) and not delete_after.strip():
                problems.append(f"handoff entries[{index}] has an empty delete-after proof")
        if owner not in {"Generous-Corp/pulp", "Generous-Corp/vellum"}:
            problems.append(f"handoff entries[{index}] has an unknown current owner")
    if len(ids) != len(set(ids)):
        problems.append("handoff entries must have unique ids")
    for field in ("b0_required_inputs", "stop_rules"):
        values = document.get(field)
        if not isinstance(values, list) or not values or len(values) != len(set(values)):
            problems.append(f"handoff {field} must be nonempty and unique")
    return problems


def validate_handoff_routing(document: dict[str, Any], root: pathlib.Path) -> list[str]:
    """Bind handoff paths and digest to the frozen authoritative routing helper."""

    projection_value = document.get("ownership_projection")
    if projection_value != ".github/vellum-ownership.json":
        return [
            "handoff ownership_projection must be the authoritative "
            ".github/vellum-ownership.json path"
        ]
    projection_path = root / projection_value
    if projection_path.resolve() != (root.resolve() / ".github/vellum-ownership.json"):
        return ["handoff ownership projection resolved outside its authoritative identity"]
    helper_path = (
        root
        / ".agents/skills/pulp-vellum-change-routing/scripts/routing_evidence.py"
    )
    try:
        spec = importlib.util.spec_from_file_location("gpu_recipe_routing_evidence", helper_path)
        if spec is None or spec.loader is None:
            raise ValueError("cannot load routing helper module")
        routing = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(routing)
        projection, expansion = routing.load_projection(
            projection_path, require_expansion=True
        )
        if expansion is None:
            raise ValueError("ownership projection has no exact expansion")
    except Exception as exc:  # The authority helper owns detailed parse errors.
        return [f"handoff ownership projection is invalid: {exc}"]

    problems = []
    if document.get("expansion_id") != expansion["id"]:
        problems.append("handoff expansion_id differs from the authoritative projection")
    if document.get("route_set_sha256") != expansion["route_set_sha256"]:
        problems.append("handoff route_set_sha256 differs from the authoritative projection")
    for index, entry in enumerate(document.get("entries", [])):
        if not isinstance(entry, dict) or not isinstance(entry.get("paths"), list):
            continue
        for path in entry["paths"]:
            if not isinstance(path, str):
                continue
            candidate = root / path
            if not candidate.exists() or not candidate.resolve().is_relative_to(root.resolve()):
                problems.append(
                    f"handoff entries[{index}] path does not exist inside the repository: {path!r}"
                )
                continue
            try:
                result = routing.route_changes(
                    projection, expansion, [("Generous-Corp/pulp", path)]
                )
            except Exception as exc:
                problems.append(f"handoff entries[{index}] path cannot be routed: {exc}")
                continue
            routed_owner = result["changes"][0]["owner"]
            if routed_owner != entry.get("current_owner"):
                problems.append(
                    f"handoff entries[{index}] owner {entry.get('current_owner')!r} "
                    f"differs from routed owner {routed_owner!r} for {path!r}"
                )
    return problems


def recipes_for_symptoms(document: dict[str, Any], symptoms: list[str]) -> list[dict[str, Any]]:
    """Return deterministic exact-tag matches for clean-agent discovery."""

    wanted = set(symptoms)
    return [recipe for recipe in document["recipes"] if wanted.intersection(recipe["symptoms"])]


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
    parser.add_argument("--catalog", type=pathlib.Path, default=DEFAULT_CATALOG)
    parser.add_argument("--schema", type=pathlib.Path, default=DEFAULT_SCHEMA)
    parser.add_argument("--registry-header", type=pathlib.Path, default=DEFAULT_REGISTRY_HEADER)
    parser.add_argument("--handoff", type=pathlib.Path, default=DEFAULT_HANDOFF)
    parser.add_argument("--list", action="store_true", help="print recipe ids")
    parser.add_argument("--show", metavar="ID", help="print one recipe as JSON")
    parser.add_argument("--symptom", action="append", default=[], help="select exact symptom tag")
    parser.add_argument("--json", action="store_true", help="emit machine-readable output")
    args = parser.parse_args(argv)
    if args.show and args.symptom:
        print("gpu-recipe-catalog: --show and --symptom are mutually exclusive", file=sys.stderr)
        return 2
    try:
        document = load_and_validate(args.catalog, args.schema)
        registry_variants = probe_registry_variants(
            args.registry_header.read_text(encoding="utf-8")
        )
        problems = validate_registry_variants(document, registry_variants)
        if args.catalog.resolve() == DEFAULT_CATALOG.resolve():
            problems.extend(validate_repository_references(document, ROOT))
            handoff = json.loads(args.handoff.read_text(encoding="utf-8"))
            problems.extend(validate_handoff(handoff))
            problems.extend(validate_handoff_routing(handoff, ROOT))
        if problems:
            raise ValueError("\n".join(problems))
    except (OSError, json.JSONDecodeError, ValueError, json_schema_lite.UnsupportedKeyword) as exc:
        print(f"gpu-recipe-catalog: INVALID: {exc}", file=sys.stderr)
        return 1
    selected = document["recipes"]
    if args.show:
        selected = [recipe for recipe in selected if recipe["id"] == args.show]
        if not selected:
            print(f"gpu-recipe-catalog: unknown recipe {args.show!r}", file=sys.stderr)
            return 2
    if args.symptom:
        selected = recipes_for_symptoms(document, args.symptom)
        if not selected:
            print("gpu-recipe-catalog: no recipe matches the supplied symptoms", file=sys.stderr)
            return 2
    if args.list:
        if args.json:
            print(json.dumps({"schema": document["schema"], "recipes": [r["id"] for r in selected]}))
        else:
            print("\n".join(recipe["id"] for recipe in selected))
        return 0
    if args.show or args.symptom:
        payload: Any = selected[0] if args.show else {
            "schema": document["schema"],
            "recipes": selected,
        }
        print(json.dumps(payload, indent=2 if args.json else None, sort_keys=True))
        return 0
    print(
        "gpu-recipe-catalog: OK: "
        f"revision {document['catalog_revision']}, {len(document['recipes'])} recipes"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

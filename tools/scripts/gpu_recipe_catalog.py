#!/usr/bin/env python3
"""Validate a GPU recipe discoverability catalog without publishing planned work."""

from __future__ import annotations

import argparse
import importlib.util
import json
import pathlib
import re
import subprocess
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
HANDOFF_PACKAGE_IDS = (
    "a2-pulp-product-probes",
    "a2t-pulp-trace-analysis",
    "a3-pulp-input-to-present-host",
    "a3-pulp-runtime-control",
    "a4-pulp-dpr-experiment",
    "b1-vellum-gpu-runtime",
    "b2-vellum-gpu-diagnostics",
    "b3-vellum-gpu-capture",
    "b4-vellum-observability-executor",
    "b4-vellum-signature-prewarm",
    "b5-vellum-dpr-policy",
    "b6-pulp-vellum-adoption-skills",
    "pulp-v8-threejs-release-followup",
)
HANDOFF_AUTHORITIES = {
    "plan": {
        "repo": "danielraffel/pulp-planning",
        "revision": "dfa347d59f6ec7ededc01de5c2aba36b52cfcd42",
        "path": "research/2026-08-27-vgpu-gpu-ux-inspiration-audit-and-plan.md",
    },
    "pulp": {
        "repo": "Generous-Corp/pulp",
        "revision": "fd9cb94343a380efe74ddc6f4223c2b90ea93b62",
        "path": ".github/vellum-ownership.json",
    },
    "vellum": {
        "repo": "Generous-Corp/vellum",
        "revision": "f0bcb52ae8c2575886c1e59ba5fc22241e28b403",
        "path": "graphics/src/skia_dawn_surface.mm",
    },
}
HANDOFF_UPSTREAM = {
    "issue": {
        "repo": "Generous-Corp/vellum",
        "number": 26,
        "node_id": "I_kwDOTgkcHM8AAAABOw9JNg",
        "url": "https://github.com/Generous-Corp/vellum/issues/26",
    },
    "comments": [
        {
            "id": 5461645539,
            "node_id": "IC_kwDOTgkcHM8AAAABRYoY4w",
            "url": (
                "https://github.com/Generous-Corp/vellum/issues/26"
                "#issuecomment-5461645539"
            ),
        },
        {
            "id": 5462069172,
            "node_id": "IC_kwDOTgkcHM8AAAABRZCPtA",
            "url": (
                "https://github.com/Generous-Corp/vellum/issues/26"
                "#issuecomment-5462069172"
            ),
        },
    ],
}
HANDOFF_VELLUM_TARGET_PACKAGES = frozenset(
    package for package in HANDOFF_PACKAGE_IDS if package.startswith("b")
)
HANDOFF_EXISTING_VELLUM_PATHS = frozenset({
    ".agents/skills/vellum-app-authoring/SKILL.md",
    "cmake/VellumSkiaDawn.cmake",
    "docs/architecture/gpu-boundary.md",
    "graphics/include/vellum/graphics/skia_dawn_surface.hpp",
    "graphics/src/skia_dawn_surface.mm",
    "graphics/tests/gpu_style_test.cpp",
    "graphics/tests/text_shaping_concurrency_test.cpp",
    "provenance/third-party-lock.json",
})


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


def _closed_text_list(value: Any, *, allow_empty: bool = False) -> bool:
    return (
        isinstance(value, list)
        and (allow_empty or bool(value))
        and value == sorted(set(value))
        and all(isinstance(item, str) and item.strip() == item and item for item in value)
    )


def _closed_contract(
    value: Any, keys: set[str], list_fields: set[str], *, allow_empty: set[str] = frozenset(),
) -> bool:
    if not isinstance(value, dict) or set(value) != keys:
        return False
    for key in keys:
        item = value[key]
        if key in list_fields:
            if not _closed_text_list(item, allow_empty=key in allow_empty):
                return False
        elif not isinstance(item, str) or not item or item.strip() != item:
            return False
    return True


def validate_handoff(document: Any) -> list[str]:
    """Validate the exact, default-zero-authority Vellum implementation ledger."""

    if not isinstance(document, dict):
        return ["handoff root must be an object"]
    required = {
        "schema", "catalog", "ownership_projection", "expansion_id",
        "route_set_sha256", "boundary_change_authorized", "authorities",
        "upstream", "cutover_trigger", "required_package_ids", "entries",
        "b0_required_inputs", "stop_rules",
    }
    problems: list[str] = []
    if set(document) != required:
        problems.append("handoff top-level fields must match the closed v2 contract")
    if document.get("schema") != "pulp.gpu-vellum-handoff.v2":
        problems.append("handoff schema must be pulp.gpu-vellum-handoff.v2")
    if document.get("boundary_change_authorized") is not False:
        problems.append("horizon-A handoff must not authorize an ownership boundary change")
    if document.get("authorities") != HANDOFF_AUTHORITIES:
        problems.append("handoff authorities must pin the reviewed plan/Pulp/Vellum revisions")
    if document.get("upstream") != HANDOFF_UPSTREAM:
        problems.append("handoff upstream issue/comment IDs differ from the reviewed authority")
    route_digest = document.get("route_set_sha256")
    if not isinstance(route_digest, str) or not re.fullmatch(r"[0-9a-f]{64}", route_digest):
        problems.append("handoff route_set_sha256 must be lowercase SHA-256")
    if document.get("required_package_ids") != list(HANDOFF_PACKAGE_IDS):
        problems.append("handoff required_package_ids must include the closed A2-B6 package set")
    for field in ("b0_required_inputs", "stop_rules"):
        if not _closed_text_list(document.get(field)):
            problems.append(f"handoff {field} must be nonempty, sorted, and unique")
    trigger = document.get("cutover_trigger")
    if not isinstance(trigger, str) or not trigger.strip():
        problems.append("handoff cutover_trigger must be explicit")

    entries = document.get("entries")
    if not isinstance(entries, list) or not entries:
        problems.append("handoff entries must be a nonempty array")
        return problems
    entry_fields = {
        "id", "phase", "trigger", "pulp_paths", "vellum_paths",
        "api_contract", "ownership_lifetime_rt", "trace_contract", "tests",
        "performance_gate", "pulp_adoption", "retained_paths", "delete_paths",
        "deletion_gate", "terminal_result",
    }
    path_fields = {"repo", "path", "state", "revision"}
    api_keys = {"surfaces", "request", "result", "errors", "availability"}
    ownership_keys = {"owner", "ownership", "lifetime", "threads", "rt"}
    trace_keys = {"producer", "events", "required_fields", "query_outputs", "loss"}
    test_keys = {"unit", "planted", "real_host", "installed_prefix"}
    performance_keys = {"metrics", "threshold", "audio", "failure"}
    adoption_keys = {"adapter", "surfaces", "proof", "duplicate_rejection"}
    terminal_keys = {"pass", "no_change", "nonterminal"}
    pulp_states = {
        "pulp-owned-retained", "pulp-owned-adoption",
        "pulp-owned-delete-candidate",
        "framework-authoritative-transferred-compatibility",
    }
    ids: list[str] = []
    for index, entry in enumerate(entries):
        prefix = f"handoff entries[{index}]"
        if not isinstance(entry, dict) or set(entry) != entry_fields:
            problems.append(f"{prefix} fields must match the closed v2 package contract")
            continue
        package_id = entry["id"]
        ids.append(package_id)
        if not isinstance(package_id, str) or not package_id:
            problems.append(f"{prefix}.id is invalid")
        for field in ("phase", "trigger"):
            value = entry[field]
            if not isinstance(value, str) or not value.strip():
                problems.append(f"{prefix}.{field} must be explicit")

        path_sets: dict[str, set[str]] = {}
        for field, repo, states in (
            ("pulp_paths", "Generous-Corp/pulp", pulp_states),
            ("vellum_paths", "Generous-Corp/vellum", {"existing", "proposed"}),
        ):
            rows = entry[field]
            if not isinstance(rows, list):
                problems.append(f"{prefix}.{field} must be an array")
                path_sets[field] = set()
                continue
            ordering = []
            observed: set[str] = set()
            for row_index, row in enumerate(rows):
                row_prefix = f"{prefix}.{field}[{row_index}]"
                if not isinstance(row, dict) or set(row) != path_fields:
                    problems.append(f"{row_prefix} fields must be repo/path/state/revision")
                    continue
                path = row["path"]
                ordering.append((row["path"], row["state"], row["revision"], row["repo"]))
                if (
                    row["repo"] != repo or row["state"] not in states
                    or row["revision"] != HANDOFF_AUTHORITIES[
                        "pulp" if field == "pulp_paths" else "vellum"
                    ]["revision"]
                    or not isinstance(path, str) or not _safe_relative(path)
                    or any(token in path for token in ("*", "?", "[", "]"))
                ):
                    problems.append(f"{row_prefix} is not an exact pinned path object")
                if path in observed:
                    problems.append(f"{prefix}.{field} contains duplicate paths")
                observed.add(path)
                if field == "vellum_paths":
                    if row["state"] == "existing" and path not in HANDOFF_EXISTING_VELLUM_PATHS:
                        problems.append(f"{row_prefix} claims an unknown existing Vellum path")
                    if row["state"] == "proposed" and path in HANDOFF_EXISTING_VELLUM_PATHS:
                        problems.append(f"{row_prefix} mislabels an existing Vellum path as proposed")
            if ordering != sorted(ordering):
                problems.append(f"{prefix}.{field} must be bytewise sorted")
            path_sets[field] = observed
        if package_id in HANDOFF_VELLUM_TARGET_PACKAGES and not entry["vellum_paths"]:
            problems.append(f"{prefix} has an empty applicable Vellum target")

        if not _closed_contract(entry["api_contract"], api_keys, {"surfaces"}):
            problems.append(f"{prefix}.api_contract is incomplete")
        if not _closed_contract(entry["ownership_lifetime_rt"], ownership_keys, set()):
            problems.append(f"{prefix}.ownership_lifetime_rt is incomplete")
        trace = entry["trace_contract"]
        if not _closed_contract(
            trace, trace_keys, {"events", "required_fields", "query_outputs"},
            allow_empty={"events", "required_fields", "query_outputs"},
        ):
            problems.append(f"{prefix}.trace_contract is incomplete")
        elif trace["producer"] != "none" and not all(
            trace[field] for field in ("events", "required_fields", "query_outputs")
        ):
            problems.append(f"{prefix}.trace_contract omits an active producer contract")
        if not _closed_contract(entry["tests"], test_keys, test_keys):
            problems.append(f"{prefix}.tests has the wrong closed shape")
        elif not entry["tests"]["planted"]:
            problems.append(f"{prefix}.tests must include planted behavior")
        if not _closed_contract(entry["performance_gate"], performance_keys, {"metrics"}):
            problems.append(f"{prefix}.performance_gate is incomplete")
        if not _closed_contract(entry["pulp_adoption"], adoption_keys, {"surfaces"}):
            problems.append(f"{prefix}.pulp_adoption is incomplete")
        if not _closed_contract(entry["terminal_result"], terminal_keys, set()):
            problems.append(f"{prefix}.terminal_result is incomplete")

        retained = entry["retained_paths"]
        deleted = entry["delete_paths"]
        if not _closed_text_list(retained, allow_empty=True):
            problems.append(f"{prefix}.retained_paths must be exact, sorted, and unique")
            retained = []
        if not _closed_text_list(deleted, allow_empty=True):
            problems.append(f"{prefix}.delete_paths must be exact, sorted, and unique")
            deleted = []
        pulp_inventory = path_sets["pulp_paths"]
        if not set(retained).issubset(pulp_inventory):
            problems.append(f"{prefix}.retained_paths is absent from the Pulp inventory")
        if not set(deleted).issubset(pulp_inventory):
            problems.append(f"{prefix}.delete candidate is absent from the Pulp inventory")
        if set(retained).intersection(deleted):
            problems.append(f"{prefix} retained/deleted paths overlap")
        if set(retained).union(deleted) != pulp_inventory:
            problems.append(
                f"{prefix} must disposition every Pulp path as retained or an exact delete"
            )
        path_rows = {
            row.get("path"): row for row in entry["pulp_paths"] if isinstance(row, dict)
        }
        for path in deleted:
            if (
                any(token in path for token in ("*", "?", "[", "]"))
                or path_rows.get(path, {}).get("state") != "pulp-owned-delete-candidate"
            ):
                problems.append(f"{prefix}.delete_paths contains a broad or unclassified delete")

        gate = entry["deletion_gate"]
        if not isinstance(gate, dict) or set(gate) != {
            "authorized", "authority", "required_evidence",
        }:
            problems.append(f"{prefix}.deletion_gate has the wrong fields")
        elif not deleted:
            if gate != {"authorized": False, "authority": "none", "required_evidence": []}:
                problems.append(f"{prefix} grants prose-only deletion without delete_paths")
        elif (
            gate["authorized"] is not False
            or not isinstance(gate["authority"], str) or not gate["authority"]
            or not _closed_text_list(gate["required_evidence"])
        ):
            problems.append(f"{prefix}.deletion_gate cannot authorize deletion in horizon A")

    if ids != list(HANDOFF_PACKAGE_IDS):
        problems.append("handoff entries must equal the ordered closed A2-B6 package set")
    return problems


def validate_handoff_routing(document: dict[str, Any], root: pathlib.Path) -> list[str]:
    """Bind pinned Pulp paths and deletion candidates to routing and Git facts."""

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
    expected_owners = {
        "pulp-owned-retained": "Generous-Corp/pulp",
        "pulp-owned-adoption": "Generous-Corp/pulp",
        "pulp-owned-delete-candidate": "Generous-Corp/pulp",
        "framework-authoritative-transferred-compatibility": "Generous-Corp/vellum",
    }
    pulp_revision = HANDOFF_AUTHORITIES["pulp"]["revision"]
    for index, entry in enumerate(document.get("entries", [])):
        if not isinstance(entry, dict) or not isinstance(entry.get("pulp_paths"), list):
            continue
        for row_index, row in enumerate(entry["pulp_paths"]):
            if not isinstance(row, dict):
                continue
            path = row.get("path")
            if not isinstance(path, str):
                continue
            candidate = root / path
            if not candidate.exists() or not candidate.resolve().is_relative_to(root.resolve()):
                problems.append(
                    f"handoff entries[{index}].pulp_paths[{row_index}] path does not exist "
                    f"inside the repository: {path!r}"
                )
                continue
            pinned = subprocess.run(
                ["git", "cat-file", "-e", f"{pulp_revision}:{path}"], cwd=root,
                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL, timeout=10, check=False,
            )
            if pinned.returncode != 0:
                problems.append(
                    f"handoff entries[{index}].pulp_paths[{row_index}] is absent "
                    "from the pinned Pulp revision"
                )
            try:
                result = routing.route_changes(
                    projection, expansion, [("Generous-Corp/pulp", path)]
                )
            except Exception as exc:
                problems.append(f"handoff entries[{index}] path cannot be routed: {exc}")
                continue
            routed_owner = result["changes"][0]["owner"]
            expected_owner = expected_owners.get(row.get("state"))
            if expected_owner is not None and routed_owner != expected_owner:
                problems.append(
                    f"handoff entries[{index}] state {row.get('state')!r} expects "
                    f"{expected_owner!r} but routing returned {routed_owner!r} for {path!r}"
                )
        for path in entry.get("delete_paths", []):
            candidate = root / path
            if candidate.exists() and not candidate.is_file():
                problems.append(
                    f"handoff entries[{index}].delete_paths contains broad directory {path!r}"
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

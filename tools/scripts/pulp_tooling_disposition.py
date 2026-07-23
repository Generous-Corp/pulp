#!/usr/bin/env python3
"""Inventory Pulp developer tooling and record its Vellum disposition.

The committed map is deliberately an inventory, not an extraction mechanism.
It keeps Pulp's existing CLI/plugin/MCP affordances visible while Vellum is
validated independently, and forces every newly exposed surface to receive an
explicit ownership decision.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from cli_sync_check import extract_cli_command_names  # noqa: E402


SCHEMA_VERSION = 1
MAP_PATH = Path("docs/status/pulp-tooling-disposition.json")
DISPOSITIONS = {"pulp-owned", "candidate-shared-later", "excluded"}

SOURCE_FILES = {
    "cli": [
        "tools/cli/pulp_cli.cpp",
        "experimental/pulp-rs/src/main.rs",
        "docs/status/cli-commands.yaml",
    ],
    "claude_commands": [".claude/commands/*.md"],
    "agent_skills": [".agents/skills/*/SKILL.md"],
    "mcp_tools": ["tools/mcp/pulp_mcp.cpp"],
    "plugin_registrations": [
        ".claude-plugin/plugin.json",
        ".claude-plugin/marketplace.json",
        ".mcp.json",
    ],
}

CLASSIFICATION_MEANINGS = {
    "pulp-owned": (
        "Pulp keeps this product/workflow surface. It may eventually wrap a "
        "Vellum primitive, but it is not transferred as framework API."
    ),
    "candidate-shared-later": (
        "The independent Vellum product should evaluate an equivalent or "
        "extracted primitive before any ownership transfer is approved."
    ),
    "excluded": (
        "The surface is audio-, plug-in-, or Pulp-product-specific and is "
        "outside the audio-free Vellum framework boundary."
    ),
}

CLI_ARGUMENT_PROVENANCE = {
    "inventory_source": "docs/status/cli-commands.yaml",
    "parser_guard": (
        "Top-level command names are independently extracted from the C++ "
        "dispatch tables and Rust clap enum and must match the YAML manifest."
    ),
    "limitation": (
        "Pulp has no uniform machine-readable schema for nested subcommands and "
        "arguments: their parsers are distributed across C++, Rust, delegated "
        "binaries, and scripts. Therefore argument rows are the declared CLI "
        "contract, not proof of parser acceptance; focused parser and --help "
        "shellout tests remain the behavioral authority."
    ),
}


# This policy seeds the first committed map only. Subsequent --write runs retain
# the map's decisions and write any newly discovered surface as UNCLASSIFIED,
# which makes --check fail until a reviewer makes an explicit decision.
INITIAL_POLICY: dict[str, dict[str, set[str]]] = {
    "cli": {
        "candidate-shared-later": {
            "build", "cache", "clean", "coverage", "create", "design",
            "design-debug", "dev", "doctor", "export-tokens", "harness",
            "import-design", "inspect", "motion", "run", "sdk", "ship",
            "status", "test", "trace", "upgrade", "validate", "version",
        },
        "pulp-owned": {
            "add", "audit", "ci-host", "ci-local", "config", "content",
            "docs", "fmt", "help", "identity", "kit", "list", "loop",
            "macos", "minos", "overflow", "pr", "project", "projects",
            "remove", "search", "suggest", "target", "tool", "tweaks",
            "update",
        },
        "excluded": {"audio", "bake", "host", "import", "scan"},
    },
    "claude_commands": {
        "candidate-shared-later": {
            "coverage-diff", "create", "design", "doctor", "import-design",
            "inspect", "motion", "run", "ship", "status", "test", "trace",
            "upgrade", "validate", "version",
        },
        "pulp-owned": {
            "ci", "ci-host", "codex-consult", "content", "docs", "kit",
            "minos", "pr", "prototype-loop",
        },
        "excluded": {"audio-compare", "audio-harness", "audio-inspect", "bake"},
    },
    "agent_skills": {
        "candidate-shared-later": {
            "android", "engine", "import-design", "ios", "motion",
            "screenshot", "screenshot-sync", "sdf-text", "ship",
            "skia-gpu-build", "streams", "threejs-bridge", "trace-analysis",
            "trace-sql", "web-plugins", "webview-ui",
        },
        "pulp-owned": {
            "ci", "cli-maintenance", "code-comments", "content",
            "friction-report", "handoff", "installable-tools", "intel-canary",
            "kits", "packages", "pr-batching", "pr-review-sweep",
            "prototype-loop", "pulp-web-demo", "tart-ci", "update-demos",
            "upgrade", "video-proof",
        },
        "excluded": {
            "aax", "ara", "audio-harness", "audio-headless-debug", "auv2",
            "auv3", "clap", "cmajor-external", "daw-smoke", "faust",
            "heritage-profile", "hosting", "jsfx-subset", "moonbase", "mpe",
            "playback", "stretch", "timebase", "timeline", "view-bridge",
            "vst3",
        },
    },
    "mcp_tools": {
        "candidate-shared-later": {
            "pulp_build", "pulp_compat", "pulp_create", "pulp_docs_check",
            "pulp_docs_search", "pulp_get_view_tree", "pulp_inspect_dom",
            "pulp_inspect_evaluate", "pulp_inspect_params",
            "pulp_inspect_pending_requests", "pulp_inspect_performance",
            "pulp_inspect_screenshot", "pulp_inspect_set_param",
            "pulp_motion_disable_cost", "pulp_motion_enable_cost",
            "pulp_motion_list_traces", "pulp_motion_load_fixture",
            "pulp_motion_pause", "pulp_motion_play", "pulp_motion_scrub_to",
            "pulp_motion_snapshot", "pulp_motion_start_trace",
            "pulp_motion_stop_trace", "pulp_screenshot", "pulp_simulate_click",
            "pulp_status", "pulp_test", "pulp_trace_explain",
            "pulp_trace_query", "pulp_trace_snapshot", "pulp_trace_start",
            "pulp_trace_stop", "pulp_validate",
        },
        "pulp-owned": {
            "pulp_content", "pulp_content_install", "pulp_content_list",
            "pulp_content_preview", "pulp_content_remove", "pulp_content_rescan",
            "pulp_content_reveal", "pulp_content_update", "pulp_content_validate",
            "pulp_kit", "pulp_kit_apply", "pulp_kit_init", "pulp_kit_inspect",
            "pulp_kit_pack", "pulp_kit_plan", "pulp_kit_publish_check",
            "pulp_kit_remove", "pulp_kit_search", "pulp_kit_validate",
            "pulp_kit_verify", "pulp_minos",
        },
        "excluded": {
            "pulp_audio_compare", "pulp_audio_excerpt_find",
            "pulp_audio_model_activate", "pulp_audio_model_list",
            "pulp_audio_model_status", "pulp_audio_plugin_inspect",
            "pulp_audio_probe_json", "pulp_audio_read_bundle",
            "pulp_audio_render", "pulp_audio_scope", "pulp_inspect_audio",
        },
    },
    "plugin_registrations": {
        "pulp-owned": {
            "claude-marketplace:pulp",
            "claude-plugin:pulp",
            "mcp-server:pulp",
        },
    },
}


class InventoryError(RuntimeError):
    pass


def find_repo_root(start: Path | None = None) -> Path:
    current = (start or Path.cwd()).resolve()
    for candidate in (current, *current.parents):
        if (candidate / "CMakeLists.txt").is_file() and (candidate / "core").is_dir():
            return candidate
    raise InventoryError("not inside a Pulp source checkout")


def _load_yaml(path: Path) -> Any:
    try:
        import yaml
    except ImportError as exc:  # pragma: no cover - repository gates install PyYAML
        raise InventoryError("PyYAML is required to inventory cli-commands.yaml") from exc
    try:
        return yaml.safe_load(path.read_text(encoding="utf-8"))
    except Exception as exc:
        raise InventoryError(f"cannot parse {path}: {exc}") from exc


def _normalize_arguments(raw: Any) -> list[dict[str, Any]]:
    if not raw:
        return []
    result: list[dict[str, Any]] = []
    for arg in raw:
        if not isinstance(arg, dict) or not isinstance(arg.get("name"), str):
            raise InventoryError(f"invalid CLI argument row: {arg!r}")
        row: dict[str, Any] = {"name": arg["name"]}
        if "kind" in arg:
            row["kind"] = arg["kind"]
        if "required" in arg:
            row["required"] = bool(arg["required"])
        result.append(row)
    return result


def _normalize_subcommands(raw: Any) -> list[dict[str, Any]]:
    if not raw:
        return []
    result: list[dict[str, Any]] = []
    for subcommand in raw:
        if not isinstance(subcommand, dict) or not isinstance(subcommand.get("name"), str):
            raise InventoryError(f"invalid CLI subcommand row: {subcommand!r}")
        result.append({
            "name": subcommand["name"],
            "arguments": _normalize_arguments(subcommand.get("args")),
            "subcommands": _normalize_subcommands(subcommand.get("subcommands")),
        })
    return result


def collect_cli(root: Path) -> tuple[list[dict[str, Any]], list[str]]:
    manifest_path = root / "docs/status/cli-commands.yaml"
    manifest = _load_yaml(manifest_path) or {}
    command_rows = manifest.get("commands") or []
    declared: dict[str, dict[str, Any]] = {}
    for row in command_rows:
        if not isinstance(row, dict) or not isinstance(row.get("name"), str):
            raise InventoryError(f"invalid CLI command row: {row!r}")
        declared[row["name"]] = row

    installed = extract_cli_command_names(root)
    issues: list[str] = []
    missing_manifest = sorted(installed - set(declared))
    stale_manifest = sorted(set(declared) - installed)
    if missing_manifest:
        issues.append("CLI commands missing from cli-commands.yaml: " + ", ".join(missing_manifest))
    if stale_manifest:
        issues.append("cli-commands.yaml entries missing from installed CLI: " + ", ".join(stale_manifest))

    result: list[dict[str, Any]] = []
    for name in sorted(installed):
        row = declared.get(name, {})
        result.append({
            "name": name,
            "arguments": _normalize_arguments(row.get("args")),
            "subcommands": _normalize_subcommands(row.get("subcommands")),
        })
    return result, issues


def collect_claude_commands(root: Path) -> list[dict[str, Any]]:
    names = sorted(path.stem for path in (root / ".claude/commands").glob("*.md"))
    return [{"name": name} for name in names]


def collect_agent_skills(root: Path) -> list[dict[str, Any]]:
    result: list[dict[str, Any]] = []
    for path in sorted((root / ".agents/skills").glob("*/SKILL.md")):
        match = re.search(r"^name:\s*([^\n]+)$", path.read_text(encoding="utf-8"), re.MULTILINE)
        if not match:
            raise InventoryError(f"skill has no frontmatter name: {path.relative_to(root)}")
        result.append({"name": match.group(1).strip().strip('"\'')})
    return sorted(result, key=lambda row: row["name"])


def collect_mcp_tools(root: Path) -> list[dict[str, Any]]:
    source = (root / "tools/mcp/pulp_mcp.cpp").read_text(encoding="utf-8")
    names = sorted(set(re.findall(r'"name"\s*:\s*"(pulp_\w+)"', source)))
    return [{"name": name} for name in names]


def _load_json_object(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise InventoryError(f"cannot parse {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise InventoryError(f"{path} must contain a JSON object")
    return value


def collect_plugin_registrations(root: Path) -> list[dict[str, Any]]:
    plugin = _load_json_object(root / ".claude-plugin/plugin.json")
    marketplace = _load_json_object(root / ".claude-plugin/marketplace.json")
    mcp = _load_json_object(root / ".mcp.json")
    rows: list[dict[str, Any]] = []

    plugin_name = plugin.get("name")
    if not isinstance(plugin_name, str):
        raise InventoryError(".claude-plugin/plugin.json needs a plugin name")
    rows.append({
        "name": f"claude-plugin:{plugin_name}",
        "version": plugin.get("version"),
        "commands": plugin.get("commands"),
        "skills": plugin.get("skills"),
        "registration_keys": sorted(plugin),
    })

    plugins = marketplace.get("plugins")
    if not isinstance(plugins, list):
        raise InventoryError(".claude-plugin/marketplace.json needs a plugins array")
    marketplace_metadata = marketplace.get("metadata") or {}
    if not isinstance(marketplace_metadata, dict):
        raise InventoryError(".claude-plugin/marketplace.json metadata must be an object")
    for registration in plugins:
        if not isinstance(registration, dict) or not isinstance(
            registration.get("name"), str
        ):
            raise InventoryError("marketplace plugin registration needs a name")
        rows.append({
            "name": f"claude-marketplace:{registration['name']}",
            "version": registration.get("version"),
            "source": registration.get("source"),
            "min_cli_version": registration.get("min_cli_version"),
            "registration_keys": sorted(registration),
            "catalog_name": marketplace.get("name"),
            "catalog_version": marketplace.get("version"),
            "catalog_metadata_version": marketplace_metadata.get("version"),
        })

    servers = mcp.get("mcpServers")
    if not isinstance(servers, dict):
        raise InventoryError(".mcp.json needs an mcpServers object")
    for name, registration in servers.items():
        if not isinstance(name, str) or not isinstance(registration, dict):
            raise InventoryError("MCP server registration is malformed")
        environment = registration.get("env") or {}
        headers = registration.get("headers") or {}
        if not isinstance(environment, dict) or not isinstance(headers, dict):
            raise InventoryError("MCP server env/headers must be objects")
        public_configuration = {
            key: registration[key]
            for key in ("command", "args", "cwd", "type", "url")
            if key in registration
        }
        rows.append({
            "name": f"mcp-server:{name}",
            "configuration": public_configuration,
            "registration_keys": sorted(registration),
            "environment_keys": sorted(environment),
            "header_keys": sorted(headers),
        })
    return sorted(rows, key=lambda row: row["name"])


def collect_inventory(root: Path) -> tuple[dict[str, list[dict[str, Any]]], list[str]]:
    cli, issues = collect_cli(root)
    return {
        "cli": cli,
        "claude_commands": collect_claude_commands(root),
        "agent_skills": collect_agent_skills(root),
        "mcp_tools": collect_mcp_tools(root),
        "plugin_registrations": collect_plugin_registrations(root),
    }, issues


def initial_disposition(section: str, name: str) -> str:
    matches = [
        disposition
        for disposition, names in INITIAL_POLICY.get(section, {}).items()
        if name in names
    ]
    return matches[0] if len(matches) == 1 else "UNCLASSIFIED"


def render_map(
    inventory: dict[str, list[dict[str, Any]]],
    previous: dict[str, Any] | None = None,
) -> dict[str, Any]:
    previous = previous or {}
    previous_entries = previous.get("entries") or {}
    rendered: dict[str, list[dict[str, Any]]] = {}
    for section, rows in inventory.items():
        old_by_name = {
            row.get("name"): row
            for row in previous_entries.get(section, [])
            if isinstance(row, dict)
        }
        rendered_rows: list[dict[str, Any]] = []
        for row in rows:
            old = old_by_name.get(row["name"], {})
            disposition = old.get("disposition") or initial_disposition(section, row["name"])
            rendered_rows.append({
                "name": row["name"],
                "disposition": disposition,
                **{key: value for key, value in row.items() if key != "name"},
            })
        rendered[section] = rendered_rows
    return {
        "schema_version": SCHEMA_VERSION,
        "purpose": (
            "Track every current Pulp developer-tooling surface while Vellum is "
            "validated independently. This map does not transfer ownership."
        ),
        "inheritance": (
            "CLI arguments and nested subcommands inherit their top-level "
            "command disposition unless a future schema records an explicit override."
        ),
        "cli_argument_provenance": CLI_ARGUMENT_PROVENANCE,
        "classification_meanings": CLASSIFICATION_MEANINGS,
        "source_files": SOURCE_FILES,
        "entries": rendered,
    }


def _structural_entries(document: dict[str, Any]) -> dict[str, list[dict[str, Any]]]:
    entries = document.get("entries") or {}
    result: dict[str, list[dict[str, Any]]] = {}
    for section in SOURCE_FILES:
        result[section] = [
            {key: value for key, value in row.items() if key != "disposition"}
            for row in entries.get(section, [])
        ]
    return result


def validate_map(root: Path, path: Path) -> list[str]:
    issues: list[str] = []
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return [f"tooling disposition map is missing: {path}"]
    except json.JSONDecodeError as exc:
        return [f"tooling disposition map is invalid JSON: {exc}"]

    if document.get("schema_version") != SCHEMA_VERSION:
        issues.append(f"schema_version must be {SCHEMA_VERSION}")
    inventory, source_issues = collect_inventory(root)
    issues.extend(source_issues)
    expected = render_map(inventory, document)
    metadata_keys = {
        "purpose", "inheritance", "cli_argument_provenance",
        "classification_meanings", "source_files",
    }
    metadata_is_stale = any(
        document.get(key) != expected.get(key) for key in metadata_keys
    )
    if metadata_is_stale or _structural_entries(document) != _structural_entries(expected):
        issues.append(
            "tooling inventory is stale; run "
            "python3 tools/scripts/pulp_tooling_disposition.py --write"
        )

    entries = document.get("entries") or {}
    for section in SOURCE_FILES:
        for row in entries.get(section, []):
            disposition = row.get("disposition")
            if disposition not in DISPOSITIONS:
                issues.append(
                    f"{section}:{row.get('name', '<missing>')} needs an explicit disposition"
                )
    return issues


def write_map(root: Path, path: Path) -> None:
    inventory, source_issues = collect_inventory(root)
    if source_issues:
        raise InventoryError("; ".join(source_issues))
    previous: dict[str, Any] | None = None
    if path.exists():
        previous = json.loads(path.read_text(encoding="utf-8"))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(render_map(inventory, previous), indent=2) + "\n", encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--write", action="store_true", help="refresh the committed inventory")
    parser.add_argument("--root", type=Path, help="Pulp checkout root (defaults to cwd discovery)")
    parser.add_argument("--map", dest="map_path", type=Path, help="override map path (tests)")
    args = parser.parse_args(argv)

    try:
        root = find_repo_root(args.root)
        path = args.map_path or (root / MAP_PATH)
        if not path.is_absolute():
            path = root / path
        if args.write:
            write_map(root, path)
        issues = validate_map(root, path)
    except (InventoryError, OSError, ValueError) as exc:
        print(f"pulp-tooling-disposition: {exc}", file=sys.stderr)
        return 1

    if issues:
        for issue in issues:
            print(f"pulp-tooling-disposition: {issue}", file=sys.stderr)
        return 1
    counts = {
        section: len(rows)
        for section, rows in (json.loads(path.read_text())["entries"]).items()
    }
    print("pulp-tooling-disposition: OK " + " ".join(f"{key}={value}" for key, value in counts.items()))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Keep development-inspector capability docs and client claims honest."""

from __future__ import annotations

import argparse
import collections
import pathlib
import re


CAPABILITY_RE = re.compile(
    r'PULP_INSPECT_CAPABILITY\((\w+),\s*"([^"]+)",\s*"([^"]+)",'
    r'\s*\w+,\s*\w+,\s*\w+,\s*\w+,\s*([01]),\s*([01]),'
)
CAPABILITY_ROW_RE = re.compile(
    r"^\|\s*(?:`[^`]+`\s+\()?`([^`]+)`\)?\s*\|\s*"
    r"(yes|no)\s*\|\s*(yes|no)\s*\|\s*(.*?)\s*\|\s*$",
    re.MULTILINE,
)
OPERATION_RE = re.compile(
    r'PULP_(RECEIPT_|PRODUCED_ARTIFACT_)?OPERATION\(\s*'
    r'([A-Za-z][A-Za-z0-9_]*)\s*,\s*"([^"]+)"'
)
RESULT_KIND_RE = re.compile(r'"([a-z][a-z0-9-]*)"\s*\)')
OPERATION_REGISTRY_RE = re.compile(
    r"constexpr auto kControlOperations\s*=.*?^    \}\);$", re.DOTALL | re.MULTILINE
)
OPERATION_ROW_RE = re.compile(
    r"^\|\s*`([^`]+)`\s*\|\s*`([^`]+)`\s+\(`([^`]+)`\)\s*\|\s*`([^`]+)`\s*\|\s*$",
    re.MULTILINE,
)
CAPABILITY_DEFINITIONS_PATH = "inspect/include/pulp/inspect/capability_definitions.inc"
CONTROL_MANIFEST_PATH = "inspect/src/control_manifest.cpp"
CAPABILITY_DOC_PATH = "docs/reference/development-inspector-capabilities.md"
GENERATOR_INVOCATION = "python3 tools/scripts/inspector_truth_check.py --write"
UNDOCUMENTED_REALITY = "_Undocumented: describe the current reality for this capability._"

CapabilityDefinition = collections.namedtuple(
    "CapabilityDefinition", "symbol legacy_id contract_id observe develop"
)
ControlOperation = collections.namedtuple(
    "ControlOperation", "operation_id capability_symbol result_kind"
)
MCP_TOOL_RE = re.compile(
    r'"name":"(pulp_(?:inspect|motion|trace)_[^"]+)",'
    r'"description":"([^"]+)"'
)
MIGRATION_GUIDE_GLOB = "coming-from-*.md"
MIGRATION_GUIDE_FORBIDDEN_CLAIMS = (
    "It also speaks JSON-RPC over a local TCP port",
)

FORBIDDEN_CLAIMS = {
    "tools/cli/cmd_inspect.cpp": (
        "connect to a running plugin's inspector",
        "Launch a plugin with inspector enabled",
    ),
    "tools/cli/pulp_cli.cpp": (
        "Connect to a running plugin inspector",
    ),
    "experimental/pulp-rs/src/help.rs": (
        "Connect to a running plugin inspector",
    ),
    "experimental/pulp-rs/src/main.rs": (
        "Normal launches publish no endpoint",
        "InspectorServer",
        "authenticated discovery",
    ),
    "docs/reference/scripted-ui-inspector.md": (
        "binds all interfaces",
        "transport is unauthenticated",
        "currently unauthenticated",
        "port-file hint",
    ),
    ".claude/commands/inspect.md": (
        "pulp inspect doctor",
        "pulp inspect list",
        "pulp inspect capabilities",
        "--session",
        "--instance",
        "--publication",
    ),
    "docs/agent-integrations.md": (
        "pulp_inspect_list",
        "pulp_inspect_capabilities",
        "pulp_inspect_doctor",
        "discovery, capabilities, and doctor",
    ),
    "docs/reference/cli.md": (
        "`Runtime.evaluate`, `Capture.screenshot`, and `Capture.screenshotNode`",
        "same temp-file hint as `pulp inspect`",
        "defaults to `9147`",
        "pulp trace start --categories dsp,render --out",
        "start [--categories LIST] [--out FILE.pftrace]",
        "- `--host HOST` - inspector host, defaulting to `127.0.0.1`",
        "pulp inspect doctor",
        "pulp inspect list",
        "pulp inspect capabilities",
        "--session SESSION_ID",
    ),
    "docs/reference/development-inspector-capabilities.md": (
        "current dispatch does not enforce the registry",
        "safe multi-consumer fan-out is not implemented",
        "A normal `pulp run`",
        "standalone ownership lands",
        "standalone attachment lands",
        "standalone still constructs only the visual overlay",
        "Production standalone session owner",
        "Production standalone attachment",
        "Production standalone activation",
        "pulp inspect list",
        "pulp_inspect_list",
        "owner-private ephemeral record/token files",
        "nonce/HMAC",
        "real standalone workflow",
    ),
    "tools/mcp/pulp_mcp.cpp": (
        "lacks authenticated main-thread dispatch",
        "Requires a custom host/test fixture that explicitly constructs an inspector endpoint",
        "Requires a custom inspector fixture; normal launches provide no endpoint",
        "Normal launches provide no endpoint",
        "Live host capture is unavailable",
        '"name":"pulp_motion_load_fixture"',
        '"out_path":{"type":"string","description":"Explicit .pftrace output path',
    ),
    "docs/status/cli-commands.yaml": (
        "auto-discovery from a temp-file hint",
        "same temp-file auto-discovery as `pulp inspect`",
        "defaults to 9147",
        "description: Output path for the flushed `.pftrace`",
    ),
    "docs/guides/motion-observability.md": (
        "probes `127.0.0.1:9147`",
    ),
    "experimental/pulp-rs/src/cmd/trace.rs": (
        "pub const DEFAULT_INSPECTOR_PORT: u16 = 9147",
        ".arg(\"--port\")\n            .arg(port.to_string())",
        "use std::net::TcpStream;",
        "fn inspector_reachable(",
        'buf.push_str("\\"out_path\\":\\"");',
    ),
    ".claude/commands/trace.md": (
        "(default 9147)",
    ),
    ".agents/skills/motion/SKILL.md": (
        "PULP_MOTION_SERVER=1",
        "Raw inspector wire",
        "pulp motion load-fixture captures/",
        "--session-id",
        "--instance-id",
    ),
    ".agents/skills/trace-analysis/SKILL.md": (
        "PULP_TRACE_SERVER=1",
    ),
    ".agents/skills/cli-maintenance/SKILL.md": (
        "inspector transport has no authentication",
        "transport is unauthenticated",
        "profiles, list, capabilities, doctor",
        "explicitly hosted inspector fixture",
    ),
}

REQUIRED_CLAIMS = {
    ".claude/commands/inspect.md": (
        "installed `pulp` command",
        "pulp inspect profiles",
        "pulp inspect audit",
        "temporary capability reduction",
    ),
    "docs/agent-integrations.md": (
        "pulp_inspect_profiles",
        "temporary capability reduction",
        "4–7 retain",
    ),
    "docs/reference/cli.md": (
        "Static metadata and offline artifact audit",
        "temporary capability reduction",
        "Phases 4–7",
        "No phase may restore a legacy Inspector",
    ),
    "docs/reference/development-inspector-capabilities.md": (
        "temporary capability reduction",
        "Phases 4–7",
        "legacy server, raw client, discovery",
        "Capability dispatch is fail-closed",
    ),
    "tools/mcp/pulp_mcp.cpp": (
        "Installed in-process",
        "canonical capability-control client",
        "legacy Inspector publication and raw host/port selectors are not accepted",
    ),
    "docs/reference/scripted-ui-inspector.md": (
        "not currently reachable",
        "canonical capability-control replacement",
        "capability reduction",
    ),
    ".agents/skills/motion/SKILL.md": (
        "intentionally unavailable",
        "in-process fixture APIs",
        "canonical broker/control replacement",
    ),
    ".agents/skills/trace-analysis/SKILL.md": (
        "canonical capability-control client",
        "fails closed",
    ),
    ".agents/skills/cli-maintenance/SKILL.md": (
        "static metadata and offline artifact audit",
        "temporary capability reduction",
        "Phases 4–7",
    ),
    "docs/status/cli-commands.yaml": (
        "Read static Development Inspector profiles and perform offline artifact audit",
        "temporary capability reduction",
        "no legacy Inspector fallback",
    ),
}

REQUIRED_BUILD_CONTRACTS = {
    "CMakeLists.txt": (
        "add_subdirectory(inspect)",
        "function(_pulp_configure_standalone_target target view_target)",
        "target_compile_definitions(${target} PRIVATE PULP_ENABLE_INSPECTOR=0)",
        "_pulp_configure_standalone_target(pulp-standalone pulp::view)",
        "_pulp_configure_standalone_target(pulp-standalone-native pulp::view-native)",
    ),
    "inspect/CMakeLists.txt": (
        "if(NOT PULP_ENABLE_INSPECTOR)\n    return()\nendif()",
        "if(PULP_ENABLE_GPU AND NOT ANDROID AND NOT IOS)",
        "src/control_inspector_client.cpp",
        "src/control_broker.cpp",
        "src/control_client.cpp",
        "src/control_trace_session_executor.cpp",
    ),
    "tools/cmake/PulpInstallRules.cmake": (
        "inspect/include/pulp/inspect/control_inspector_client.hpp",
        "inspect/include/pulp/inspect/trace_inspector.hpp",
    ),
    "tools/cmake/PulpControlShipping.cmake": (
        "function(_pulp_cache_control_declarations target profile capabilities eval_ack)",
        'set(PULP_${target}_CONTROL_PROFILE "${profile}" CACHE INTERNAL "" FORCE)',
        'set(PULP_${target}_CONTROL_CAPABILITIES "${capabilities}" CACHE INTERNAL "" FORCE)',
        '"${eval_ack}" CACHE INTERNAL "" FORCE)',
    ),
    "tools/cmake/PulpUtils.cmake": (
        "_pulp_cache_control_declarations(${target}",
    ),
    "tools/cli/CMakeLists.txt": (
        "cmd_inspect_unavailable.cpp",
        "cmd_tweaks_unavailable.cpp",
        "target_link_libraries(pulp-cli PRIVATE pulp::inspect-protocol)",
    ),
    "tools/mcp/CMakeLists.txt": (
        "if(TARGET pulp::inspect-client)",
        "PULP_MCP_ENABLE_INSPECTOR_CLIENT=0",
    ),
    "test/cmake/view_widget_bridge_tests.cmake": (
        "pulp-test-inspector-stripped-artifact",
        "check_inspector_stripped_artifact.cmake",
    ),
}

REMOVED_AUTHORITY_PATHS = (
    "inspect/src/client.cpp",
    "inspect/src/inspector_server.cpp",
    "inspect/src/discovery_reader.cpp",
    "inspect/src/discovery_publisher.cpp",
    "inspect/include/pulp/inspect/inspector_server.hpp",
    "inspect/include/pulp/inspect/discovery.hpp",
    "inspect/include/pulp/inspect/discovery_publisher.hpp",
    "core/format/src/standalone_inspector.cpp",
    "core/format/include/pulp/format/detail/standalone_inspector.hpp",
    "core/format/src/standalone_inspector_capture.cpp",
    "core/format/src/standalone_inspector_policy.cpp",
    "core/format/src/standalone_runtime_eval_dispatch.cpp",
    "experimental/pulp-rs/src/cmd/motion.rs",
    "experimental/pulp-rs/src/cmd/motion_tests.rs",
    ".claude/commands/motion.md",
)

RETIRED_MCP_TOOLS = (
    "pulp_inspect_list",
    "pulp_inspect_capabilities",
    "pulp_inspect_doctor",
    "pulp_inspect_evaluate",
    "pulp_inspect_screenshot",
    "pulp_motion_",
)


def _without_source_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", text)


def _contains_in_order(text: str, fragments: tuple[str, ...]) -> bool:
    offset = 0
    for fragment in fragments:
        found = text.find(fragment, offset)
        if found < 0:
            return False
        offset = found + len(fragment)
    return True


def security_implementation_errors(root: pathlib.Path) -> list[str]:
    """Prove the retired standalone Inspector authority cannot be rebuilt."""
    errors: list[str] = []
    for relative_path in REMOVED_AUTHORITY_PATHS:
        if (root / relative_path).exists():
            errors.append(f"retired Inspector authority path still exists: {relative_path}")
    client_header = (root / "inspect/include/pulp/inspect/client.hpp").read_text(
        encoding="utf-8"
    )
    if "class InspectorClient" in client_header or "request_inspector(" in client_header:
        errors.append("installed client header still exposes the raw Inspector client")
    inspect_cmake = (root / "inspect/CMakeLists.txt").read_text(encoding="utf-8")
    for retired_target in (
        "pulp-inspect-discovery-support",
        "pulp-inspect-discovery",
        "pulp-inspect-publication",
    ):
        if retired_target in inspect_cmake:
            errors.append(f"retired Inspector authority target remains: {retired_target}")
    return errors


def public_surface_errors(root: pathlib.Path) -> list[str]:
    """Pin the intentionally reduced Phase 3 CLI/MCP surface."""
    errors: list[str] = []
    inspect_source = (root / "tools/cli/cmd_inspect.cpp").read_text(encoding="utf-8")
    help_text = _without_source_comments(inspect_source)
    for required in (
        "pulp inspect profiles [--json]",
        "pulp inspect audit ARTIFACT [--json]",
    ):
        if required not in help_text:
            errors.append(f"inspect CLI omits reduced surface: {required}")
    for retired in ("list", "capabilities", "doctor"):
        if re.search(rf'\bverb\s*==\s*"{re.escape(retired)}"', help_text):
            errors.append(f"inspect CLI restores retired live route: {retired}")
    for retired_flag in ("--host", "--port"):
        if re.search(rf'\barg\s*==\s*"{re.escape(retired_flag)}"', help_text):
            errors.append(f"inspect CLI restores retired live selector: {retired_flag}")

    mcp_source = (root / "tools/mcp/pulp_mcp.cpp").read_text(encoding="utf-8")
    for retired in RETIRED_MCP_TOOLS:
        if f'"name":"{retired}' in mcp_source:
            errors.append(f"retired Inspector/Motion MCP tool remains: {retired}")
    for required in ("pulp_inspect_profiles", "pulp_trace_start", "pulp_trace_stop"):
        if f'"name":"{required}"' not in mcp_source:
            errors.append(f"required reduced/canonical MCP tool is missing: {required}")

    trace_dispatch = (root / "experimental/pulp-rs/src/cmd/trace_dispatch.rs").read_text(
        encoding="utf-8"
    )
    if not _contains_in_order(
        trace_dispatch,
        (
            "if let Sub::Query(q)",
            "run_offline_query",
            "matches!(sub, Sub::Start(_) | Sub::Stop(_))",
            "to_control_call(sub)",
        ),
    ):
        errors.append(
            "trace dispatch does not separate offline query from canonical lifecycle control"
        )
    for retired in (
        "resolve_publication_selection",
        "call_selected",
        "PULP_INSPECTOR_PORT",
    ):
        if retired in trace_dispatch:
            errors.append(f"trace dispatch restores retired authority path: {retired}")
    return errors

def parse_capability_definitions(text: str) -> list[CapabilityDefinition]:
    """Read the canonical capability inventory out of the registry include."""
    return [
        CapabilityDefinition(
            symbol=symbol,
            legacy_id=legacy_id,
            contract_id=contract_id,
            observe=observe == "1",
            develop=develop == "1",
        )
        for symbol, legacy_id, contract_id, observe, develop in CAPABILITY_RE.findall(text)
    ]


def parse_control_operations(text: str) -> list[ControlOperation]:
    """Read every typed operation the frozen control registry declares.

    Scanning is bounded to the registry array. The macro definitions above it
    are not invocations -- they spell the parameter list rather than a quoted
    slug -- and bounding the tail keeps a plain operation's result kind from
    being read out of unrelated code below the array.
    """
    registry = OPERATION_REGISTRY_RE.search(text)
    if registry is not None:
        text = registry.group(0)
    matches = list(OPERATION_RE.finditer(text))
    operations: list[ControlOperation] = []
    for index, match in enumerate(matches):
        macro_kind, symbol, slug = match.groups()
        end = matches[index + 1].start() if index + 1 < len(matches) else len(text)
        if macro_kind == "RECEIPT_":
            result_kind = "receipt"
        elif macro_kind == "PRODUCED_ARTIFACT_":
            result_kind = "artifact"
        else:
            trailing = RESULT_KIND_RE.findall(text[match.end():end])
            result_kind = trailing[-1] if trailing else "unknown"
        operations.append(
            ControlOperation(
                operation_id=f"dev.pulp.{slug}@1",
                capability_symbol=symbol,
                result_kind=result_kind,
            )
        )
    return operations


def _capability_cell(definition: CapabilityDefinition) -> str:
    return f"`{definition.contract_id}` (`{definition.legacy_id}`)"


def render_capability_matrix(
    definitions: list[CapabilityDefinition], reality: dict[str, str]
) -> str:
    """Emit the profile-membership matrix; hand-written reality is preserved."""
    lines = [
        "| Canonical capability (legacy spelling) | `observe` | `develop` | Current reality |",
        "|---|---:|---:|---|",
    ]
    for definition in definitions:
        observe = "yes" if definition.observe else "no"
        develop = "yes" if definition.develop else "no"
        prose = reality.get(definition.legacy_id, "").strip() or UNDOCUMENTED_REALITY
        lines.append(
            f"| {_capability_cell(definition)} | {observe} | {develop} | {prose} |"
        )
    return "\n".join(lines)


def render_operation_matrix(
    operations: list[ControlOperation], definitions: list[CapabilityDefinition]
) -> str:
    """Emit the typed operation matrix straight from the frozen registry."""
    by_symbol = {definition.symbol: definition for definition in definitions}
    lines = [
        "| Typed operation | Gating capability (legacy spelling) | Result |",
        "|---|---|---|",
    ]
    for operation in operations:
        definition = by_symbol.get(operation.capability_symbol)
        capability = (
            _capability_cell(definition)
            if definition
            else f"`{operation.capability_symbol}` (`unknown`)"
        )
        lines.append(
            f"| `{operation.operation_id}` | {capability} | `{operation.result_kind}` |"
        )
    return "\n".join(lines)


def _generated_block_re(name: str) -> re.Pattern[str]:
    return re.compile(
        rf"(<!-- BEGIN GENERATED {re.escape(name)}[^>]*-->\n)"
        rf"(.*?)"
        rf"(<!-- END GENERATED {re.escape(name)} -->)",
        re.DOTALL,
    )


def capability_doc_text(
    definitions: list[CapabilityDefinition],
    operations: list[ControlOperation],
    current: str,
) -> str:
    """Return the capability doc with both generated matrices refreshed."""
    reality = {
        capability_id: prose
        for capability_id, _, _, prose in CAPABILITY_ROW_RE.findall(current)
    }
    rendered = current
    for name, body in (
        ("capability-matrix", render_capability_matrix(definitions, reality)),
        ("operation-matrix", render_operation_matrix(operations, definitions)),
    ):
        pattern = _generated_block_re(name)
        if not pattern.search(rendered):
            raise ValueError(
                f"{CAPABILITY_DOC_PATH} has no generated `{name}` region to write"
            )
        rendered = pattern.sub(
            lambda match, body=body: match.group(1) + body + "\n" + match.group(3),
            rendered,
            count=1,
        )
    return rendered


def operation_doc_errors(
    definitions: list[CapabilityDefinition],
    operations: list[ControlOperation],
    capability_doc: str,
) -> list[str]:
    """Every typed operation must name the capability contract that gates it.

    Documenting the capability alone leaves a hole: a second operation added to
    an already-documented capability would otherwise ship undescribed.
    """
    errors: list[str] = []
    if not operations:
        errors.append(
            "control operation registry parsed zero operations; the check is not measuring "
            f"{CONTROL_MANIFEST_PATH}"
        )
        return errors
    by_symbol = {definition.symbol: definition for definition in definitions}
    parsed_rows = OPERATION_ROW_RE.findall(capability_doc)
    documented = collections.Counter(operation_id for operation_id, _, _, _ in parsed_rows)
    rows = {
        operation_id: (contract_id, legacy_id, result_kind)
        for operation_id, contract_id, legacy_id, result_kind in parsed_rows
    }
    for operation_id, count in sorted(documented.items()):
        if count != 1:
            errors.append(
                f"development inspector docs contain {count} rows for operation "
                f"`{operation_id}`"
            )
    registry_ids = {operation.operation_id for operation in operations}
    for operation_id in sorted(set(rows) - registry_ids):
        errors.append(
            f"development inspector docs contain unknown operation `{operation_id}`"
        )
    for operation in operations:
        definition = by_symbol.get(operation.capability_symbol)
        if definition is None:
            errors.append(
                f"control operation `{operation.operation_id}` names unknown capability "
                f"`{operation.capability_symbol}`"
            )
            continue
        if operation.result_kind == "unknown":
            errors.append(
                f"control operation `{operation.operation_id}` has no readable result kind; "
                "the manifest parser is not measuring its declaration"
            )
        row = rows.get(operation.operation_id)
        if row is None:
            errors.append(
                f"development inspector docs omit operation `{operation.operation_id}` "
                f"(capability `{definition.contract_id}`)"
            )
            continue
        contract_id, legacy_id, result_kind = row
        if (contract_id, legacy_id) != (definition.contract_id, definition.legacy_id):
            errors.append(
                f"development inspector docs bind operation `{operation.operation_id}` to "
                f"capability `{contract_id}`; the registry gates it on "
                f"`{definition.contract_id}`"
            )
        if result_kind != operation.result_kind:
            errors.append(
                f"development inspector docs record result `{result_kind}` for operation "
                f"`{operation.operation_id}`; the registry declares "
                f"`{operation.result_kind}`"
            )
    return errors


def undocumented_reality_errors(
    definitions: list[CapabilityDefinition], capability_doc: str
) -> list[str]:
    """Every capability must describe what it actually does today.

    `--write` fills a capability it has no prose for with a placeholder, so the
    check has to reject that placeholder; otherwise a newly registered
    capability ships documented only by the generator's own filler.
    """
    errors: list[str] = []
    reality = {
        capability_id: prose
        for capability_id, _, _, prose in CAPABILITY_ROW_RE.findall(capability_doc)
    }
    for definition in definitions:
        if reality.get(definition.legacy_id) != UNDOCUMENTED_REALITY:
            continue
        errors.append(
            "development inspector docs leave the current reality of "
            f"`{definition.legacy_id}` undocumented; replace the placeholder with "
            "prose describing what the capability does today"
        )
    return errors


def check_root(
    root: pathlib.Path,
    *,
    required_claims=REQUIRED_CLAIMS,
    required_build_contracts=REQUIRED_BUILD_CONTRACTS,
) -> list[str]:
    errors: list[str] = []
    definitions = (root / CAPABILITY_DEFINITIONS_PATH).read_text(encoding="utf-8")
    capability_definitions = parse_capability_definitions(definitions)
    capability_doc = (root / CAPABILITY_DOC_PATH).read_text(encoding="utf-8")
    control_manifest = (root / CONTROL_MANIFEST_PATH).read_text(encoding="utf-8")
    control_operations = parse_control_operations(control_manifest)

    if not capability_definitions:
        errors.append(
            "capability registry parsed zero capabilities; the check is not measuring "
            f"{CAPABILITY_DEFINITIONS_PATH}"
        )

    parsed_rows = CAPABILITY_ROW_RE.findall(capability_doc)
    row_counts = collections.Counter(capability_id for capability_id, _, _, _ in parsed_rows)
    for capability_id, count in row_counts.items():
        if count != 1:
            errors.append(
                "development inspector docs contain "
                f"{count} profile rows for `{capability_id}`"
            )
    capability_rows = {
        capability_id: (observe == "yes", develop == "yes")
        for capability_id, observe, develop, _ in parsed_rows
    }
    definition_ids = {
        definition.legacy_id for definition in capability_definitions
    }
    extra_rows = sorted(set(capability_rows) - definition_ids)
    for capability_id in extra_rows:
        errors.append(
            f"development inspector docs contain unknown capability `{capability_id}`"
        )
    for definition in capability_definitions:
        if f"`{definition.legacy_id}`" not in capability_doc:
            errors.append(
                f"development inspector docs omit capability `{definition.legacy_id}`"
            )
            continue
        expected = (definition.observe, definition.develop)
        if capability_rows.get(definition.legacy_id) != expected:
            expected_text = (
                f"observe={'yes' if expected[0] else 'no'}, "
                f"develop={'yes' if expected[1] else 'no'}"
            )
            errors.append(
                "development inspector docs have stale profile membership for "
                f"`{definition.legacy_id}`; expected {expected_text}"
            )

    errors.extend(undocumented_reality_errors(capability_definitions, capability_doc))
    errors.extend(
        operation_doc_errors(capability_definitions, control_operations, capability_doc)
    )

    shipping_cmake_path = root / "tools/cmake/PulpControlShipping.cmake"
    if shipping_cmake_path.exists():
        shipping_cmake = shipping_cmake_path.read_text(encoding="utf-8")

        def cmake_list(name: str) -> list[str]:
            match = re.search(
                rf"set\({re.escape(name)}\s+(.*?)\)",
                shipping_cmake,
                re.DOTALL,
            )
            return match.group(1).split() if match else []

        legacy = cmake_list("_PULP_INSPECTOR_SHIPPING_CAPABILITIES")
        contracts = cmake_list("_PULP_CONTROL_CAPABILITIES")
        registry_pairs = {
            definition.legacy_id: definition.contract_id
            for definition in capability_definitions
        }
        projected_pairs = list(zip(legacy, contracts))
        if (
            len(legacy) != len(contracts)
            or len(set(legacy)) != len(legacy)
            or len(set(contracts)) != len(contracts)
            or any(registry_pairs.get(old) != contract for old, contract in projected_pairs)
        ):
            errors.append(
                "control shipping capability projection differs from the canonical registry"
            )
        digest_include = (
            root / "inspect/include/pulp/inspect/control_registry_digest.inc"
        ).read_text(encoding="utf-8")
        header_digest = re.search(r'"([0-9a-f]{64})"', digest_include)
        cmake_digest = re.search(
            r'set\(_PULP_CONTROL_REGISTRY_DIGEST_V1\s+"([0-9a-f]{64})"\)',
            shipping_cmake,
        )
        if (
            not header_digest
            or not cmake_digest
            or header_digest.group(1) != cmake_digest.group(1)
        ):
            errors.append(
                "installed shipping helper registry digest differs from the canonical header"
            )

    for relative_path, claims in FORBIDDEN_CLAIMS.items():
        text = (root / relative_path).read_text(encoding="utf-8")
        for claim in claims:
            if claim in text:
                errors.append(f"{relative_path} retains stale claim: {claim}")

    migration_guides = sorted(
        (root / "docs/guides").glob(MIGRATION_GUIDE_GLOB)
    )
    if not migration_guides:
        errors.append("inspector truth could not locate migration guides")
    for path in migration_guides:
        relative_path = path.relative_to(root).as_posix()
        text = path.read_text(encoding="utf-8")
        for claim in MIGRATION_GUIDE_FORBIDDEN_CLAIMS:
            if claim in text:
                errors.append(f"{relative_path} retains stale claim: {claim}")

    for relative_path, claims in required_claims.items():
        text = (root / relative_path).read_text(encoding="utf-8")
        for claim in claims:
            if claim not in text:
                errors.append(f"{relative_path} omits required claim: {claim}")

    for relative_path, contracts in required_build_contracts.items():
        text = (root / relative_path).read_text(encoding="utf-8")
        for contract in contracts:
            if contract not in text:
                errors.append(
                    f"{relative_path} omits inspector build contract: {contract}"
                )

    errors.extend(security_implementation_errors(root))
    errors.extend(public_surface_errors(root))

    inspect_cmake = (root / "inspect/CMakeLists.txt").read_text(encoding="utf-8")
    for target in ("pulp-inspect-runtime",):
        match = re.search(
            rf"target_link_libraries\(\s*{re.escape(target)}\b(.*?)\)",
            inspect_cmake,
            re.DOTALL,
        )
        if not match:
            errors.append(f"{target} has no inspect link contract")
        elif re.search(
            r"pulp::inspect-discovery(?:[\s;]|$)", match.group(1)
        ):
            errors.append(
                f"{target} publicly regains discovery reader authority"
            )

    mcp_source = (root / "tools/mcp/pulp_mcp.cpp").read_text(encoding="utf-8")
    for tool_name, description in MCP_TOOL_RE.findall(mcp_source):
        if tool_name == "pulp_inspect_pending_requests":
            continue
        if tool_name.startswith("pulp_inspect_"):
            if "source-checkout" in description:
                errors.append(
                    f"{tool_name} retains a source-checkout-only client path"
                )
            if not re.search(r"\b(?:Installed|in-process)\b", description):
                errors.append(
                    f"{tool_name} must disclose its installed in-process client path"
                )
        elif tool_name.startswith("pulp_motion_"):
            errors.append(f"retired Motion MCP tool remains: {tool_name}")
        elif "canonical capability-control" not in description:
            errors.append(f"{tool_name} must disclose its canonical control path")

    return errors


def write_root(root: pathlib.Path) -> bool:
    """Regenerate the capability and operation matrices. Returns True if changed."""
    definitions = parse_capability_definitions(
        (root / CAPABILITY_DEFINITIONS_PATH).read_text(encoding="utf-8")
    )
    operations = parse_control_operations(
        (root / CONTROL_MANIFEST_PATH).read_text(encoding="utf-8")
    )
    if not definitions:
        raise ValueError(
            f"{CAPABILITY_DEFINITIONS_PATH} yielded no capabilities; refusing to write"
        )
    if not operations:
        raise ValueError(
            f"{CONTROL_MANIFEST_PATH} yielded no operations; refusing to write"
        )
    path = root / CAPABILITY_DOC_PATH
    current = path.read_text(encoding="utf-8")
    rendered = capability_doc_text(definitions, operations, current)
    if rendered == current:
        return False
    path.write_text(rendered, encoding="utf-8")
    return True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--root",
        type=pathlib.Path,
        default=pathlib.Path(__file__).resolve().parents[2],
    )
    parser.add_argument(
        "--write",
        action="store_true",
        help="regenerate the generated capability and operation matrices in place",
    )
    args = parser.parse_args()

    if args.write:
        try:
            changed = write_root(args.root.resolve())
        except ValueError as error:
            print(f"inspector-truth: {error}")
            return 1
        print(
            "inspector-truth: "
            + (
                f"regenerated {CAPABILITY_DOC_PATH}"
                if changed
                else f"{CAPABILITY_DOC_PATH} already matches the registry"
            )
        )

    errors = check_root(args.root.resolve())
    if errors:
        for error in errors:
            print(f"inspector-truth: {error}")
        return 1
    print("inspector-truth: capability docs and client claims are in sync")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

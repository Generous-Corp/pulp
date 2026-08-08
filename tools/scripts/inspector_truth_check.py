#!/usr/bin/env python3
"""Keep development-inspector capability docs and client claims honest."""

from __future__ import annotations

import argparse
import collections
import pathlib
import re


CAPABILITY_RE = re.compile(
    r'PULP_INSPECT_CAPABILITY\(\w+,\s*"([^"]+)",\s*"([^"]+)",'
    r'\s*\w+,\s*\w+,\s*\w+,\s*\w+,\s*([01]),\s*([01]),'
)
CAPABILITY_ROW_RE = re.compile(
    r"^\|\s*(?:`[^`]+`\s+\()?`([^`]+)`\)?\s*\|\s*"
    r"(yes|no)\s*\|\s*(yes|no)\s*\|",
    re.MULTILINE,
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
        "target_compile_definitions(pulp-standalone PRIVATE PULP_ENABLE_INSPECTOR=0)",
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
    "tools/cmake/PulpInspectorShipping.cmake": (
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
    for claim in (
        "legacy live Trace.query/snapshot/explain authority was removed",
        "pulp trace start/stop use canonical capability control",
        "legacy --port/--session/--instance/--publication selectors",
    ):
        if claim not in trace_dispatch:
            errors.append(f"trace dispatch omits canonical-only contract: {claim}")
    return errors

def check_root(
    root: pathlib.Path,
    *,
    required_claims=REQUIRED_CLAIMS,
    required_build_contracts=REQUIRED_BUILD_CONTRACTS,
) -> list[str]:
    errors: list[str] = []
    definitions = (
        root / "inspect/include/pulp/inspect/capability_definitions.inc"
    ).read_text(encoding="utf-8")
    capability_definitions = CAPABILITY_RE.findall(definitions)
    capability_doc = (
        root / "docs/reference/development-inspector-capabilities.md"
    ).read_text(encoding="utf-8")

    parsed_rows = CAPABILITY_ROW_RE.findall(capability_doc)
    row_counts = collections.Counter(capability_id for capability_id, _, _ in parsed_rows)
    for capability_id, count in row_counts.items():
        if count != 1:
            errors.append(
                "development inspector docs contain "
                f"{count} profile rows for `{capability_id}`"
            )
    capability_rows = {
        capability_id: (observe == "yes", develop == "yes")
        for capability_id, observe, develop in parsed_rows
    }
    definition_ids = {
        capability_id for capability_id, _, _, _ in capability_definitions
    }
    extra_rows = sorted(set(capability_rows) - definition_ids)
    for capability_id in extra_rows:
        errors.append(
            f"development inspector docs contain unknown capability `{capability_id}`"
        )
    for capability_id, _, observe, develop in capability_definitions:
        if f"`{capability_id}`" not in capability_doc:
            errors.append(
                f"development inspector docs omit capability `{capability_id}`"
            )
            continue
        expected = (observe == "1", develop == "1")
        if capability_rows.get(capability_id) != expected:
            expected_text = (
                f"observe={'yes' if expected[0] else 'no'}, "
                f"develop={'yes' if expected[1] else 'no'}"
            )
            errors.append(
                "development inspector docs have stale profile membership for "
                f"`{capability_id}`; expected {expected_text}"
            )

    shipping_cmake_path = root / "tools/cmake/PulpInspectorShipping.cmake"
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
            capability_id: contract_id
            for capability_id, contract_id, _, _ in capability_definitions
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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--root",
        type=pathlib.Path,
        default=pathlib.Path(__file__).resolve().parents[2],
    )
    args = parser.parse_args()

    errors = check_root(args.root.resolve())
    if errors:
        for error in errors:
            print(f"inspector-truth: {error}")
        return 1
    print("inspector-truth: capability docs and client claims are in sync")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

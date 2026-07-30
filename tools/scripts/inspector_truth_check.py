#!/usr/bin/env python3
"""Keep development-inspector capability docs and client claims honest."""

from __future__ import annotations

import argparse
import collections
import pathlib
import re


CAPABILITY_RE = re.compile(
    r'PULP_INSPECT_CAPABILITY\(\w+,\s*"([^"]+)",\s*\w+,\s*([01]),\s*([01]),'
)
CAPABILITY_ROW_RE = re.compile(
    r"^\|\s*`([^`]+)`\s*\|\s*(yes|no)\s*\|\s*(yes|no)\s*\|",
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
    "docs/reference/scripted-ui-inspector.md": (
        "binds all interfaces",
        "transport is unauthenticated",
        "currently unauthenticated",
        "port-file hint",
    ),
    ".claude/commands/inspect.md": (
        "`Runtime.evaluate`, `Capture.screenshot`, and `Capture.screenshotNode`",
        "transitional port-file hint",
        "without authenticated session identity",
    ),
    "docs/agent-integrations.md": (
        "`pulp_inspect_evaluate` and `pulp_inspect_screenshot` currently",
    ),
    "docs/reference/cli.md": (
        "`Runtime.evaluate`, `Capture.screenshot`, and `Capture.screenshotNode`",
        "same temp-file hint as `pulp inspect`",
    ),
    "docs/reference/development-inspector-capabilities.md": (
        "current dispatch does not enforce the registry",
        "safe multi-consumer fan-out is not implemented",
    ),
    "tools/mcp/pulp_mcp.cpp": (
        "lacks authenticated main-thread dispatch",
    ),
    "docs/status/cli-commands.yaml": (
        "auto-discovery from a temp-file hint",
        "same temp-file auto-discovery as `pulp inspect`",
    ),
    "experimental/pulp-rs/src/cmd/motion.rs": (
        "pub const DEFAULT_INSPECTOR_PORT: u16 = 9147",
        ".arg(\"--port\")\n            .arg(port.to_string())",
        "use std::net::TcpStream;",
        "fn inspector_reachable(",
    ),
    "experimental/pulp-rs/src/cmd/trace.rs": (
        "pub const DEFAULT_INSPECTOR_PORT: u16 = 9147",
        ".arg(\"--port\")\n            .arg(port.to_string())",
        "use std::net::TcpStream;",
        "fn inspector_reachable(",
    ),
    ".claude/commands/trace.md": (
        "(default 9147)",
    ),
}

REQUIRED_CLAIMS = {
    ".claude/commands/inspect.md": (
        "unavailable in normal launches",
        "explicitly wired custom fixture",
    ),
    "docs/agent-integrations.md": (
        "unavailable in normal launches",
        "explicitly wired custom fixture",
    ),
    "docs/reference/cli.md": (
        "unavailable in normal launches",
        "explicitly wired",
    ),
    "docs/reference/development-inspector-capabilities.md": (
        "owner-private ephemeral record/token files",
        "Capability dispatch is fail-closed",
    ),
    "docs/reference/scripted-ui-inspector.md": (
        "nonce/HMAC",
        "owner-private per-session credential",
    ),
}

REQUIRED_BUILD_CONTRACTS = {
    "CMakeLists.txt": (
        "if(PULP_ENABLE_INSPECTOR)\n    add_subdirectory(inspect)\nendif()",
        "if(PULP_ENABLE_INSPECTOR AND TARGET pulp::inspect AND NOT IOS)",
        "target_link_libraries(pulp-standalone PRIVATE pulp::inspect)",
    ),
    "inspect/CMakeLists.txt": (
        "if(PULP_ENABLE_GPU AND NOT ANDROID AND NOT IOS)",
    ),
    "tools/cli/CMakeLists.txt": (
        "cmd_inspect_unavailable.cpp",
        "cmd_tweaks_unavailable.cpp",
    ),
    "test/cmake/view_widget_bridge_tests.cmake": (
        "pulp-test-inspector-stripped-artifact",
        "check_inspector_stripped_artifact.cmake",
    ),
}

REQUIRED_SECURITY_CONTRACTS = {
    "inspect/src/discovery.cpp": (
        "if (info.pbi_status == SZOMB)",
    ),
}


def check_root(root: pathlib.Path) -> list[str]:
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
    definition_ids = {capability_id for capability_id, _, _ in capability_definitions}
    extra_rows = sorted(set(capability_rows) - definition_ids)
    for capability_id in extra_rows:
        errors.append(
            f"development inspector docs contain unknown capability `{capability_id}`"
        )
    for capability_id, observe, develop in capability_definitions:
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

    for relative_path, claims in REQUIRED_CLAIMS.items():
        text = (root / relative_path).read_text(encoding="utf-8")
        for claim in claims:
            if claim not in text:
                errors.append(f"{relative_path} omits required claim: {claim}")

    for relative_path, contracts in REQUIRED_BUILD_CONTRACTS.items():
        text = (root / relative_path).read_text(encoding="utf-8")
        for contract in contracts:
            if contract not in text:
                errors.append(
                    f"{relative_path} omits inspector build contract: {contract}"
                )

    for relative_path, contracts in REQUIRED_SECURITY_CONTRACTS.items():
        text = (root / relative_path).read_text(encoding="utf-8")
        for contract in contracts:
            if contract not in text:
                errors.append(
                    f"{relative_path} omits inspector security contract: {contract}"
                )

    mcp_source = (root / "tools/mcp/pulp_mcp.cpp").read_text(encoding="utf-8")
    for tool_name, description in MCP_TOOL_RE.findall(mcp_source):
        if tool_name == "pulp_inspect_pending_requests":
            continue
        if "source-checkout" not in description:
            errors.append(
                f"{tool_name} must disclose its source-checkout-only client path"
            )

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

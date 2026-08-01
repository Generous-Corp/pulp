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
    "experimental/pulp-rs/src/main.rs": (
        "PULP_MOTION_SERVER=1",
        "PULP_TRACE_SERVER=1",
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
        "defaults to `9147`",
        "pulp trace start --categories dsp,render --out",
        "start [--categories LIST] [--out FILE.pftrace]",
        "- `--host HOST` - inspector host, defaulting to `127.0.0.1`",
    ),
    "docs/reference/development-inspector-capabilities.md": (
        "current dispatch does not enforce the registry",
        "safe multi-consumer fan-out is not implemented",
    ),
    "tools/mcp/pulp_mcp.cpp": (
        "lacks authenticated main-thread dispatch",
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
    "experimental/pulp-rs/src/cmd/motion.rs": (
        "pub const DEFAULT_INSPECTOR_PORT: u16 = 9147",
        ".arg(\"--port\")\n            .arg(port.to_string())",
        "use std::net::TcpStream;",
        "fn inspector_reachable(",
        "PULP_MOTION_SERVER=1",
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
        "remote clients cannot select a filesystem path",
        "select one exact",
        "authenticated publication; all three are required",
        "trace_control_available",
    ),
    "docs/reference/development-inspector-capabilities.md": (
        "owner-private ephemeral record/token files",
        "extended ACLs",
        "Capability dispatch is fail-closed",
    ),
    "docs/reference/scripted-ui-inspector.md": (
        "nonce/HMAC",
        "owner-private per-session credential",
    ),
    ".agents/skills/motion/SKILL.md": (
        "explicitly wired custom fixture",
        "authenticated discovery",
        "nonce/HMAC",
        "intentionally unavailable",
        "--session ID --instance ID --publication ID",
    ),
    ".agents/skills/trace-analysis/SKILL.md": (
        "explicitly wired custom fixture",
        "authenticated discovery",
    ),
    ".agents/skills/cli-maintenance/SKILL.md": (
        "nonce/HMAC",
        "owner-private per-session credential",
        "defense-in-depth",
    ),
    "docs/status/cli-commands.yaml": (
        "Exact authenticated session id; must be paired with --instance",
        "Exact authenticated instance id; must be paired with --session",
        "Non-reusable publication id",
        "trace_control_available",
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
        "src/discovery_common.cpp",
        "src/discovery_paths.cpp",
        "src/discovery_security.cpp",
        "pulp::inspect-discovery-support",
        "pulp-inspect-discovery-support PRIVATE Advapi32",
        "add_library(pulp-inspect-discovery src/discovery_reader.cpp)",
        "add_library(pulp-inspect-publication",
        "src/discovery_publisher.cpp",
        "src/discovery_security_write.cpp",
        "pulp::inspect-publication",
        "src/trace_inspector.cpp",
    ),
    "tools/cmake/PulpInstallRules.cmake": (
        "inspect/include/pulp/inspect/publication_binding.hpp",
        "inspect/include/pulp/inspect/trace_inspector.hpp",
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

SECURITY_IMPLEMENTATION_PATHS = (
    "inspect/src/client.cpp",
    "inspect/src/inspector_server.cpp",
    "inspect/src/discovery_reader.cpp",
    "experimental/pulp-rs/src/cmd/inspector.rs",
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
    """Check executable security invariants in production implementations."""
    sources = {
        path: _without_source_comments((root / path).read_text(encoding="utf-8"))
        for path in SECURITY_IMPLEMENTATION_PATHS
    }
    client = sources["inspect/src/client.cpp"]
    server = sources["inspect/src/inspector_server.cpp"]
    discovery = sources["inspect/src/discovery_reader.cpp"]
    rust = sources["experimental/pulp-rs/src/cmd/inspector.rs"]
    errors: list[str] = []

    if not re.search(
        r"challenge\.session_id\s*!=\s*record\.session_id\s*\|\|\s*"
        r"challenge\.instance_id\s*!=\s*record\.instance_id\s*\|\|\s*"
        r"challenge\.publication_id\s*!=\s*record\.publication_id",
        client,
    ):
        errors.append("client authentication no longer binds the exact publication")
    if "verify_inspector_server_auth_proof(" not in client:
        errors.append("client authentication no longer verifies the server proof")
    if not re.search(
        r"if\s*\(!event_state->authenticated\).*?"
        r"event_state->pre_auth_events\.push_back",
        client,
        re.DOTALL,
    ):
        errors.append("client events are no longer quarantined before authentication")

    if not re.search(
        r"if\s*\(!authenticated\)\s*\{.*?"
        r"request\.method\s*!=\s*methods::kSessionAuthenticate",
        server,
        re.DOTALL,
    ):
        errors.append("server accepts requests before authentication")
    if not _contains_in_order(
        server,
        (
            "found->second->verifier->authenticate(proof)",
            "found->second->verifier.reset()",
            "found->second->authenticated =",
        ),
    ):
        errors.append("server authentication verifier can be replayed")
    if not _contains_in_order(
        server,
        (
            "session->suspend_dispatches()",
            "server.stop()",
            "client->outbound->shutdown()",
            "cleanup_cv.wait(",
            "publication.clear_after_endpoint_stop()",
            "secure_zero_memory(token.data(), token.size())",
        ),
    ):
        errors.append("server teardown no longer drains clients before retirement")

    if not re.search(
        r"current->session_id\s*!=\s*record\.session_id\s*\|\|\s*"
        r"current->instance_id\s*!=\s*record\.instance_id\s*\|\|\s*"
        r"current->publication_id\s*!=\s*record\.publication_id",
        discovery,
    ):
        errors.append("credential reread no longer rejects stale publication identity")
    if not re.search(
        r"record\.session_id\s*!=\s*session_id\).*?"
        r"record\.instance_id\s*!=\s*instance_id\).*?"
        r"record\.publication_id\s*!=\s*publication_id",
        discovery,
        re.DOTALL,
    ):
        errors.append("discovery selection no longer matches the complete identity")

    if "is_some_and(|selection| !selection.publication_id.is_empty())" not in rust:
        errors.append("Rust mutation routing no longer requires a publication id")
    if not _contains_in_order(
        rust,
        (
            'value.get("sessionId")?.as_str()?',
            'value.get("instanceId")?.as_str()?',
            'value.get("publicationId")?.as_str()?',
            "!valid_session_identity(publication_id)",
        ),
    ):
        errors.append("Rust capability discovery no longer validates exact identity")

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

    reader_header = (
        root / "inspect/include/pulp/inspect/discovery.hpp"
    ).read_text(encoding="utf-8")
    publisher_header = (
        root / "inspect/include/pulp/inspect/discovery_publisher.hpp"
    ).read_text(encoding="utf-8")
    if "InspectorDiscoveryPublisher" in reader_header:
        errors.append(
            "read-only discovery header exposes publisher authority"
        )
    if "class InspectorDiscoveryPublisher" not in publisher_header:
        errors.append(
            "publisher authority header omits InspectorDiscoveryPublisher"
        )

    inspect_cmake = (root / "inspect/CMakeLists.txt").read_text(encoding="utf-8")
    for target in ("pulp-inspect-publication", "pulp-inspect-runtime"):
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

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
        "select one exact authenticated session identity; both are required together",
    ),
    "docs/reference/development-inspector-capabilities.md": (
        "owner-private ephemeral record/token files",
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
        "--session ID --instance ID",
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
        "add_library(pulp-inspect-publication src/discovery.cpp)",
        "PULP_INSPECT_READER_ONLY=1",
        "PULP_INSPECT_PUBLISHER_ONLY=1",
        "pulp::inspect-publication",
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
        "info.kp_proc.p_stat == SZOMB",
        "without a supported start-time identity fail closed",
        "record && read_credential(*record).has_value()",
        "std::numeric_limits<std::int64_t>::max() - now",
    ),
    "inspect/src/inspector_publication.hpp": (
        "heartbeat_interval > std::chrono::milliseconds::max() / 3",
        "std::chrono::steady_clock::duration::max()",
        "std::chrono::steady_clock::time_point::max() - interval",
        "!publisher_->refresh(ttl_)",
    ),
    "inspect/src/trace_inspector.cpp": (
        "out_path is unavailable over the inspector",
        "ring_mb < kMinTraceRingMb || ring_mb > kMaxTraceRingMb",
        "Tracing::start(categories, {}, ring_kb)",
    ),
    "experimental/pulp-rs/src/cmd/trace.rs": (
        "pulp trace start --out is unavailable",
        "if !(1..=512).contains(&ring_mb)",
    ),
    "experimental/pulp-rs/src/cmd/motion.rs": (
        'a == "--session" || a == "--instance"',
        "--session and --instance must be supplied together",
        "talker.call_selected(",
        'no_args("play", &rest[1..])',
        'no_args("scrub", &args[1..])',
        'no_args("cost", &args[1..])',
    ),
    "tools/mcp/pulp_mcp.cpp": (
        '"minimum":1,"maximum":512',
        "The host owns the trace destination.",
    ),
    "core/runtime/src/socket.cpp": (
        "FIONBIO",
        "O_NONBLOCK",
        "WSAPoll",
        "::poll(&descriptor, 1, wait_ms)",
        "SO_ERROR",
        "restore_blocking()",
    ),
    "inspect/src/client.cpp": (
        "events::IpcTransport::Socket,\n                                   bounded_timeout",
        "const auto challenge_timeout = remaining()",
        "const auto authentication_timeout = remaining()",
        "impl_->wait_for_response(1, remaining())",
    ),
    "experimental/pulp-rs/src/cmd/inspector.rs": (
        "must be an integer from 1 to 65535",
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

#!/usr/bin/env python3

import pathlib
import tempfile
import unittest

import inspector_truth_check


class InspectorTruthCheckTests(unittest.TestCase):
    def make_root(self) -> pathlib.Path:
        root = pathlib.Path(self.tempdir.name)
        files = {
            "inspect/include/pulp/inspect/capability_definitions.inc":
                'PULP_INSPECT_CAPABILITY(StateRead, "state.read", Observe, 1, 1, 1)\n',
            "docs/reference/development-inspector-capabilities.md":
                "| `state.read` | yes | yes | available |\n"
                "owner-private ephemeral record/token files; "
                "Capability dispatch is fail-closed\n",
            "tools/cli/cmd_inspect.cpp": "custom fixture only\n",
            "tools/cli/pulp_cli.cpp":
                "Connect to an explicitly hosted inspector fixture\n",
            "experimental/pulp-rs/src/help.rs":
                "Connect to an explicitly hosted inspector fixture\n",
            "experimental/pulp-rs/src/main.rs":
                "explicitly owned custom host; authenticated discovery\n",
            "docs/reference/scripted-ui-inspector.md":
                "loopback only; nonce/HMAC proof; "
                "owner-private per-session credential\n",
            "docs/guides/coming-from-reference.md": "visual overlay only\n",
            "docs/guides/motion-observability.md":
                "authenticated discovery filter\n",
            ".claude/commands/inspect.md":
                "unavailable in normal launches; explicitly wired custom fixture\n",
            "docs/agent-integrations.md":
                "unavailable in normal launches; explicitly wired custom fixture\n",
            "docs/reference/cli.md":
                "unavailable in normal launches; explicitly wired\n",
            "docs/status/cli-commands.yaml":
                "owner-private authenticated discovery\n",
            "experimental/pulp-rs/src/cmd/motion.rs":
                "authenticated auto-discovery\n",
            "experimental/pulp-rs/src/cmd/trace.rs":
                "authenticated auto-discovery\n",
            "experimental/pulp-rs/src/cmd/inspector.rs":
                "must be an integer from 1 to 65535\n",
            ".claude/commands/trace.md":
                "authenticated ephemeral discovery\n",
            ".agents/skills/motion/SKILL.md":
                "explicitly wired custom fixture; authenticated discovery; "
                "nonce/HMAC; intentionally unavailable\n",
            ".agents/skills/trace-analysis/SKILL.md":
                "explicitly wired custom fixture; authenticated discovery\n",
            ".agents/skills/cli-maintenance/SKILL.md":
                "nonce/HMAC; owner-private per-session credential; "
                "defense-in-depth\n",
            "tools/mcp/pulp_mcp.cpp":
                '"name":"pulp_inspect_dom","description":"Experimental source-checkout client"\n',
            "CMakeLists.txt":
                "if(PULP_ENABLE_INSPECTOR)\n"
                "    add_subdirectory(inspect)\n"
                "endif()\n"
                "if(PULP_ENABLE_INSPECTOR AND TARGET pulp::inspect AND NOT IOS)\n"
                "target_link_libraries(pulp-standalone PRIVATE pulp::inspect)\n",
            "inspect/CMakeLists.txt":
                "add_library(pulp-inspect-publication src/discovery.cpp)\n"
                "PULP_INSPECT_READER_ONLY=1\n"
                "PULP_INSPECT_PUBLISHER_ONLY=1\n"
                "pulp::inspect-publication\n"
                "if(PULP_ENABLE_GPU AND NOT ANDROID AND NOT IOS)\n",
            "inspect/include/pulp/inspect/discovery.hpp":
                "class InspectorDiscoveryReader {};\n",
            "inspect/include/pulp/inspect/discovery_publisher.hpp":
                "class InspectorDiscoveryPublisher {};\n",
            "tools/cli/CMakeLists.txt":
                "cmd_inspect_unavailable.cpp\ncmd_tweaks_unavailable.cpp\n",
            "test/cmake/view_widget_bridge_tests.cmake":
                "pulp-test-inspector-stripped-artifact\n"
                "check_inspector_stripped_artifact.cmake\n",
            "inspect/src/discovery.cpp":
                "info.kp_proc.p_stat == SZOMB\n"
                "without a supported start-time identity fail closed\n"
                "record && read_credential(*record).has_value()\n"
                "std::numeric_limits<std::int64_t>::max() - now\n",
            "inspect/src/inspector_publication.hpp":
                "heartbeat_interval > std::chrono::milliseconds::max() / 3\n"
                "std::chrono::steady_clock::duration::max()\n"
                "std::chrono::steady_clock::time_point::max() - interval\n"
                "!publisher_->refresh(ttl_)\n",
        }
        for relative, text in files.items():
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        return root

    def setUp(self) -> None:
        self.tempdir = tempfile.TemporaryDirectory()

    def tearDown(self) -> None:
        self.tempdir.cleanup()

    def test_accepts_truthful_fixture(self) -> None:
        self.assertEqual(inspector_truth_check.check_root(self.make_root()), [])

    def test_rejects_omitted_capability(self) -> None:
        root = self.make_root()
        (root / "docs/reference/development-inspector-capabilities.md").write_text(
            "no capability table\n", encoding="utf-8"
        )
        self.assertIn("omit capability", " ".join(inspector_truth_check.check_root(root)))

    def test_rejects_stale_profile_membership(self) -> None:
        root = self.make_root()
        path = root / "docs/reference/development-inspector-capabilities.md"
        path.write_text(
            path.read_text(encoding="utf-8").replace(
                "| `state.read` | yes | yes |",
                "| `state.read` | no | yes |",
            ),
            encoding="utf-8",
        )
        self.assertIn(
            "stale profile membership",
            " ".join(inspector_truth_check.check_root(root)),
        )

    def test_rejects_duplicate_and_unknown_profile_rows(self) -> None:
        root = self.make_root()
        path = root / "docs/reference/development-inspector-capabilities.md"
        path.write_text(
            path.read_text(encoding="utf-8")
            + "| `state.read` | no | yes | contradictory duplicate |\n"
            + "| `invented.extra` | yes | yes | unknown row |\n",
            encoding="utf-8",
        )
        errors = " ".join(inspector_truth_check.check_root(root))
        self.assertIn("2 profile rows", errors)
        self.assertIn("unknown capability `invented.extra`", errors)

    def test_rejects_stale_runtime_claim_and_mcp_description(self) -> None:
        root = self.make_root()
        (root / "tools/cli/cmd_inspect.cpp").write_text(
            "connect to a running plugin's inspector\n", encoding="utf-8"
        )
        (root / "tools/cli/pulp_cli.cpp").write_text(
            "Connect to a running plugin inspector\n", encoding="utf-8"
        )
        (root / "tools/mcp/pulp_mcp.cpp").write_text(
            '"name":"pulp_inspect_dom","description":"Live plugin inspector"\n',
            encoding="utf-8",
        )
        errors = " ".join(inspector_truth_check.check_root(root))
        self.assertIn("stale claim", errors)
        self.assertIn("source-checkout-only", errors)

    def test_rejects_obsolete_unauthenticated_transport_claims(self) -> None:
        root = self.make_root()
        (root / "docs/reference/scripted-ui-inspector.md").write_text(
            "inspector transport is unauthenticated and uses a port-file hint\n",
            encoding="utf-8",
        )
        (root / "tools/mcp/pulp_mcp.cpp").write_text(
            '"name":"pulp_inspect_set_param",'
            '"description":"Experimental source-checkout client that lacks '
            'authenticated main-thread dispatch"\n',
            encoding="utf-8",
        )
        errors = " ".join(inspector_truth_check.check_root(root))
        self.assertIn("transport is unauthenticated", errors)
        self.assertIn("port-file hint", errors)
        self.assertIn("lacks authenticated main-thread dispatch", errors)

    def test_rejects_stale_discovery_claims_on_all_cli_surfaces(self) -> None:
        root = self.make_root()
        (root / ".claude/commands/inspect.md").write_text(
            "unavailable in normal launches; explicitly wired custom fixture; "
            "transitional port-file hint without authenticated session identity\n",
            encoding="utf-8",
        )
        (root / "docs/reference/cli.md").write_text(
            "unavailable in normal launches; explicitly wired; "
            "same temp-file hint as `pulp inspect`\n",
            encoding="utf-8",
        )
        (root / "docs/status/cli-commands.yaml").write_text(
            "auto-discovery from a temp-file hint\n"
            "same temp-file auto-discovery as `pulp inspect`\n",
            encoding="utf-8",
        )
        errors = " ".join(inspector_truth_check.check_root(root))
        self.assertIn(".claude/commands/inspect.md retains stale claim", errors)
        self.assertIn("docs/reference/cli.md retains stale claim", errors)
        self.assertIn("docs/status/cli-commands.yaml retains stale claim", errors)

    def test_rejects_legacy_rust_wrapper_port_defaults(self) -> None:
        root = self.make_root()
        (root / "experimental/pulp-rs/src/cmd/motion.rs").write_text(
            "pub const DEFAULT_INSPECTOR_PORT: u16 = 9147;\n"
            "use std::net::TcpStream;\n"
            "fn inspector_reachable(port: u16) -> bool { port > 0 }\n",
            encoding="utf-8",
        )
        (root / "experimental/pulp-rs/src/cmd/trace.rs").write_text(
            "pub const DEFAULT_INSPECTOR_PORT: u16 = 9147;\n"
            "use std::net::TcpStream;\n"
            "fn inspector_reachable(port: u16) -> bool { port > 0 }\n",
            encoding="utf-8",
        )
        (root / ".claude/commands/trace.md").write_text(
            "authenticated ephemeral discovery (default 9147)\n",
            encoding="utf-8",
        )
        errors = " ".join(inspector_truth_check.check_root(root))
        self.assertIn("cmd/motion.rs retains stale claim", errors)
        self.assertIn("cmd/trace.rs retains stale claim", errors)
        self.assertIn(".claude/commands/trace.md retains stale claim", errors)

    def test_rejects_fixed_port_and_unavailable_fixture_client_surfaces(self) -> None:
        root = self.make_root()
        (root / "docs/reference/cli.md").write_text(
            "unavailable in normal launches; explicitly wired; "
            "trace defaults to `9147`\n",
            encoding="utf-8",
        )
        (root / "docs/status/cli-commands.yaml").write_text(
            "trace defaults to 9147\n", encoding="utf-8"
        )
        guide = root / "docs/guides/motion-observability.md"
        guide.parent.mkdir(parents=True, exist_ok=True)
        guide.write_text(
            "each command probes `127.0.0.1:9147`\n", encoding="utf-8"
        )
        (root / "tools/mcp/pulp_mcp.cpp").write_text(
            '"name":"pulp_motion_load_fixture",'
            '"description":"Experimental source-checkout client"\n',
            encoding="utf-8",
        )
        errors = " ".join(inspector_truth_check.check_root(root))
        self.assertIn("docs/reference/cli.md retains stale claim", errors)
        self.assertIn("docs/status/cli-commands.yaml retains stale claim", errors)
        self.assertIn(
            "docs/guides/motion-observability.md retains stale claim", errors
        )
        self.assertIn("tools/mcp/pulp_mcp.cpp retains stale claim", errors)

    def test_rejects_stale_rust_help_claim(self) -> None:
        root = self.make_root()
        (root / "experimental/pulp-rs/src/help.rs").write_text(
            "Connect to a running plugin inspector\n", encoding="utf-8"
        )
        self.assertIn(
            "experimental/pulp-rs/src/help.rs retains stale claim",
            " ".join(inspector_truth_check.check_root(root)),
        )

    def test_rejects_nonexistent_rust_server_flags(self) -> None:
        root = self.make_root()
        (root / "experimental/pulp-rs/src/main.rs").write_text(
            "motion: PULP_MOTION_SERVER=1\n"
            "trace: PULP_TRACE_SERVER=1\n",
            encoding="utf-8",
        )
        errors = " ".join(inspector_truth_check.check_root(root))
        self.assertIn("main.rs retains stale claim: PULP_MOTION_SERVER=1", errors)
        self.assertIn("main.rs retains stale claim: PULP_TRACE_SERVER=1", errors)

    def test_rejects_stale_shipped_inspector_workflows(self) -> None:
        root = self.make_root()
        (root / ".agents/skills/motion/SKILL.md").write_text(
            "PULP_MOTION_SERVER=1\nRaw inspector wire\n",
            encoding="utf-8",
        )
        (root / ".agents/skills/trace-analysis/SKILL.md").write_text(
            "PULP_TRACE_SERVER=1\n", encoding="utf-8"
        )
        (root / ".agents/skills/cli-maintenance/SKILL.md").write_text(
            "inspector transport has no authentication\n",
            encoding="utf-8",
        )
        errors = " ".join(inspector_truth_check.check_root(root))
        self.assertIn(".agents/skills/motion/SKILL.md retains stale claim", errors)
        self.assertIn(
            ".agents/skills/trace-analysis/SKILL.md retains stale claim", errors
        )
        self.assertIn(
            ".agents/skills/cli-maintenance/SKILL.md retains stale claim", errors
        )

    def test_rejects_stale_migration_guide_claim(self) -> None:
        root = self.make_root()
        (root / "docs/guides/coming-from-reference.md").write_text(
            "It also speaks JSON-RPC over a local TCP port\n",
            encoding="utf-8",
        )
        self.assertIn(
            "docs/guides/coming-from-reference.md retains stale claim",
            " ".join(inspector_truth_check.check_root(root)),
        )

    def test_rejects_eval_and_policy_truth_drift(self) -> None:
        root = self.make_root()
        (root / ".claude/commands/inspect.md").write_text(
            "`Runtime.evaluate`, `Capture.screenshot`, and `Capture.screenshotNode`\n",
            encoding="utf-8",
        )
        (root / "docs/reference/development-inspector-capabilities.md").write_text(
            "| `state.read` | yes | yes | available |\n"
            "current dispatch does not enforce the registry\n",
            encoding="utf-8",
        )
        errors = " ".join(inspector_truth_check.check_root(root))
        self.assertIn("retains stale claim", errors)
        self.assertIn("omits required claim", errors)

    def test_rejects_overlay_only_build_gate_regression(self) -> None:
        root = self.make_root()
        (root / "CMakeLists.txt").write_text(
            "add_subdirectory(inspect)\n"
            "target_link_libraries(pulp-standalone PRIVATE pulp::inspect)\n",
            encoding="utf-8",
        )
        self.assertIn(
            "omits inspector build contract",
            " ".join(inspector_truth_check.check_root(root)),
        )

    def test_rejects_macos_zombie_liveness_regression(self) -> None:
        root = self.make_root()
        (root / "inspect/src/discovery.cpp").write_text(
            "return process start time without checking status\n",
            encoding="utf-8",
        )
        self.assertIn(
            "omits inspector security contract",
            " ".join(inspector_truth_check.check_root(root)),
        )

    def test_rejects_publisher_authority_in_the_reader_header(self) -> None:
        root = self.make_root()
        (root / "inspect/include/pulp/inspect/discovery.hpp").write_text(
            "class InspectorDiscoveryReader {};\n"
            "class InspectorDiscoveryPublisher {};\n",
            encoding="utf-8",
        )
        self.assertIn(
            "read-only discovery header exposes publisher authority",
            " ".join(inspector_truth_check.check_root(root)),
        )


if __name__ == "__main__":
    unittest.main()

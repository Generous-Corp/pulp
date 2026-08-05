#!/usr/bin/env python3

import ast
import pathlib
import tempfile
import unittest

import inspector_truth_check

TEST_REQUIRED_CLAIMS = {
    "docs/reference/development-inspector-capabilities.md": (
        "policy registry enforced",
    ),
    "tools/mcp/pulp_mcp.cpp": (
        '"minimum":1,"maximum":512',
    ),
}
TEST_REQUIRED_BUILD_CONTRACTS = {
    "CMakeLists.txt": (
        "PULP_ENABLE_INSPECTOR",
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
}


class InspectorTruthCheckTests(unittest.TestCase):
    def make_root(self) -> pathlib.Path:
        root = pathlib.Path(self.tempdir.name)
        files = {
            "inspect/include/pulp/inspect/capability_definitions.inc":
                'PULP_INSPECT_CAPABILITY(StateRead, "state.read", '
                '"dev.pulp.state/read@1", Observe, None, HostMain, Response, '
                '1, 1, 1, 0)\n',
            "docs/reference/development-inspector-capabilities.md":
                "| `state.read` | yes | yes | available |\n",
            "docs/guides/coming-from-reference.md": "visual overlay only\n",
            "inspect/include/pulp/inspect/discovery.hpp":
                "class InspectorDiscoveryReader {};\n",
            "inspect/include/pulp/inspect/discovery_publisher.hpp":
                "class InspectorDiscoveryPublisher {};\n",
            "tools/mcp/pulp_mcp.cpp":
                '"name":"pulp_inspect_dom","description":"Installed in-process client"\n'
                '"name":"pulp_motion_snapshot","description":"Experimental source-checkout client"\n',
        }
        canonical_contracts = (
            TEST_REQUIRED_CLAIMS,
            TEST_REQUIRED_BUILD_CONTRACTS,
        )
        for contract in canonical_contracts:
            for relative, requirements in contract.items():
                files.setdefault(relative, "")
                files[relative] += "\n".join(requirements) + "\n"
        fixture_digest = "a" * 64
        files["tools/cmake/PulpInspectorShipping.cmake"] += (
            "set(_PULP_INSPECTOR_SHIPPING_CAPABILITIES state.read)\n"
            "set(_PULP_CONTROL_CAPABILITIES dev.pulp.state/read@1)\n"
            f'set(_PULP_CONTROL_REGISTRY_DIGEST_V1 "{fixture_digest}")\n'
        )
        files["inspect/include/pulp/inspect/control_registry_digest.inc"] = (
            f'#define PULP_CONTROL_REGISTRY_DIGEST_V1 "{fixture_digest}"\n'
        )
        # These blocks exercise parsed link-authority behavior rather than an
        # exact required-source contract, so they remain hand-written.
        files.setdefault("inspect/CMakeLists.txt", "")
        files["inspect/CMakeLists.txt"] += (
            "target_link_libraries(pulp-inspect-publication\n"
            "  PUBLIC pulp::inspect-protocol pulp::inspect-discovery-support\n"
            ")\n"
            "target_link_libraries(pulp-inspect-runtime\n"
            "  PUBLIC pulp::inspect-publication\n"
            ")\n"
        )
        for relative in inspector_truth_check.FORBIDDEN_CLAIMS:
            files.setdefault(relative, "safe fixture\n")
        repository = pathlib.Path(inspector_truth_check.__file__).parents[2]
        for relative in inspector_truth_check.SECURITY_IMPLEMENTATION_PATHS:
            files[relative] = (repository / relative).read_text(encoding="utf-8")
        for relative, text in files.items():
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        return root

    def setUp(self) -> None:
        self.tempdir = tempfile.TemporaryDirectory()

    def tearDown(self) -> None:
        self.tempdir.cleanup()

    def check_root(self, root: pathlib.Path) -> list[str]:
        return inspector_truth_check.check_root(
            root,
            required_claims=TEST_REQUIRED_CLAIMS,
            required_build_contracts=TEST_REQUIRED_BUILD_CONTRACTS,
        )

    def mutate(
        self, root: pathlib.Path, relative: str, before: str, after: str
    ) -> None:
        path = root / relative
        text = path.read_text(encoding="utf-8")
        self.assertIn(before, text)
        path.write_text(text.replace(before, after, 1), encoding="utf-8")

    def test_accepts_truthful_fixture(self) -> None:
        self.assertEqual(self.check_root(self.make_root()), [])

    def test_requires_force_refreshed_control_declarations(self) -> None:
        root = self.make_root()
        self.mutate(
            root,
            "tools/cmake/PulpInspectorShipping.cmake",
            'set(PULP_${target}_CONTROL_CAPABILITIES "${capabilities}" CACHE INTERNAL "" FORCE)',
            'set(PULP_${target}_CONTROL_CAPABILITIES "${capabilities}" CACHE INTERNAL "")',
        )
        self.assertIn(
            "CONTROL_CAPABILITIES",
            " ".join(self.check_root(root)),
        )

    def test_contract_registries_have_no_duplicate_literal_keys(self) -> None:
        checker_path = pathlib.Path(inspector_truth_check.__file__)
        tree = ast.parse(checker_path.read_text(encoding="utf-8"))
        duplicates = []
        for node in ast.walk(tree):
            if not isinstance(node, ast.Dict):
                continue
            seen = set()
            for key in node.keys:
                if not (isinstance(key, ast.Constant) and
                        isinstance(key.value, str)):
                    continue
                if key.value in seen:
                    duplicates.append(key.value)
                seen.add(key.value)
        self.assertEqual(duplicates, [])

    def test_detects_exact_publication_authentication_mutation(self) -> None:
        root = self.make_root()
        self.mutate(
            root,
            "inspect/src/client.cpp",
            "challenge.publication_id != record.publication_id",
            "false",
        )
        self.assertIn(
            "authentication no longer binds the exact publication",
            " ".join(self.check_root(root)),
        )

    def test_detects_authentication_replay_mutation(self) -> None:
        root = self.make_root()
        self.mutate(
            root,
            "inspect/src/inspector_server.cpp",
            "found->second->verifier.reset();",
            "",
        )
        self.assertIn("verifier can be replayed", " ".join(self.check_root(root)))

    def test_detects_preauthentication_event_mutation(self) -> None:
        root = self.make_root()
        self.mutate(
            root,
            "inspect/src/client.cpp",
            "if (!event_state->authenticated) {",
            "if (false) {",
        )
        self.assertIn(
            "events are no longer quarantined", " ".join(self.check_root(root))
        )

    def test_detects_teardown_order_mutation(self) -> None:
        root = self.make_root()
        self.mutate(
            root,
            "inspect/src/inspector_server.cpp",
            "client->outbound->shutdown();",
            "client->outbound->request_stop();",
        )
        self.assertIn(
            "teardown no longer drains clients", " ".join(self.check_root(root))
        )

    def test_detects_cpp_exact_selection_mutation(self) -> None:
        root = self.make_root()
        self.mutate(
            root,
            "inspect/src/discovery_reader.cpp",
            "record.publication_id != publication_id",
            "false",
        )
        self.assertIn("complete identity", " ".join(self.check_root(root)))

    def test_detects_rust_exact_selection_mutation(self) -> None:
        root = self.make_root()
        self.mutate(
            root,
            "experimental/pulp-rs/src/cmd/inspector.rs",
            "is_some_and(|selection| !selection.publication_id.is_empty())",
            "is_some_and(|_| true)",
        )
        self.assertIn("requires a publication id", " ".join(self.check_root(root)))

    def test_rejects_omitted_capability(self) -> None:
        root = self.make_root()
        (root / "docs/reference/development-inspector-capabilities.md").write_text(
            "no capability table\n", encoding="utf-8"
        )
        self.assertIn("omit capability", " ".join(self.check_root(root)))

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
            " ".join(self.check_root(root)),
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
        errors = " ".join(self.check_root(root))
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
            '"name":"pulp_inspect_dom","description":"Experimental '
            'source-checkout client. Requires a custom host/test fixture that '
            'explicitly constructs an inspector endpoint"\n'
            '"name":"pulp_inspect_params","description":"Live plugin inspector"\n',
            encoding="utf-8",
        )
        capability_doc = root / "docs/reference/development-inspector-capabilities.md"
        capability_doc.write_text(
            capability_doc.read_text(encoding="utf-8")
            + "A normal `pulp run` constructs no network session.\n",
            encoding="utf-8",
        )
        errors = " ".join(self.check_root(root))
        self.assertIn("stale claim", errors)
        self.assertIn("A normal `pulp run`", errors)
        self.assertIn("Requires a custom host/test fixture", errors)
        self.assertIn("source-checkout-only", errors)

    def test_requires_installed_inspector_and_source_checkout_motion_paths(
        self,
    ) -> None:
        root = self.make_root()
        (root / "tools/mcp/pulp_mcp.cpp").write_text(
            '"name":"pulp_inspect_dom","description":"Experimental '
            'source-checkout client"\n'
            '"name":"pulp_inspect_params","description":"Authenticated client"\n'
            '"name":"pulp_motion_snapshot","description":"In-process client"\n',
            encoding="utf-8",
        )
        errors = " ".join(self.check_root(root))
        self.assertIn(
            "pulp_inspect_dom retains a source-checkout-only client path",
            errors,
        )
        self.assertIn(
            "pulp_inspect_params must disclose its installed in-process client path",
            errors,
        )
        self.assertIn(
            "pulp_motion_snapshot must disclose its source-checkout-only client path",
            errors,
        )

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
        errors = " ".join(self.check_root(root))
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
        errors = " ".join(self.check_root(root))
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
        errors = " ".join(self.check_root(root))
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
        errors = " ".join(self.check_root(root))
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
            " ".join(self.check_root(root)),
        )

    def test_rejects_nonexistent_rust_server_flags(self) -> None:
        root = self.make_root()
        (root / "experimental/pulp-rs/src/main.rs").write_text(
            "motion: PULP_MOTION_SERVER=1\n"
            "trace: PULP_TRACE_SERVER=1\n",
            encoding="utf-8",
        )
        errors = " ".join(self.check_root(root))
        self.assertIn("main.rs retains stale claim: PULP_MOTION_SERVER=1", errors)
        self.assertIn("main.rs retains stale claim: PULP_TRACE_SERVER=1", errors)

    def test_rejects_remote_trace_path_and_unbounded_ring_claims(self) -> None:
        root = self.make_root()
        (root / "docs/reference/cli.md").write_text(
            "unavailable in normal launches; explicitly wired\n"
            "pulp trace start --categories dsp,render --out /tmp/x.pftrace\n",
            encoding="utf-8",
        )
        (root / "tools/mcp/pulp_mcp.cpp").write_text(
            '"out_path":{"type":"string","description":"Explicit .pftrace output path"\n',
            encoding="utf-8",
        )
        errors = " ".join(self.check_root(root))
        self.assertIn("docs/reference/cli.md retains stale claim", errors)
        self.assertIn("tools/mcp/pulp_mcp.cpp retains stale claim", errors)
        self.assertIn("omits required claim", errors)

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
        errors = " ".join(self.check_root(root))
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
            " ".join(self.check_root(root)),
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
        errors = " ".join(self.check_root(root))
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
            " ".join(self.check_root(root)),
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
            " ".join(self.check_root(root)),
        )


if __name__ == "__main__":
    unittest.main()

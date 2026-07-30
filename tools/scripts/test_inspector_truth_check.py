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
            "docs/reference/scripted-ui-inspector.md":
                "loopback only; nonce/HMAC proof; "
                "owner-private per-session credential\n",
            "docs/guides/coming-from-reference.md": "visual overlay only\n",
            ".claude/commands/inspect.md":
                "unavailable in normal launches; explicitly wired custom fixture\n",
            "docs/agent-integrations.md":
                "unavailable in normal launches; explicitly wired custom fixture\n",
            "docs/reference/cli.md":
                "unavailable in normal launches; explicitly wired\n",
            "docs/status/cli-commands.yaml":
                "owner-private authenticated discovery\n",
            "tools/mcp/pulp_mcp.cpp":
                '"name":"pulp_inspect_dom","description":"Experimental source-checkout client"\n',
            "CMakeLists.txt":
                "if(PULP_ENABLE_INSPECTOR)\n"
                "    add_subdirectory(inspect)\n"
                "endif()\n"
                "if(PULP_ENABLE_INSPECTOR AND TARGET pulp::inspect AND NOT IOS)\n"
                "target_link_libraries(pulp-standalone PRIVATE pulp::inspect)\n",
            "inspect/CMakeLists.txt":
                "if(PULP_ENABLE_GPU AND NOT ANDROID AND NOT IOS)\n",
            "tools/cli/CMakeLists.txt":
                "cmd_inspect_unavailable.cpp\ncmd_tweaks_unavailable.cpp\n",
            "test/cmake/view_widget_bridge_tests.cmake":
                "pulp-test-inspector-stripped-artifact\n"
                "check_inspector_stripped_artifact.cmake\n",
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

    def test_rejects_stale_rust_help_claim(self) -> None:
        root = self.make_root()
        (root / "experimental/pulp-rs/src/help.rs").write_text(
            "Connect to a running plugin inspector\n", encoding="utf-8"
        )
        self.assertIn(
            "experimental/pulp-rs/src/help.rs retains stale claim",
            " ".join(inspector_truth_check.check_root(root)),
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


if __name__ == "__main__":
    unittest.main()

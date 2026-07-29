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
                "classified unavailable for future policy; "
                "current dispatch does not enforce the registry\n",
            "tools/cli/cmd_inspect.cpp": "custom fixture only\n",
            "tools/cli/pulp_cli.cpp":
                "Connect to an explicitly hosted inspector fixture\n",
            "experimental/pulp-rs/src/help.rs":
                "Connect to an explicitly hosted inspector fixture\n",
            "docs/reference/scripted-ui-inspector.md": "loopback only\n",
            "docs/guides/coming-from-juce.md": "visual overlay only\n",
            ".claude/commands/inspect.md":
                "unavailable in normal launches; explicitly wired custom fixture\n",
            "docs/agent-integrations.md":
                "unavailable in normal launches; explicitly wired custom fixture\n",
            "docs/reference/cli.md":
                "unavailable in normal launches; explicitly wired\n",
            "tools/mcp/pulp_mcp.cpp":
                '"name":"pulp_inspect_dom","description":"Experimental source-checkout client"\n',
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

    def test_rejects_stale_rust_help_claim(self) -> None:
        root = self.make_root()
        (root / "experimental/pulp-rs/src/help.rs").write_text(
            "Connect to a running plugin inspector\n", encoding="utf-8"
        )
        self.assertIn(
            "experimental/pulp-rs/src/help.rs retains stale claim",
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
            "filesystem and editor-launch methods are unavailable\n",
            encoding="utf-8",
        )
        errors = " ".join(inspector_truth_check.check_root(root))
        self.assertIn("retains stale claim", errors)
        self.assertIn("omits required claim", errors)


if __name__ == "__main__":
    unittest.main()

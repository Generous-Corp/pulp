#!/usr/bin/env python3
"""Unit tests for tools/scripts/check_cli_mcp_parity.py."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import os
import pathlib
import runpy
import sys
import tempfile
import unittest
from unittest import mock


SCRIPT = pathlib.Path(__file__).parent / "check_cli_mcp_parity.py"
spec = importlib.util.spec_from_file_location("check_cli_mcp_parity", SCRIPT)
assert spec and spec.loader
parity = importlib.util.module_from_spec(spec)
sys.modules["check_cli_mcp_parity"] = parity
spec.loader.exec_module(parity)


@contextlib.contextmanager
def chdir(path: pathlib.Path):
    old = pathlib.Path.cwd()
    os.chdir(path)
    try:
        yield
    finally:
        os.chdir(old)


def make_repo(root: pathlib.Path) -> pathlib.Path:
    """Create a minimal Pulp-shaped fake repo skeleton."""
    root.mkdir(parents=True)
    (root / "core").mkdir()
    (root / "CMakeLists.txt").write_text("cmake_minimum_required(VERSION 3.24)\n")
    (root / "tools" / "cli").mkdir(parents=True)
    (root / "tools" / "mcp").mkdir(parents=True)
    (root / "tools" / "scripts").mkdir(parents=True)
    return root


def write_cli(root: pathlib.Path, *commands: str, with_audit: bool = False) -> None:
    cli_path = root / "tools" / "cli" / "pulp_cli.cpp"
    body_lines = [f'    {{"{c}", "summary", handle_{c.replace("-", "_")}}},' for c in commands]
    body = "\n".join(body_lines)
    extra = ""
    if with_audit:
        extra = '\nif (command == "audit") return handle_audit(args);\n'
    cli_path.write_text(
        "static const Command commands[] = {\n"
        f"{body}\n"
        "};\n"
        f"{extra}"
    )


def write_mcp(root: pathlib.Path, *tool_names: str) -> None:
    mcp_path = root / "tools" / "mcp" / "pulp_mcp.cpp"
    body = ",\n".join(
        f'{{"name":"{name}","description":"x","inputSchema":{{}}}}' for name in tool_names
    )
    mcp_path.write_text(
        "static std::string tools_list_json() {\n"
        f'    return R"JSON({{"tools":[{body}]}})JSON";\n'
        "}\n"
    )


def write_baseline(root: pathlib.Path, cli_only: dict, mcp_only: dict) -> pathlib.Path:
    bl = root / "tools" / "scripts" / "cli_mcp_parity_baseline.json"
    bl.write_text(
        json.dumps({"cli_only": cli_only, "mcp_only": mcp_only}, indent=2)
    )
    return bl


def make_diff(**overrides) -> parity.Diff:
    defaults = {
        "cli_commands": {"build"},
        "mcp_tools": {"pulp_build"},
        "new_cli_only": set(),
        "new_mcp_only": set(),
        "accepted_cli_only": set(),
        "accepted_mcp_only": set(),
        "stale_cli_only_baseline": set(),
        "stale_mcp_only_baseline": set(),
    }
    defaults.update(overrides)
    return parity.Diff(**defaults)


# ── Tests ───────────────────────────────────────────────────────────────


class FindRepoRoot(unittest.TestCase):
    def test_walks_up_to_find_pulp_repo(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = make_repo(pathlib.Path(td) / "repo")
            found = parity.find_repo_root(root / "tools" / "cli")
            self.assertIsNotNone(found)
            # macOS resolves /var → /private/var; compare resolved paths.
            self.assertEqual(found.resolve(), root.resolve())

    def test_returns_none_outside_repo(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            self.assertIsNone(parity.find_repo_root(pathlib.Path(td)))


class CliExtractor(unittest.TestCase):
    def test_extracts_table_commands(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = make_repo(pathlib.Path(td) / "repo")
            write_cli(root, "build", "test", "ship")
            self.assertEqual(
                parity.extract_cli_commands(root / "tools" / "cli" / "pulp_cli.cpp"),
                {"build", "test", "ship"},
            )

    def test_extracts_manual_dispatch_commands(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = make_repo(pathlib.Path(td) / "repo")
            write_cli(root, "build", with_audit=True)
            self.assertEqual(
                parity.extract_cli_commands(root / "tools" / "cli" / "pulp_cli.cpp"),
                {"build", "audit"},
            )

    def test_extracts_script_and_binary_command_tables(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = make_repo(pathlib.Path(td) / "repo")
            cli_path = root / "tools" / "cli" / "pulp_cli.cpp"
            cli_path.write_text(
                "static const Command commands[] = {\n"
                '    {"build", "x", h_build},\n'
                "};\n"
                "static const ScriptCommand script_commands[] = {\n"
                '    {"loop", "x", h_loop},\n'
                "};\n"
                "static const BinaryCommand binary_commands[] = {\n"
                '    {"mcp", "x", h_mcp},\n'
                "};\n"
            )

            self.assertEqual(
                parity.extract_cli_commands(cli_path),
                {"build", "loop", "mcp"},
            )

    def test_skips_hidden_aliases(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = make_repo(pathlib.Path(td) / "repo")
            cli_path = root / "tools" / "cli" / "pulp_cli.cpp"
            cli_path.write_text(
                "static const Command commands[] = {\n"
                '    {"build", "x", h_build},\n'
                "};\n"
                'if (command == "add-component") return foo();\n'
                'if (command == "install") return bar();\n'
            )
            self.assertEqual(
                parity.extract_cli_commands(cli_path),
                {"build"},
            )

    def test_returns_empty_for_missing_file(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            self.assertEqual(
                parity.extract_cli_commands(pathlib.Path(td) / "missing.cpp"),
                set(),
            )


class McpExtractor(unittest.TestCase):
    def test_extracts_tool_names(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = make_repo(pathlib.Path(td) / "repo")
            write_mcp(root, "pulp_build", "pulp_test", "pulp_inspect_dom")
            self.assertEqual(
                parity.extract_mcp_tools(root / "tools" / "mcp" / "pulp_mcp.cpp"),
                {"pulp_build", "pulp_test", "pulp_inspect_dom"},
            )

    def test_extract_mcp_tools_ignores_non_pulp_names(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = make_repo(pathlib.Path(td) / "repo")
            mcp_path = root / "tools" / "mcp" / "pulp_mcp.cpp"
            mcp_path.write_text(
                '"name":"pulp_build"\n'
                '"name":"other_tool"\n'
                '"name":"pulp_test"\n'
            )

            self.assertEqual(
                parity.extract_mcp_tools(mcp_path),
                {"pulp_build", "pulp_test"},
            )

    def test_returns_empty_for_missing_file(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            self.assertEqual(
                parity.extract_mcp_tools(pathlib.Path(td) / "missing.cpp"),
                set(),
            )


class NameMapping(unittest.TestCase):
    def test_cli_to_mcp_hyphenates(self) -> None:
        self.assertEqual(parity.cli_to_mcp_name("import-design"), "pulp_import_design")
        self.assertEqual(parity.cli_to_mcp_name("build"), "pulp_build")

    def test_mcp_to_cli_underscores(self) -> None:
        self.assertEqual(parity.mcp_to_cli_name("pulp_import_design"), "import-design")
        self.assertEqual(parity.mcp_to_cli_name("pulp_build"), "build")
        # Defensive: non-prefixed names pass through.
        self.assertEqual(parity.mcp_to_cli_name("foo"), "foo")


class BaselineLoader(unittest.TestCase):
    def test_missing_baseline_yields_empty(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            self.assertEqual(
                parity.load_baseline(pathlib.Path(td) / "x.json").cli_only, {}
            )

    def test_malformed_baseline_raises(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            p = pathlib.Path(td) / "bad.json"
            p.write_text("[]")
            with self.assertRaises(ValueError):
                parity.load_baseline(p)

    def test_non_object_baseline_sections_raise(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            p = pathlib.Path(td) / "bad.json"
            for payload in (
                {"cli_only": ["ship"], "mcp_only": {}},
                {"cli_only": {}, "mcp_only": ["pulp_ship"]},
            ):
                with self.subTest(payload=payload):
                    p.write_text(json.dumps(payload), encoding="utf-8")
                    with self.assertRaisesRegex(ValueError, "cli_only/mcp_only"):
                        parity.load_baseline(p)

    def test_missing_baseline_sections_default_to_empty(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            p = pathlib.Path(td) / "baseline.json"
            p.write_text("{}", encoding="utf-8")

            baseline = parity.load_baseline(p)

        self.assertEqual(baseline.cli_only, {})
        self.assertEqual(baseline.mcp_only, {})


class DiffComputation(unittest.TestCase):
    def test_clean_when_each_cli_has_corresponding_mcp(self) -> None:
        diff = parity.compute_diff(
            cli_commands={"build", "test"},
            mcp_tools={"pulp_build", "pulp_test"},
            baseline=parity.Baseline(),
        )
        self.assertEqual(diff.new_cli_only, set())
        self.assertEqual(diff.new_mcp_only, set())

    def test_unbaselined_cli_command_flagged_as_new_gap(self) -> None:
        diff = parity.compute_diff(
            cli_commands={"build", "ship"},
            mcp_tools={"pulp_build"},
            baseline=parity.Baseline(),
        )
        self.assertEqual(diff.new_cli_only, {"ship"})

    def test_baselined_cli_command_accepted_silently(self) -> None:
        diff = parity.compute_diff(
            cli_commands={"build", "ship"},
            mcp_tools={"pulp_build"},
            baseline=parity.Baseline(cli_only={"ship": "trivial via Bash"}),
        )
        self.assertEqual(diff.new_cli_only, set())
        self.assertEqual(diff.accepted_cli_only, {"ship"})

    def test_unbaselined_mcp_only_tool_warns_but_is_not_a_failure(self) -> None:
        diff = parity.compute_diff(
            cli_commands={"build"},
            mcp_tools={"pulp_build", "pulp_inspect_dom"},
            baseline=parity.Baseline(),
        )
        self.assertEqual(diff.new_cli_only, set())
        self.assertEqual(diff.new_mcp_only, {"pulp_inspect_dom"})

    def test_stale_baseline_entries_are_reported(self) -> None:
        diff = parity.compute_diff(
            cli_commands={"build"},
            mcp_tools={"pulp_build"},
            baseline=parity.Baseline(cli_only={"ship": "ships gone"}),
        )
        self.assertEqual(diff.stale_cli_only_baseline, {"ship"})

    def test_package_manager_commands_are_excluded_from_parity_surface(self) -> None:
        # `remove`, `list`, `search`, etc. are package-manager commands;
        # they should not count as gaps even with no MCP exposure and no
        # baseline entry.
        diff = parity.compute_diff(
            cli_commands={"build", "remove", "list", "search", "update", "suggest", "target"},
            mcp_tools={"pulp_build"},
            baseline=parity.Baseline(),
        )
        self.assertEqual(diff.new_cli_only, set())

    def test_mcp_only_baseline_accepts_and_stales_entries(self) -> None:
        diff = parity.compute_diff(
            cli_commands={"build"},
            mcp_tools={"pulp_build", "pulp_inspect_dom"},
            baseline=parity.Baseline(mcp_only={
                "pulp_inspect_dom": "inspector-only",
                "pulp_old_tool": "removed",
            }),
        )

        self.assertEqual(diff.new_mcp_only, set())
        self.assertEqual(diff.accepted_mcp_only, {"pulp_inspect_dom"})
        self.assertEqual(diff.stale_mcp_only_baseline, {"pulp_old_tool"})


class RenderTextTests(unittest.TestCase):
    def test_render_text_colorizes_failure_warning_and_stale_sections(self) -> None:
        diff = make_diff(
            cli_commands={"build", "ship"},
            mcp_tools={"pulp_build", "pulp_inspect_dom"},
            new_cli_only={"ship"},
            new_mcp_only={"pulp_inspect_dom"},
            stale_cli_only_baseline={"old-cli"},
            stale_mcp_only_baseline={"pulp_old"},
        )

        rendered = parity.render_text(diff, color=True, mode="report")

        self.assertIn("\033[31m", rendered)
        self.assertIn("expected MCP tool: pulp_ship", rendered)
        self.assertIn("MCP tool(s) without CLI", rendered)
        self.assertIn("cli_only.old-cli", rendered)
        self.assertIn("mcp_only.pulp_old", rendered)
        self.assertIn("FAIL: new CLI", rendered)

    def test_render_text_hint_mode_explains_non_blocking_gap(self) -> None:
        rendered = parity.render_text(
            make_diff(new_cli_only={"ship"}, cli_commands={"ship"}, mcp_tools=set()),
            color=False,
            mode="hint",
        )

        self.assertIn("HINT: new CLI", rendered)
        self.assertNotIn("\033[", rendered)

    def test_color_helper_can_be_disabled(self) -> None:
        self.assertEqual(parity._color("32", "OK", enabled=False), "OK")
        self.assertEqual(parity._color("32", "OK", enabled=True), "\033[32mOK\033[0m")


class MainExitCodes(unittest.TestCase):
    def _run(self, root: pathlib.Path, *cli_args: str) -> tuple[int, str, str]:
        out = io.StringIO()
        err = io.StringIO()
        argv = [
            "--no-color",
            "--repo-root",
            str(root),
            *cli_args,
        ]
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = parity.main(argv)
        return rc, out.getvalue(), err.getvalue()

    def test_report_mode_passes_when_clean(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = make_repo(pathlib.Path(td) / "repo")
            write_cli(root, "build")
            write_mcp(root, "pulp_build")
            write_baseline(root, {}, {})
            rc, out, _ = self._run(root, "--mode=report")
            self.assertEqual(rc, 0)
            self.assertIn("OK: parity check clean.", out)

    def test_report_mode_fails_on_new_cli_gap(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = make_repo(pathlib.Path(td) / "repo")
            write_cli(root, "build", "ship")
            write_mcp(root, "pulp_build")
            write_baseline(root, {}, {})
            rc, out, _ = self._run(root, "--mode=report")
            self.assertEqual(rc, 1)
            self.assertIn("ship", out)
            self.assertIn("FAIL", out)

    def test_report_mode_passes_with_baselined_gap(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = make_repo(pathlib.Path(td) / "repo")
            write_cli(root, "build", "ship")
            write_mcp(root, "pulp_build")
            write_baseline(root, {"ship": "intentional"}, {})
            rc, out, _ = self._run(root, "--mode=report")
            self.assertEqual(rc, 0)
            self.assertIn("OK", out)

    def test_hint_mode_returns_zero_even_on_gap(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = make_repo(pathlib.Path(td) / "repo")
            write_cli(root, "build", "ship")
            write_mcp(root, "pulp_build")
            write_baseline(root, {}, {})
            rc, out, _ = self._run(root, "--mode=hint")
            self.assertEqual(rc, 0)
            self.assertIn("HINT", out)
            self.assertIn("ship", out)

    def test_json_mode_emits_machine_readable_payload(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = make_repo(pathlib.Path(td) / "repo")
            write_cli(root, "build", "ship")
            write_mcp(root, "pulp_build")
            write_baseline(root, {"ship": "intentional"}, {})
            rc, out, _ = self._run(root, "--mode=report", "--json")
            self.assertEqual(rc, 0)
            payload = json.loads(out)
            self.assertEqual(payload["mode"], "report")
            self.assertEqual(payload["cli_commands"], ["build", "ship"])
            self.assertEqual(payload["mcp_tools"], ["pulp_build"])
            self.assertEqual(payload["accepted_cli_only"], ["ship"])
            self.assertEqual(payload["new_cli_only"], [])

    def test_main_uses_source_and_baseline_overrides(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = make_repo(pathlib.Path(td) / "repo")
            cli = root / "custom_cli.cpp"
            mcp = root / "custom_mcp.cpp"
            cli.write_text('static const Command commands[] = {\n    {"ship", "x", h},\n};\n')
            mcp.write_text('return R"JSON({"tools":[]})JSON";\n')
            baseline = root / "custom_baseline.json"
            baseline.write_text(json.dumps({"cli_only": {"ship": "manual"}, "mcp_only": {}}))

            rc, out, _ = self._run(
                root,
                "--mode=report",
                "--cli-source", str(cli),
                "--mcp-source", str(mcp),
                "--baseline", str(baseline),
            )

        self.assertEqual(rc, 0)
        self.assertIn("allow-list covers 1", out)

    def test_script_entrypoint_propagates_main_status(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = make_repo(pathlib.Path(td) / "repo")
            write_cli(root, "ship")
            write_mcp(root)
            write_baseline(root, {}, {})
            with mock.patch.object(
                sys,
                "argv",
                [str(SCRIPT), "--repo-root", str(root), "--mode=report", "--no-color"],
            ), contextlib.redirect_stdout(io.StringIO()):
                with self.assertRaises(SystemExit) as cm:
                    runpy.run_path(str(SCRIPT), run_name="__main__")

        self.assertEqual(cm.exception.code, 1)

    def test_no_repo_root_returns_one(self) -> None:
        with tempfile.TemporaryDirectory() as td, chdir(pathlib.Path(td)):
            err = io.StringIO()
            with contextlib.redirect_stderr(err):
                rc = parity.main(["--mode=report"])
            self.assertEqual(rc, 1)
            self.assertIn("not in a Pulp project", err.getvalue())


class IntegrationAgainstRealRepo(unittest.TestCase):
    """Smoke test against the real Pulp repo: today's baseline must keep
    the gate green. If this test ever fails locally, either a new CLI
    command landed without an MCP tool or the baseline drifted."""

    def test_real_repo_passes_report_mode(self) -> None:
        repo = parity.find_repo_root(pathlib.Path(__file__).parent)
        if repo is None:
            self.skipTest("not in a Pulp checkout")
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = parity.main(["--mode=report", "--no-color", "--repo-root", str(repo)])
        self.assertEqual(rc, 0, f"parity check failed:\n{out.getvalue()}")


if __name__ == "__main__":
    unittest.main(verbosity=2)

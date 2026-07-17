#!/usr/bin/env python3
"""Tests for tools/scripts/tools_registry_check.py.

The registry's whole value is that it can't quietly become a lie, so the tests
concentrate on the invariants that fail LOUDLY:

  * the coverage sweep catches an unregistered tool (the anti-rot gate)
  * the digest sync check catches drift
  * a `planned` entry can never carry an invocation (never hand an agent a
    command for something that isn't in the tree)
  * a broken invocation / missing path / stale exclusion is caught

Plus a smoke test that the REAL registry is clean.

Run:
    python3 tools/scripts/test_tools_registry_check.py
"""
from __future__ import annotations

import importlib.util
import pathlib
import subprocess
import sys
import tempfile
import unittest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "tools/scripts/tools_registry_check.py"


def _load():
    spec = importlib.util.spec_from_file_location("tools_registry_check", SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["tools_registry_check"] = mod
    spec.loader.exec_module(mod)
    return mod


MOD = _load()


def _tool(**over) -> dict:
    base = {
        "name": "montage",
        "path": "tools/import-design/montage.py",
        "category": "visual-compare",
        "use_when": "Stack reference vs render into one labeled image.",
        "invocation": "python3 tools/import-design/montage.py --out c.png a.png:A",
        "availability": "always",
        "skill": "screenshot",
    }
    base.update(over)
    return base


class EntryPointDetection(unittest.TestCase):
    """A tool is something you can RUN; a library module is not."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = pathlib.Path(self.tmp.name)
        self.addCleanup(self.tmp.cleanup)

    def _write(self, name: str, text: str) -> pathlib.Path:
        p = self.dir / name
        p.write_text(text)
        return p

    def test_python_with_main_guard_is_entry_point(self):
        p = self._write("t.py", 'import sys\nif __name__ == "__main__":\n    sys.exit(0)\n')
        self.assertTrue(MOD.is_entry_point(p))

    def test_python_single_quoted_main_guard_is_entry_point(self):
        p = self._write("t.py", "if __name__ == '__main__':\n    pass\n")
        self.assertTrue(MOD.is_entry_point(p))

    def test_library_module_is_not_an_entry_point(self):
        p = self._write("lib.py", "def helper():\n    return 1\n")
        self.assertFalse(MOD.is_entry_point(p))

    def test_shell_script_is_entry_point(self):
        p = self._write("t.sh", "#!/usr/bin/env bash\necho hi\n")
        self.assertTrue(MOD.is_entry_point(p))

    def test_mjs_with_shebang_is_entry_point(self):
        p = self._write("t.mjs", "#!/usr/bin/env node\nconsole.log(1)\n")
        self.assertTrue(MOD.is_entry_point(p))

    def test_mjs_module_without_main_is_not_entry_point(self):
        p = self._write("shim.mjs", "export function x() { return 1 }\n")
        self.assertFalse(MOD.is_entry_point(p))

    def test_non_script_suffix_is_not_entry_point(self):
        p = self._write("notes.md", "# hi\n")
        self.assertFalse(MOD.is_entry_point(p))

    def test_test_files_are_skipped_by_path(self):
        for rel in ("test_foo.py", "foo.test.mjs", "tests/foo.py",
                    "pkg/tests/bar.py", "conftest.py"):
            self.assertTrue(MOD._is_test_path(rel), rel)
        self.assertFalse(MOD._is_test_path("tools/import-design/montage.py"))


class CoverageSweep(unittest.TestCase):
    """The anti-rot gate: a new tool that nobody registered must fail."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = pathlib.Path(self.tmp.name)
        (root / "swept").mkdir()
        (root / "swept" / "registered.py").write_text('if __name__ == "__main__":\n    pass\n')
        (root / "swept" / "helper.py").write_text("def x():\n    return 1\n")
        self._real_root = MOD.ROOT
        MOD.ROOT = root
        self.root = root
        self.addCleanup(self._restore)

    def _restore(self):
        MOD.ROOT = self._real_root
        self.tmp.cleanup()

    def _registry(self, tools, exclude=None) -> dict:
        return {"sweep": {"include_dirs": ["swept"]}, "tools": tools,
                "exclude": exclude or []}

    def test_registered_tool_passes(self):
        reg = self._registry([{"name": "r", "path": "swept/registered.py"}])
        self.assertEqual(MOD.check_coverage(reg), [])

    def test_unregistered_tool_is_caught(self):
        (self.root / "swept" / "sneaky.py").write_text('if __name__ == "__main__":\n    pass\n')
        reg = self._registry([{"name": "r", "path": "swept/registered.py"}])
        problems = MOD.check_coverage(reg)
        self.assertEqual(len(problems), 1)
        self.assertIn("UNREGISTERED TOOL", problems[0])
        self.assertIn("sneaky.py", problems[0])

    def test_unregistered_tool_may_be_excluded_with_a_reason(self):
        (self.root / "swept" / "sneaky.py").write_text('if __name__ == "__main__":\n    pass\n')
        reg = self._registry(
            [{"name": "r", "path": "swept/registered.py"}],
            exclude=[{"path": "swept/sneaky.py", "reason": "fixture generator"}])
        self.assertEqual(MOD.check_coverage(reg), [])

    def test_exclusion_without_a_reason_is_caught(self):
        (self.root / "swept" / "sneaky.py").write_text('if __name__ == "__main__":\n    pass\n')
        reg = self._registry([{"name": "r", "path": "swept/registered.py"}],
                             exclude=[{"path": "swept/sneaky.py"}])
        self.assertTrue(any("needs a `reason`" in p for p in MOD.check_coverage(reg)))

    def test_library_module_needs_no_registration(self):
        # helper.py has no main guard — sweeping it in would drown the registry.
        reg = self._registry([{"name": "r", "path": "swept/registered.py"}])
        self.assertEqual(MOD.check_coverage(reg), [])

    def test_directory_path_covers_entry_points_beneath_it(self):
        pkg = self.root / "swept" / "pkg"
        pkg.mkdir()
        (pkg / "cli.py").write_text('if __name__ == "__main__":\n    pass\n')
        reg = self._registry([{"name": "r", "path": "swept/registered.py"},
                              {"name": "p", "path": "swept/pkg"}])
        self.assertEqual(MOD.check_coverage(reg), [])

    def test_stale_exclusion_for_a_deleted_file_is_caught(self):
        reg = self._registry([{"name": "r", "path": "swept/registered.py"}],
                             exclude=[{"path": "swept/gone.py", "reason": "x"}])
        self.assertTrue(any("no longer exists" in p for p in MOD.check_coverage(reg)))

    def test_exclusion_of_a_non_entry_point_is_a_noop_and_caught(self):
        reg = self._registry([{"name": "r", "path": "swept/registered.py"}],
                             exclude=[{"path": "swept/helper.py", "reason": "x"}])
        self.assertTrue(any("no-op" in p for p in MOD.check_coverage(reg)))


class SweepIsGitTracked(unittest.TestCase):
    """The registry describes the COMMITTED repo, not somebody's working tree.

    In a shared worktree several agents edit at once; sweeping untracked files
    would make one agent's WIP script fail another agent's push.
    """

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmp.name)
        (self.root / "swept").mkdir()
        self._real_root = MOD.ROOT
        MOD.ROOT = self.root
        self.addCleanup(self._restore)

    def _restore(self):
        MOD.ROOT = self._real_root
        self.tmp.cleanup()

    def _git(self, *args):
        subprocess.run(["git", "-C", str(self.root), *args],
                       check=True, capture_output=True)

    def _tool_file(self, name: str):
        (self.root / "swept" / name).write_text('if __name__ == "__main__":\n    pass\n')

    def _registry(self):
        return {"sweep": {"include_dirs": ["swept"]}, "tools": [], "exclude": []}

    def test_untracked_wip_is_not_swept_but_committed_tool_is(self):
        self._git("init", "-q")
        self._git("config", "user.email", "t@example.com")
        self._git("config", "user.name", "t")
        self._tool_file("wip.py")
        # Untracked: another agent's in-flight work — must not fail this gate.
        self.assertEqual(MOD.check_coverage(self._registry()), [])
        self._git("add", "swept/wip.py")
        self._git("commit", "-qm", "add tool")
        # Committed: now it is a tool, and the sweep fires.
        problems = MOD.check_coverage(self._registry())
        self.assertTrue(any("wip.py" in p and "UNREGISTERED TOOL" in p
                            for p in problems))

    def test_falls_back_to_a_filesystem_walk_outside_git(self):
        # A source tarball has no .git; the sweep must still work.
        self._tool_file("shipped.py")
        self.assertIsNone(MOD._tracked_files())
        problems = MOD.check_coverage(self._registry())
        self.assertTrue(any("shipped.py" in p for p in problems))


class Validation(unittest.TestCase):
    def _check(self, tool) -> list[str]:
        return MOD.validate({"tools": [tool]})

    def test_good_entry_passes(self):
        self.assertEqual(self._check(_tool()), [])

    def test_missing_path_is_caught(self):
        p = self._check(_tool(path="tools/import-design/nope.py"))
        self.assertTrue(any("path does not exist" in x for x in p))

    def test_invocation_to_a_missing_script_is_caught(self):
        p = self._check(_tool(invocation="python3 tools/import-design/nope.py"))
        self.assertTrue(any("missing script" in x for x in p))

    def test_unknown_interpreter_is_caught(self):
        p = self._check(_tool(invocation="./montage.py --out x"))
        self.assertTrue(any("unknown interpreter" in x for x in p))

    def test_bad_availability_is_caught(self):
        p = self._check(_tool(availability="maybe"))
        self.assertTrue(any("availability 'maybe'" in x for x in p))

    def test_missing_use_when_is_caught(self):
        p = self._check(_tool(use_when=""))
        self.assertTrue(any("use_when" in x for x in p))

    def test_nonexistent_skill_is_caught(self):
        p = self._check(_tool(skill="no-such-skill"))
        self.assertTrue(any("no-such-skill" in x for x in p))

    def test_missing_docs_page_is_caught(self):
        p = self._check(_tool(docs="guides/does-not-exist.md"))
        self.assertTrue(any("missing docs/" in x for x in p))

    def test_duplicate_names_are_caught(self):
        p = MOD.validate({"tools": [_tool(), _tool()]})
        self.assertTrue(any("duplicate name" in x for x in p))

    def test_optional_install_requires_install_field(self):
        p = self._check(_tool(availability="optional-install"))
        self.assertTrue(any("requires an `install`" in x for x in p))


class PlannedEntries(unittest.TestCase):
    """The registry must NEVER hand an agent a command for an absent tool."""

    def _planned(self, **over) -> dict:
        base = {"name": "layout-parity", "category": "visual-compare",
                "use_when": "Report misplaced nodes by id.",
                "availability": "planned", "pointer": "planning/plan.md — Phase 2."}
        base.update(over)
        return base

    def test_planned_with_pointer_passes(self):
        self.assertEqual(MOD.validate({"tools": [self._planned()]}), [])

    def test_planned_with_an_invocation_is_rejected(self):
        p = MOD.validate({"tools": [self._planned(invocation="python3 x.py")]})
        self.assertTrue(any("must NOT carry an `invocation`" in x for x in p))

    def test_planned_without_a_pointer_is_rejected(self):
        p = MOD.validate({"tools": [self._planned(pointer="")]})
        self.assertTrue(any("requires a `pointer`" in x for x in p))

    def test_planned_claiming_a_path_is_rejected(self):
        p = MOD.validate({"tools": [self._planned(path="tools/x.py")]})
        self.assertTrue(any("must not claim a `path`" in x for x in p))


class PulpInvocations(unittest.TestCase):
    """`pulp ...` invocations are cross-checked against cli-commands.yaml."""

    CLI = {"audio": ["scope", "validate summarize", "render"], "build": []}

    def test_known_command_and_subcommand(self):
        self.assertIsNone(MOD.check_invocation(
            "pulp audio validate summarize <out.wav> [--json]", self.CLI))

    def test_single_word_subcommand(self):
        self.assertIsNone(MOD.check_invocation(
            "pulp audio scope --input-wav <f.wav>", self.CLI))

    def test_command_without_subcommands(self):
        self.assertIsNone(MOD.check_invocation("pulp build", self.CLI))

    def test_flag_only_is_allowed(self):
        self.assertIsNone(MOD.check_invocation("pulp audio --help", self.CLI))

    def test_unknown_command_is_caught(self):
        self.assertIn("not a command",
                      MOD.check_invocation("pulp bogus thing", self.CLI) or "")

    def test_unknown_subcommand_is_caught(self):
        self.assertIn("matches no subcommand",
                      MOD.check_invocation("pulp audio bogus", self.CLI) or "")

    def test_bare_pulp_is_caught(self):
        self.assertIn("no command", MOD.check_invocation("pulp", self.CLI) or "")


class DigestHandle(unittest.TestCase):
    """The digest must name the thing you TYPE, not a source file."""

    def test_script_invocation_drops_the_interpreter(self):
        self.assertEqual(
            MOD.digest_handle("python3 tools/import-design/montage.py --out c.png"),
            "tools/import-design/montage.py")

    def test_cli_invocation_keeps_the_command_path(self):
        self.assertEqual(
            MOD.digest_handle("pulp audio validate summarize <out.wav> [--json]"),
            "pulp audio validate summarize")

    def test_module_invocation_keeps_the_dash_m_prefix(self):
        self.assertEqual(
            MOD.digest_handle("python -m quality_lab.cli compare --reference r.wav"),
            "python -m quality_lab.cli compare")

    def test_subcommand_is_kept(self):
        self.assertEqual(
            MOD.digest_handle("node tools/import-design/fig_decode.mjs emit <f.fig>"),
            "tools/import-design/fig_decode.mjs emit")


class DigestSync(unittest.TestCase):
    """Drift between tools.yaml and the CLAUDE.md digest must fail."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.doc = pathlib.Path(self.tmp.name) / "CLAUDE.md"
        self._real_doc = MOD.DIGEST_DOC
        MOD.DIGEST_DOC = self.doc
        self.addCleanup(self._restore)
        self.registry = {"tools": [_tool()]}

    def _restore(self):
        MOD.DIGEST_DOC = self._real_doc
        self.tmp.cleanup()

    def _skeleton(self, body: str = "") -> None:
        self.doc.write_text(
            f"# CLAUDE.md\n\n<!-- generated:start id=tools-digest -->\n"
            f"{body}<!-- generated:end id=tools-digest -->\n\ntail\n")

    def test_write_then_check_is_in_sync(self):
        self._skeleton()
        self.assertEqual(MOD.digest_problems(self.registry, write=True), [])
        self.assertEqual(MOD.digest_problems(self.registry, write=False), [])

    def test_stale_digest_is_caught(self):
        self._skeleton(body="stale hand-written junk\n")
        problems = MOD.digest_problems(self.registry, write=False)
        self.assertTrue(any("out of sync" in p for p in problems))

    def test_drift_after_a_registry_change_is_caught(self):
        self._skeleton()
        MOD.digest_problems(self.registry, write=True)
        changed = {"tools": [_tool(use_when="A totally different need.")]}
        self.assertTrue(any("out of sync" in p
                            for p in MOD.digest_problems(changed, write=False)))

    def test_missing_markers_are_caught(self):
        self.doc.write_text("# CLAUDE.md\n\nno markers here\n")
        problems = MOD.digest_problems(self.registry, write=False)
        self.assertTrue(any("missing the generated markers" in p for p in problems))

    def test_write_preserves_surrounding_content(self):
        self._skeleton()
        MOD.digest_problems(self.registry, write=True)
        text = self.doc.read_text()
        self.assertTrue(text.startswith("# CLAUDE.md"))
        self.assertTrue(text.rstrip().endswith("tail"))

    def test_planned_tools_are_absent_from_the_digest(self):
        # A planned tool has no invocation to offer, so the digest must not
        # advertise it as something to run.
        reg = {"tools": [_tool(), {"name": "layout-parity",
                                   "category": "visual-compare",
                                   "use_when": "Report misplaced nodes.",
                                   "availability": "planned",
                                   "pointer": "planning/plan.md"}]}
        self.assertNotIn("layout-parity", MOD.render_digest(reg))


class RealRegistry(unittest.TestCase):
    """Smoke: the checked-in registry is valid, covered, and in sync."""

    def test_registry_is_clean(self):
        self.assertEqual(MOD.main(["--check"]), 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)

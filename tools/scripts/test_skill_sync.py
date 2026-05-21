#!/usr/bin/env python3
"""Fixture tests for skill_sync_check.py.

The SKILL.md-sync gate that pairs the version-bump gate. Split out of
`test_gates.py` (P9-NEW refactor, 2026-05) so it sits next to the
version_bump cluster test modules; test bodies are byte-identical to
their previous definitions.

Runs standalone (`python3 tools/scripts/test_skill_sync.py`) or as part
of the aggregate suite via `test_gates.py`.
"""

from __future__ import annotations

from pathlib import Path
import sys
import unittest

from gate_test_support import GateFixtureTestCase, _git


class SkillSyncTests(GateFixtureTestCase):
    """skill_sync_check fixtures."""

    def test_skill_path_touched_without_md_update_fails(self) -> None:
        self.f.write("tools/cli/cmd_foo.cpp", "// added content\nint x();\n")
        self.f.commit("cli: tweak cmd_foo")
        code, out = self.f.run_ssc()
        self.assertEqual(code, 1, msg=out)
        self.assertIn("cli-maintenance", out)
        self.assertIn("SKILL.md NOT updated", out)

    def test_skill_path_touched_with_md_update_passes(self) -> None:
        self.f.write("tools/cli/cmd_foo.cpp", "// added content\nint x();\n")
        self.f.write(".agents/skills/cli-maintenance/SKILL.md",
                     "# cli-maintenance skill\n\nNew gotcha: ...\n")
        self.f.commit("cli: tweak cmd_foo + record gotcha")
        code, out = self.f.run_ssc()
        self.assertEqual(code, 0, msg=out)
        self.assertIn("SKILL.md updated", out)

    def test_skill_update_bypass_trailer_passes(self) -> None:
        self.f.write("tools/cli/cmd_foo.cpp", "// added content\nint x();\n")
        _git(self.tmp, "add", "-A")
        _git(self.tmp, "commit", "-q", "-m",
             'cli: mechanical rename\n\n'
             'Skill-Update: skip skill=cli-maintenance reason="mechanical rename"')
        code, out = self.f.run_ssc()
        self.assertEqual(code, 0, msg=out)
        self.assertIn("bypassed", out)

    # ── Regression tests for Codex P1/P2 review on the first merge ─────

    def test_skill_side_file_does_not_satisfy_md_requirement(self) -> None:
        """Side files under the skill dir used to count as SKILL.md
        updates (Codex P2). Now only SKILL.md counts."""
        self.f.write("tools/cli/cmd_foo.cpp", "// added\nint x();\n")
        self.f.write(".agents/skills/cli-maintenance/notes.md",
                     "# scratch — not SKILL.md\n")
        self.f.commit("cli: tweak cmd_foo + add scratch notes")
        code, out = self.f.run_ssc()
        self.assertEqual(code, 1, msg=out)
        self.assertIn("cli-maintenance", out)
        self.assertIn("SKILL.md NOT updated", out)

    def test_skill_sync_matches_top_level_cli_file(self) -> None:
        """Skill-sync carried the same glob bug — its `tools/cli/**` map
        entry must match `tools/cli/cmd_foo.cpp` directly."""
        self.f.write(
            "tools/cli/cmd_foo.cpp",
            "int cmd_foo_run() { return 0; }\n",
        )
        self.f.commit("chore: cli tweak")
        code, out = self.f.run_ssc()
        # cli-maintenance skill is mapped to tools/cli/** and its SKILL.md
        # was NOT updated → expect the gate to hard-fail.
        self.assertEqual(code, 1, msg=out)
        self.assertIn("cli-maintenance", out)
        self.assertIn("SKILL.md NOT updated", out)

    def test_skill_sync_helper_paths_trailers_and_self_check(self) -> None:
        ssc = self._import_gate_module("skill_sync_check")

        trailers = {"skill-update": [
            'skip skill=ci reason="workflow-only change"',
            "skip skill=cli-maintenance reason=mechanical",
            'note skill=hosting reason="not a skip"',
            "skip",
        ]}
        self.assertEqual(
            ssc.parse_skill_update_trailer(trailers, "Skill-Update"),
            {
                "ci": "workflow-only change",
                "cli-maintenance": "mechanical",
            },
        )

        self.assertEqual(
            ssc.filter_generated(
                ["build/foo.cpp", "src/foo.cpp", "sub/foo.generated.cpp"],
                ["build/**", "**/*.generated.*"],
            ),
            ["src/foo.cpp"],
        )

        errors = ssc.self_check(
            ssc.SkillMap({"ci": [], "missing": []}),
            self.tmp / ".agents" / "skills",
        )
        self.assertTrue(any("cli-maintenance" in e for e in errors), msg=errors)
        self.assertTrue(any("missing" in e for e in errors), msg=errors)

        findings = ssc.compute_findings(
            changed=["ci/build.yml", ".agents/skills/ci/nested/SKILL.md"],
            skill_map=ssc.SkillMap({"ci": ["ci/**"]}),
            skills_dir=self.tmp / ".agents" / "skills",
            repo=self.tmp,
            bypasses={},
        )
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0].skill, "ci")
        self.assertTrue(findings[0].skill_md_modified)

        bypassed = ssc.compute_findings(
            changed=["tools/cli/cmd_foo.cpp"],
            skill_map=ssc.SkillMap({"cli-maintenance": ["tools/cli/**"]}),
            skills_dir=self.tmp / ".agents" / "skills",
            repo=self.tmp,
            bypasses={"cli-maintenance": "generated rename"},
        )
        self.assertEqual(len(bypassed), 1)
        self.assertEqual(bypassed[0].bypass_reason, "generated rename")

    def test_import_gate_module_inserts_scripts_path_when_missing(self) -> None:
        scripts = str(Path(__file__).resolve().parent)
        original_path = list(sys.path)

        try:
            sys.path[:] = [p for p in sys.path if p != scripts]
            module = self._import_gate_module("gate_common")
            self.assertIs(module, __import__("gate_common"))
            self.assertEqual(sys.path[0], scripts)
        finally:
            sys.path[:] = original_path


if __name__ == "__main__":
    unittest.main(verbosity=2)

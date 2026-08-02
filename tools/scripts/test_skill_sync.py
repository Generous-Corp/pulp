#!/usr/bin/env python3
"""Fixture tests for skill_sync_check.py.

The SKILL.md-sync gate pairs with the version-bump gate and sits next to
the version_bump cluster test modules.

Runs standalone (`python3 tools/scripts/test_skill_sync.py`) or as part
of the aggregate suite via `test_gates.py`.
"""

from __future__ import annotations

from pathlib import Path
import sys
import unittest

from gate_test_support import GateFixtureTestCase, REPO_ROOT, _git


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

    # ── Regression tests for skill update matching ─────────────────────

    def test_skill_side_file_does_not_satisfy_md_requirement(self) -> None:
        """Side files under the skill dir must not count as SKILL.md
        updates. Only SKILL.md counts."""
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


class RealSkillPathMapOwnershipTests(unittest.TestCase):
    """Ownership boundaries in the shipped ``skill_path_map.json``.

    These run against the real map, not a fixture: the thing under test
    is which skill a real repo path resolves to, and a synthetic map
    cannot express that. Resolution goes through the production
    ``compute_findings`` so the assertions and the gate can never
    disagree.

    A skill claiming a subsystem it does not document makes the gate
    fire on changes it has nothing to say about. That is not merely
    friction: it teaches contributors to reach for
    ``Skill-Update: skip`` by reflex, and that reflex is how a
    genuinely missed skill update gets waved through. So every claim
    here is asserted two-sided — the skill must NOT fire on a
    subsystem-internal change, and must STILL fire on the surface it
    genuinely owns.
    """

    @classmethod
    def setUpClass(cls) -> None:
        scripts = str(REPO_ROOT / "tools" / "scripts")
        if scripts not in sys.path:
            sys.path.insert(0, scripts)
        cls.ssc = __import__("skill_sync_check")
        cls.skill_map = cls.ssc.load_skill_map(
            REPO_ROOT / "tools" / "scripts" / "skill_path_map.json"
        )

    def owners(self, path: str) -> set[str]:
        findings = self.ssc.compute_findings(
            changed=[path],
            skill_map=self.skill_map,
            skills_dir=REPO_ROOT / ".agents" / "skills",
            repo=REPO_ROOT,
            bypasses={},
        )
        return {f.skill for f in findings}

    def assert_tracked(self, path: str) -> None:
        self.assertTrue(
            (REPO_ROOT / path).exists(),
            msg=f"{path} is not in the tree — this test asserts on a real "
                "repo path; update it to the path the file moved to.",
        )

    # ── Side 1: engine internals must NOT demand web-plugins ──────────

    def test_engine_internal_change_does_not_demand_web_plugins(self) -> None:
        """timebase / timeline / playback own their own internals.

        ``web-plugins`` documents the browser ABI lanes. An edit to an
        existing engine translation unit or header changes nothing it
        teaches, so it must not pull ``web-plugins`` into the gate.
        """
        cases = {
            "core/timeline/src/model.cpp": "timeline",
            "core/timeline/include/pulp/timeline/model.hpp": "timeline",
            "core/playback/src/tempo_sync.cpp": "playback",
            "core/timebase/src/compiled_tempo_map.cpp": "timebase",
        }
        for path, expected_owner in cases.items():
            with self.subTest(path=path):
                self.assert_tracked(path)
                owners = self.owners(path)
                self.assertIn(
                    expected_owner, owners,
                    msg=f"{path} lost its own subsystem skill: {sorted(owners)}",
                )
                self.assertNotIn(
                    "web-plugins", owners,
                    msg=f"{path} has no web surface, but web-plugins claims "
                        f"it: {sorted(owners)}",
                )

    # ── Side 2: web surfaces must STILL demand web-plugins ────────────

    def test_web_facing_change_still_demands_web_plugins(self) -> None:
        """The narrowing is a narrowing, not a deletion.

        Both `Pulp*Sources.cmake` files live *inside* the engine trees
        and are the shared source lists both web ABI lanes include —
        adding or splitting an engine TU edits them, and that is the
        change ``web-plugins`` has guidance about. They must still fire,
        alongside the plainly web-facing surfaces.
        """
        for path in (
            "core/timeline/PulpTimelineSources.cmake",
            "core/playback/PulpPlaybackSources.cmake",
            "tools/cmake/PulpWam.cmake",
            "tools/cmake/PulpWclap.cmake",
            "core/view/platform/web/window_host_web.cpp",
            ".github/workflows/web-plugins.yml",
        ):
            with self.subTest(path=path):
                self.assert_tracked(path)
                self.assertIn(
                    "web-plugins", self.owners(path),
                    msg=f"{path} is web-facing but no longer demands a "
                        "web-plugins SKILL.md update — the claim was "
                        "deleted rather than narrowed.",
                )

    def test_engine_source_closure_files_keep_both_owners(self) -> None:
        """The closure files are genuinely dual-owned, not reassigned."""
        self.assertEqual(
            self.owners("core/timeline/PulpTimelineSources.cmake"),
            {"timeline", "web-plugins"},
        )
        self.assertEqual(
            self.owners("core/playback/PulpPlaybackSources.cmake"),
            {"playback", "web-plugins"},
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)

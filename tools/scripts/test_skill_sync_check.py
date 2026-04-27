#!/usr/bin/env python3
"""Focused unit coverage for skill_sync_check.py."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import skill_sync_check as ssc


class SkillSyncCheckTests(unittest.TestCase):

    def test_load_config_resolves_repo_relative_paths_and_trailer_key(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            config = repo / "versioning.json"
            config.write_text(json.dumps({
                "$schema": "ignored",
                "_comment": "ignored",
                "skills": {
                    "skills_dir": ".agents/skills",
                    "path_map": "tools/scripts/skill_path_map.json",
                },
                "generated_globs": ["build/**"],
                "trailers": {"skill_update": "Skill-Gate"},
            }))

            loaded = ssc.load_config(config, repo)

        self.assertEqual(loaded.skills_dir, repo / ".agents" / "skills")
        self.assertEqual(
            loaded.path_map_file,
            repo / "tools" / "scripts" / "skill_path_map.json",
        )
        self.assertEqual(loaded.generated_globs, ["build/**"])
        self.assertEqual(loaded.trailer_skill_update, "Skill-Gate")

    def test_load_skill_map_ignores_metadata_and_empty_entries(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "skill_path_map.json"
            path.write_text(json.dumps({
                "$schema": "ignored",
                "_comment": "ignored",
                "skills": {
                    "ci": {"paths": [".github/workflows/**"]},
                    "empty": {},
                },
            }))

            loaded = ssc.load_skill_map(path)

        self.assertEqual(loaded.skills["ci"], [".github/workflows/**"])
        self.assertEqual(loaded.skills["empty"], [])

    def test_compute_findings_requires_skill_md_not_sidecar_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            skills_dir = repo / ".agents" / "skills"
            changed = [
                "ci/build.yml",
                ".agents/skills/ci/notes.md",
            ]

            findings = ssc.compute_findings(
                changed=changed,
                skill_map=ssc.SkillMap({"ci": ["ci/**"]}),
                skills_dir=skills_dir,
                repo=repo,
                bypasses={},
            )

        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0].touched_paths, ["ci/build.yml"])
        self.assertFalse(findings[0].skill_md_modified)

    def test_render_report_fails_only_in_enforcing_modes(self) -> None:
        finding = ssc.Finding(
            skill="ci",
            touched_paths=["ci/build.yml"],
            skill_md_modified=False,
            bypass_reason=None,
        )

        report_text, report_code = ssc.render_report(
            [finding],
            trailer_key="Skill-Update",
            mode="report",
        )
        hint_text, hint_code = ssc.render_report(
            [finding],
            trailer_key="Skill-Update",
            mode="hint",
        )

        self.assertEqual(report_code, 1)
        self.assertIn("Skill-sync check FAILED", report_text)
        self.assertIn('Skill-Update: skip skill=ci reason="..."', report_text)
        self.assertEqual(hint_code, 0)
        self.assertIn("SKILL.md NOT updated", hint_text)


if __name__ == "__main__":
    unittest.main()

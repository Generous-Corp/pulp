#!/usr/bin/env python3
"""Fixture tests for skill_path_map_lint.py and json_schema_lite.py.

Imported by `test_gates.py`, which is what CI's "Gate-script fixture
tests" step runs. A TestCase that is not imported there runs nowhere.

Every rule is exercised in both directions: a fixture that must fail and
a near-identical one that must pass. A rule proven only by its failure
case can be satisfied by a check that always fails; a rule proven only by
its success case can be satisfied by a check that does nothing.
"""

from __future__ import annotations

import json
import subprocess
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import json_schema_lite as jsl  # noqa: E402
import skill_path_map_lint as lint  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
SCHEMA_PATH = REPO / lint.SCHEMA_RELPATH


def _map(**skills: object) -> dict:
    return {"schema_version": 1, "skills": dict(skills)}


def _entry(*paths: str, **doc: str) -> dict:
    entry: dict = {"paths": list(paths)}
    if doc:
        entry["_doc"] = doc
    return entry


# Long enough to satisfy the schema's minLength on annotation prose.
WHY_EMPTY = "developer-supplied SDK that is never committed to this repo"
WHY_SCOPE = "this skill documents the whole subsystem for a stated reason"


class JsonSchemaLiteTests(unittest.TestCase):
    """The validator must fail loudly on what it cannot check."""

    def test_unimplemented_keyword_raises_instead_of_being_skipped(self) -> None:
        with self.assertRaises(jsl.UnsupportedKeyword) as ctx:
            jsl.validate({"a": 1}, {"type": "object", "dependentRequired": {}})
        self.assertIn("dependentRequired", str(ctx.exception))

    def test_unimplemented_keyword_nested_in_a_subschema_also_raises(self) -> None:
        schema = {
            "type": "object",
            "properties": {"a": {"type": "array", "contains": {"type": "string"}}},
        }
        with self.assertRaises(jsl.UnsupportedKeyword):
            jsl.validate({"a": ["x"]}, schema)

    def test_supported_keywords_accept_and_reject(self) -> None:
        cases = [
            ({"type": "integer"}, 1, "x"),
            ({"type": "object"}, {}, []),
            ({"const": 1}, 1, 2),
            ({"enum": ["a", "b"]}, "a", "c"),
            ({"type": "string", "minLength": 2}, "ab", "a"),
            ({"type": "string", "maxLength": 2}, "ab", "abc"),
            ({"type": "string", "pattern": "^a+$"}, "aaa", "aab"),
            ({"type": "array", "minItems": 1}, [1], []),
            ({"type": "array", "maxItems": 1}, [1], [1, 2]),
            ({"type": "array", "uniqueItems": True}, [1, 2], [1, 1]),
            ({"type": "object", "minProperties": 1}, {"a": 1}, {}),
            ({"type": "object", "maxProperties": 1}, {"a": 1}, {"a": 1, "b": 2}),
            ({"type": "object", "required": ["a"]}, {"a": 1}, {"b": 1}),
            (
                {"type": "object", "additionalProperties": False},
                {},
                {"a": 1},
            ),
            (
                {"type": "object", "propertyNames": {"pattern": "^a$"}},
                {"a": 1},
                {"b": 1},
            ),
            (
                {"type": "array", "items": {"type": "string"}},
                ["a"],
                [1],
            ),
            (
                {"type": "object", "properties": {"a": {"type": "string"}}},
                {"a": "x"},
                {"a": 1},
            ),
        ]
        for schema, good, bad in cases:
            with self.subTest(schema=schema):
                self.assertEqual(jsl.validate(good, schema), [], f"{good!r} should pass")
                self.assertTrue(jsl.validate(bad, schema), f"{bad!r} should fail")

    def test_boolean_is_not_an_integer(self) -> None:
        self.assertTrue(jsl.validate(True, {"type": "integer"}))

    def test_type_mismatch_suppresses_downstream_keyword_noise(self) -> None:
        errors = jsl.validate("x", {"type": "object", "required": ["a", "b", "c"]})
        self.assertEqual(len(errors), 1)


class SchemaRuleTests(unittest.TestCase):
    def _check(self, skill_map: dict) -> list[str]:
        return lint.check_schema(REPO, skill_map, SCHEMA_PATH)

    def test_real_schema_accepts_a_well_formed_map(self) -> None:
        ok = _map(alpha=_entry("core/alpha/src/**"))
        ok["$schema"] = "./skill_path_map.schema.json"
        ok["_comment"] = "fixture"
        self.assertEqual(self._check(ok), [])

    def test_bare_array_entry_is_rejected(self) -> None:
        """The shape that parses to zero patterns and reports nothing."""
        errors = self._check(_map(alpha=[]))
        self.assertTrue(any("expected type object" in e for e in errors))

    def test_missing_schema_version_is_rejected(self) -> None:
        bad = _map(alpha=_entry("core/alpha/**"))
        del bad["schema_version"]
        self.assertTrue(self._check(bad))

    def test_unknown_top_level_key_is_rejected(self) -> None:
        bad = _map(alpha=_entry("core/alpha/**"))
        bad["skils"] = {}
        self.assertTrue(any("skils" in e for e in self._check(bad)))

    def test_unknown_entry_key_is_rejected(self) -> None:
        bad = _map(alpha={"paths": ["core/alpha/**"], "note": "typo for _doc"})
        self.assertTrue(any("note" in e for e in self._check(bad)))

    def test_malformed_patterns_are_rejected(self) -> None:
        for pattern in (
            "/core/alpha/**",
            "./core/alpha/**",
            "core/../alpha/**",
            "core\\alpha\\**",
            "core//alpha/**",
            "core/alpha/",
            "core/alpha /**",
            "",
        ):
            with self.subTest(pattern=pattern):
                self.assertTrue(
                    self._check(_map(alpha=_entry(pattern))),
                    f"{pattern!r} should be rejected",
                )

    def test_dotfile_rooted_patterns_are_accepted(self) -> None:
        for pattern in (".github/workflows/**", ".agents/skills/alpha/**"):
            with self.subTest(pattern=pattern):
                self.assertEqual(self._check(_map(alpha=_entry(pattern))), [])

    def test_duplicate_patterns_within_one_entry_are_rejected(self) -> None:
        dup = _map(alpha=_entry("core/alpha/**", "core/alpha/**"))
        self.assertTrue(any("unique" in e for e in self._check(dup)))

    def test_non_kebab_skill_name_is_rejected(self) -> None:
        self.assertTrue(self._check(_map(**{"Alpha_Skill": _entry("core/a/**")})))

    def test_placeholder_annotation_prose_is_rejected(self) -> None:
        thin = _map(alpha=_entry(**{"empty-ok": "yes"}))
        self.assertTrue(any("minLength" in e for e in self._check(thin)))

    def test_missing_schema_file_is_an_error_not_a_skip(self) -> None:
        errors = lint.check_schema(
            REPO, _map(alpha=_entry("core/a/**")), REPO / "does-not-exist.json"
        )
        self.assertTrue(any("validates nothing" in e for e in errors))

    def test_dangling_schema_pointer_is_an_error(self) -> None:
        bad = _map(alpha=_entry("core/a/**"))
        bad["$schema"] = "./skill_path_map.schema.json"
        errors = lint.check_schema(
            REPO, bad, SCHEMA_PATH
        )
        self.assertEqual(errors, [])  # resolves today

        gone = dict(bad, **{"$schema": "./no-such-schema.json"})
        # A pointer the schema's own const rejects AND that resolves nowhere.
        self.assertTrue(lint.check_schema(REPO, gone, SCHEMA_PATH))


class SubmoduleRuleTests(unittest.TestCase):
    SUBS = ["planning"]

    def test_pattern_inside_a_submodule_is_rejected(self) -> None:
        errors = lint.check_submodule(
            _map(alpha=_entry("planning/2026-06-11-proposal.md")), self.SUBS
        )
        self.assertEqual(len(errors), 1)
        self.assertIn("can never match", errors[0])

    def test_the_gitlink_itself_is_also_rejected(self) -> None:
        self.assertTrue(lint.check_submodule(_map(alpha=_entry("planning")), self.SUBS))

    def test_empty_ok_annotation_does_not_unlock_it(self) -> None:
        entry = _entry("planning/spec.md", **{"empty-ok": WHY_EMPTY})
        self.assertTrue(lint.check_submodule(_map(alpha=entry), self.SUBS))

    def test_a_sibling_prefix_is_not_treated_as_inside(self) -> None:
        """`planning-notes/` is not under the `planning` submodule."""
        errors = lint.check_submodule(
            _map(alpha=_entry("planning-notes/**")), self.SUBS
        )
        self.assertEqual(errors, [])

    def test_real_repo_reports_planning_as_a_submodule_with_no_files_beneath(
        self,
    ) -> None:
        files, submodules = lint.index_entries(REPO)
        self.assertIn("planning", submodules)
        self.assertEqual([f for f in files if f.startswith("planning/")], [])


class EmptyRuleTests(unittest.TestCase):
    FILES = ["core/alpha/src/a.cpp", "docs/guides/alpha.md"]

    def test_pattern_matching_nothing_is_rejected(self) -> None:
        errors = lint.check_empty(_map(alpha=_entry("core/beta/**")), self.FILES)
        self.assertEqual(len(errors), 1)
        self.assertIn("core/beta/**", errors[0])

    def test_pattern_matching_something_passes(self) -> None:
        self.assertEqual(
            lint.check_empty(_map(alpha=_entry("core/alpha/**")), self.FILES), []
        )

    def test_external_sdk_path_is_excused_by_the_annotation(self) -> None:
        entry = _entry("external/sdk/**", **{"empty-ok": WHY_EMPTY})
        self.assertEqual(lint.check_empty(_map(alpha=entry), self.FILES), [])

    def test_annotation_does_not_excuse_an_in_tree_path(self) -> None:
        entry = _entry("core/beta/**", **{"empty-ok": WHY_EMPTY})
        errors = lint.check_empty(_map(alpha=entry), self.FILES)
        self.assertTrue(any("external/ SDK paths only" in e for e in errors))

    def test_pathless_entry_needs_the_annotation(self) -> None:
        self.assertTrue(lint.check_empty(_map(alpha=_entry()), self.FILES))
        annotated = _entry(**{"empty-ok": WHY_EMPTY})
        self.assertEqual(lint.check_empty(_map(alpha=annotated), self.FILES), [])

    def test_submodule_paths_are_left_to_the_submodule_rule(self) -> None:
        skill_map = _map(alpha=_entry("planning/spec.md"))
        self.assertTrue(lint.check_empty(skill_map, self.FILES))
        self.assertEqual(lint.check_empty(skill_map, self.FILES, ["planning"]), [])


class CoClaimRuleTests(unittest.TestCase):
    def test_new_claim_on_an_already_owned_subsystem_is_rejected(self) -> None:
        base = _map(alpha=_entry("core/shared/**"))
        head = _map(alpha=_entry("core/shared/**"), beta=_entry("core/shared/**"))
        errors = lint.check_co_claim(base, head)
        self.assertEqual(len(errors), 1)
        self.assertIn("beta", errors[0])

    def test_new_claim_by_the_sole_owner_passes(self) -> None:
        base = _map(alpha=_entry())
        head = _map(alpha=_entry("core/shared/**"))
        self.assertEqual(lint.check_co_claim(base, head), [])

    def test_pre_existing_co_claim_is_left_alone(self) -> None:
        both = _map(alpha=_entry("core/shared/**"), beta=_entry("core/shared/**"))
        self.assertEqual(lint.check_co_claim(both, both), [])

    def test_scope_annotation_unlocks_it(self) -> None:
        base = _map(alpha=_entry("core/shared/**"))
        head = _map(
            alpha=_entry("core/shared/**"),
            beta=_entry("core/shared/**", scope=WHY_SCOPE),
        )
        self.assertEqual(lint.check_co_claim(base, head), [])

    def test_narrower_co_claims_are_not_subsystem_claims(self) -> None:
        for pattern in (
            "core/shared/src/**",
            "core/shared/PulpSharedSources.cmake",
            "core/shared/*",
            "core/*/**",
        ):
            with self.subTest(pattern=pattern):
                base = _map(alpha=_entry(pattern))
                head = _map(alpha=_entry(pattern), beta=_entry(pattern))
                self.assertEqual(lint.check_co_claim(base, head), [])

    def test_no_base_map_disables_the_rule_rather_than_guessing(self) -> None:
        head = _map(alpha=_entry("core/shared/**"), beta=_entry("core/shared/**"))
        self.assertEqual(lint.check_co_claim(None, head), [])


class RealSkillPathMapLintTests(unittest.TestCase):
    """The wired contract: the repo's own map passes every rule."""

    def test_repo_map_is_clean(self) -> None:
        code = lint.main(["--repo-root", str(REPO), "--base", ""])
        self.assertEqual(code, 0, "tools/scripts/skill_path_map.json failed its lint")

    def test_planted_no_op_pattern_is_caught(self) -> None:
        """Negative control against the live map, in-memory.

        Proves the rule reads the real map's contents rather than
        reporting a constant.
        """
        skill_map = json.loads((REPO / lint.MAP_RELPATH).read_text())
        files, submodules = lint.index_entries(REPO)
        self.assertEqual(lint.check_empty(skill_map, files, submodules), [])

        name = sorted(skill_map["skills"])[0]
        skill_map["skills"][name]["paths"].append("core/no-such-subsystem/**")
        errors = lint.check_empty(skill_map, files, submodules)
        self.assertTrue(any("core/no-such-subsystem/**" in e for e in errors))

    def test_every_skill_directory_has_an_entry(self) -> None:
        """Mirrors skill_sync_check.self_check, asserted against the real tree."""
        skill_map = json.loads((REPO / lint.MAP_RELPATH).read_text())
        dirs = {
            d.name for d in (REPO / ".agents" / "skills").iterdir() if d.is_dir()
        }
        self.assertEqual(dirs, set(skill_map["skills"]))

    def test_lint_is_wired_into_every_enforcing_path(self) -> None:
        """The defect this lint exists to catch was a check nothing ran.

        Assert the script is invoked from the three paths that can stop a
        change: the pre-push hook, the on-demand gates runner, and the CI
        workflow. A lint reachable only by hand is the same defect again.
        """
        for relpath in (
            ".githooks/pre-push",
            "tools/scripts/gates.sh",
            ".github/workflows/version-skill-check.yml",
        ):
            with self.subTest(path=relpath):
                self.assertIn(
                    "skill_path_map_lint.py",
                    (REPO / relpath).read_text(),
                    f"{relpath} does not run skill_path_map_lint.py",
                )

    def test_lint_tests_are_imported_by_the_ci_entrypoint(self) -> None:
        self.assertIn(
            "test_skill_path_map_lint",
            (REPO / "tools" / "scripts" / "test_gates.py").read_text(),
        )

    def test_lint_exits_non_zero_when_invoked_as_a_script(self) -> None:
        """Exit code, not just return value — the hooks branch on it."""
        proc = subprocess.run(
            [
                sys.executable,
                str(Path(__file__).resolve().parent / "skill_path_map_lint.py"),
                "--repo-root",
                str(REPO),
                "--base",
                "",
                "--head-map",
                str(Path(__file__).resolve()),
            ],
            capture_output=True,
            text=True,
        )
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)

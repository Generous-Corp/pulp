#!/usr/bin/env python3
"""Tests for script_inputs_store.py: the fragment layout of the script-test input list.

The point of the layout is that two independent pull requests that each add
a script test merge without a conflict. `MergeTests` proves that with a real
git three-way merge, and runs the identical scenario against the single-file
layout as its negative control: that one must conflict, or the merge test is
not measuring anything.
"""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import script_inputs_store as store  # noqa: E402

GIT_ENV = {"PATH": "/usr/bin:/bin:/opt/homebrew/bin:/usr/local/bin", "HOME": "/nonexistent",
           "GIT_CONFIG_NOSYSTEM": "1", "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t",
           "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@t"}


def entry(name: str) -> dict:
    return {"entry": f"tools/scripts/test_{name}.py", "inputs": [f"tools/scripts/test_{name}.py"],
            "kind": "python"}


def sample_doc() -> dict:
    return {
        "schema": "pulp-ctest-script-inputs/v1",
        "executables_scanned_for": ["data", "spawns"],
        "executable_targets": {"Pulp UI Script Fixture": "PulpUiScriptFixture_Standalone"},
        "executables": {"pulp-test-a": {"data": "none", "inputs": [], "kind": "compiled", "sources": [],
                                        "undeclared_sources": []}},
        "executables_scanned": ["Pulp UI Script Fixture", "pulp-test-a", "pulp-test-b"],
        "tests": {name: entry(name) for name in ("alpha", "bravo", "charlie", "delta")},
    }


class Repo:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.git("init", "-q", "-b", "main")

    def git(self, *args: str, check: bool = True) -> subprocess.CompletedProcess:
        return subprocess.run(["git", "-C", str(self.root), *args], env=GIT_ENV, capture_output=True,
                              text=True, encoding="utf-8", check=check)

    def commit(self, msg: str) -> None:
        self.git("add", "--", "test")
        self.git("commit", "-q", "-m", msg)


class SlugTests(unittest.TestCase):
    def test_a_safe_lowercase_name_is_its_own_slug(self) -> None:
        self.assertEqual(store.slug("script-test-inputs-drift"), "script-test-inputs-drift")

    def test_other_names_are_hashed_and_cannot_collide_by_case(self) -> None:
        a, b = store.slug("Pulp UI A"), store.slug("pulp ui a")
        self.assertNotEqual(a.lower(), b.lower())
        self.assertRegex(a, r"^[a-z0-9._-]+--[0-9a-f]{10}$")
        # A safe name that looks like a hashed slug is hashed too, so it cannot shadow one.
        self.assertNotEqual(store.slug(a), a)

    def test_a_slug_depends_on_its_own_name_only(self) -> None:
        self.assertEqual(store.slug("Pulp UI A"), store.slug("Pulp UI A"))


class RoundTripTests(unittest.TestCase):
    def test_split_then_join_is_the_same_document(self) -> None:
        doc = sample_doc()
        self.assertEqual(store.join(store.split(doc)), doc)

    def test_a_document_without_compiled_entries_round_trips_without_them(self) -> None:
        doc = {"schema": "pulp-ctest-script-inputs/v1", "tests": {"alpha": entry("alpha")}}
        self.assertEqual(store.join(store.split(doc)), doc)

    def test_one_fragment_per_entry(self) -> None:
        files = store.split(sample_doc())
        self.assertIn("tests/alpha.json", files)
        self.assertIn("executables/pulp-test-b.json", files)
        self.assertEqual(sum(1 for f in files if f.startswith("tests/")), 4)
        self.assertEqual(sum(1 for f in files if f.startswith("executables/")), 3)

    def test_the_checked_in_list_round_trips(self) -> None:
        root = HERE.parents[1]
        if not (root / store.FRAGMENT_DIR).is_dir():
            self.skipTest("no fragment directory in this checkout")
        doc = store.load(root)
        self.assertGreater(len(doc["tests"]), 100)
        self.assertEqual(store.join(store.split(doc)), doc)
        self.assertEqual(store.split(doc), store._read_dir(root / store.FRAGMENT_DIR),
                         "every fragment is in canonical form: regenerate with script_test_inputs.py --write")


class ConsistencyTests(unittest.TestCase):
    def test_a_fragment_under_the_wrong_name_is_refused(self) -> None:
        files = store.split(sample_doc())
        files["tests/zulu.json"] = files.pop("tests/alpha.json")
        with self.assertRaisesRegex(store.StoreError, "holds 'alpha'"):
            store.join(files)

    def test_a_stray_file_is_refused(self) -> None:
        files = store.split(sample_doc())
        files["notes.txt"] = "hi\n"
        with self.assertRaisesRegex(store.StoreError, "unexpected file"):
            store.join(files)

    def test_conflict_markers_are_refused(self) -> None:
        files = store.split(sample_doc())
        files["tests/alpha.json"] = "<<<<<<< ours\n" + files["tests/alpha.json"] + ">>>>>>> theirs\n"
        with self.assertRaises(store.StoreError):
            store.join(files)

    def test_missing_meta_is_refused(self) -> None:
        files = store.split(sample_doc())
        del files[store.META_FILE]
        with self.assertRaisesRegex(store.StoreError, "meta.json"):
            store.join(files)


class WriteTests(unittest.TestCase):
    def test_write_touches_only_changed_fragments_and_removes_stale_ones(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp) / "list"
            doc = sample_doc()
            self.assertGreater(len(store.write_path(d, doc)), 5)
            self.assertEqual(store.write_path(d, doc), [])
            doc["tests"]["alpha"]["inputs"].append("tools/scripts/extra.py")
            del doc["tests"]["delta"]
            self.assertEqual(store.write_path(d, doc), ["tests/alpha.json", "tests/delta.json"])
            self.assertFalse((d / "tests/delta.json").exists())
            self.assertEqual(store.load_path(d), doc)


class RefTests(unittest.TestCase):
    def test_load_ref_reads_fragments_and_the_older_single_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Repo(Path(tmp))
            legacy = sample_doc()
            (repo.root / "test").mkdir()
            (repo.root / store.LEGACY_LIST).write_text(store.dumps(legacy), encoding="utf-8")
            repo.commit("single file")
            old = repo.git("rev-parse", "HEAD").stdout.strip()
            repo.git("rm", "-q", store.LEGACY_LIST.as_posix())
            new_doc = sample_doc()
            new_doc["tests"]["echo"] = entry("echo")
            store.write_path(repo.root / store.FRAGMENT_DIR, new_doc)
            repo.commit("fragments")
            self.assertEqual(store.load_ref(repo.root, old), legacy)
            self.assertEqual(store.load_ref(repo.root, "HEAD"), new_doc)
            self.assertEqual(store.load(repo.root), new_doc)
            self.assertIsNone(store.load_ref(repo.root, "no-such-ref"))
            ident_old = store.ref_identity(repo.root, old)
            ident_new = store.ref_identity(repo.root, "HEAD")
            self.assertTrue(ident_old and ident_new and ident_old != ident_new)


class MergeTests(unittest.TestCase):
    """Two independent PRs each add one test, next to each other in name order."""

    def merge_two_additions(self, layout: str) -> tuple[subprocess.CompletedProcess, Repo]:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        repo = Repo(Path(tmp.name))
        target = repo.root / (store.FRAGMENT_DIR if layout == "fragments" else store.LEGACY_LIST)

        def write(doc: dict) -> None:
            store.write_path(target, doc)

        base = sample_doc()
        write(base)
        repo.commit("base")
        for branch, name in (("pr-a", "bravo-two"), ("pr-b", "bravo-three")):
            repo.git("checkout", "-q", "-b", branch, "main")
            doc = sample_doc()
            doc["tests"][name] = entry(name)
            write(doc)
            repo.commit(f"add {name}")
        repo.git("checkout", "-q", "pr-a")
        return repo.git("merge", "--no-edit", "-q", "pr-b", check=False), repo

    def test_independent_additions_merge_cleanly_with_fragments(self) -> None:
        merged, repo = self.merge_two_additions("fragments")
        self.assertEqual(merged.returncode, 0, merged.stdout + merged.stderr)
        doc = store.load(repo.root)
        self.assertIn("bravo-two", doc["tests"])
        self.assertIn("bravo-three", doc["tests"])
        expected = sample_doc()
        expected["tests"].update({"bravo-two": entry("bravo-two"), "bravo-three": entry("bravo-three")})
        self.assertEqual(doc, expected)

    def test_negative_control_the_single_file_conflicts(self) -> None:
        """The same two additions conflict in the single-file layout; if they
        did not, the test above would prove nothing."""
        merged, repo = self.merge_two_additions("legacy")
        self.assertNotEqual(merged.returncode, 0, "the single-file layout merged cleanly; "
                            "the scenario no longer reproduces the conflict")
        self.assertIn("CONFLICT", merged.stdout + merged.stderr)
        repo.git("merge", "--abort", check=False)


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Tests for the agent-capability record layout (agent_capability_store.py).

Three properties, each against real data and real git:

- Lossless: the fragments regenerate the single files they replaced byte for
  byte, both for the commit that split them (read from git history) and for
  the current tree, and a split that loses anything is refused.
- Canonical: the committed fragments are exactly the split of the documents
  they assemble to, so a hand edit, a stray file, or a fork in the history
  chain is reported.
- Disjoint: two branches that each add a different capability header (its
  surface record, its reviewed fingerprint, and a recorded history state)
  touch disjoint files and `git merge-tree` merges them cleanly, while the
  same two changes in the single-file layout conflict.
"""
from __future__ import annotations

import copy
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))

import agent_capability_fingerprints as fingerprints  # noqa: E402
import agent_capability_history as history  # noqa: E402
import agent_capability_store as store  # noqa: E402
import agent_capability_surface as surface  # noqa: E402

GIT_ENV = {**os.environ, "GIT_CONFIG_NOSYSTEM": "1", "GIT_AUTHOR_NAME": "t",
           "GIT_AUTHOR_EMAIL": "t@t", "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@t"}
CATALOGS = sorted(path.relative_to(ROOT).as_posix()
                  for path in ROOT.glob(fingerprints.CATALOG_GLOB))


def git(root: pathlib.Path, *args: str, check: bool = True) -> subprocess.CompletedProcess:
    # No background maintenance: it can still be writing into .git when the
    # temporary repository is removed.
    quiet = ["-c", "gc.auto=0", "-c", "maintenance.auto=false", "-c", "core.fsmonitor=false"]
    return subprocess.run(["git", *quiet, "-C", str(root), *args], capture_output=True, text=True,
                          encoding="utf-8", env=GIT_ENV, check=check)


def current_surface() -> dict:
    document = store.load_surface(ROOT)
    assert isinstance(document, dict) and "_invalid" not in document, document
    return document


def current_history() -> dict:
    document = store.load_history(ROOT)
    assert isinstance(document, dict) and "_invalid" not in document, document
    return document


def write_files(root: pathlib.Path, files: dict[str, str]) -> None:
    for rel, text in files.items():
        path = root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")


def tree(directory: pathlib.Path, files: dict[str, str]) -> dict[str, str]:
    return {f"{directory.as_posix()}/{rel}": text for rel, text in files.items()}


def migration_commit() -> str | None:
    """The commit that replaced the single files with fragments, if history has it."""
    found = git(ROOT, "log", "-1", "--format=%H", "--diff-filter=D", "HEAD", "--",
                store.HISTORY_LEGACY.as_posix(), check=False)
    commit = found.stdout.strip()
    return commit or None


class LosslessTests(unittest.TestCase):
    def test_the_migration_commit_regenerates_the_files_it_replaced(self) -> None:
        """Each fragment directory, as the split commit wrote it, re-renders the
        exact bytes of the single file in that commit's parent, and every
        catalog literal it removed is its header's fragment."""
        commit = migration_commit()
        if commit is None:
            self.skipTest("this clone has no commit that deleted the single history file "
                          "(a shallow clone, or the split is not committed yet); the "
                          "current-tree round trip below still runs")
        parent = commit + "^1"
        for legacy in (store.SURFACE_LEGACY, store.HISTORY_LEGACY):
            original = git(ROOT, "show", f"{parent}:{legacy.as_posix()}").stdout
            regenerated = store.text_at(ROOT, commit, legacy)
            self.assertIsNotNone(regenerated, legacy)
            self.assertEqual(regenerated, original, f"{legacy} does not regenerate byte for byte")
            self.assertIsNone(
                git(ROOT, "cat-file", "-e", f"{commit}:{legacy.as_posix()}", check=False).stdout or None)
        literals: dict[str, set[str]] = {}
        count = 0
        for catalog in CATALOGS:
            shown = git(ROOT, "show", f"{parent}:{catalog}", check=False)
            if shown.returncode != 0:
                continue
            for include, value, _, _ in fingerprints._literal_removals(shown.stdout, pathlib.Path(catalog)):
                literals.setdefault(include, set()).add(value)
                count += 1
        self.assertGreater(count, 0, "the parent of the split commit holds no catalog literals")
        recorded = store.join_fingerprints(store.ref_files(ROOT, commit, store.FINGERPRINT_DIR) or {})
        self.assertEqual({include: {value} for include, value in recorded.items()}, literals)

    def test_the_current_records_round_trip(self) -> None:
        for directory, join, split, render in (
            (store.SURFACE_DIR, store.join_surface, store.split_surface, store.render_surface),
            (store.HISTORY_DIR, lambda f: store.join_history(f)[0], store.split_history,
             store.render_history),
        ):
            files = store.read_dir(ROOT / directory)
            self.assertTrue(files, directory)
            document = join(files)
            self.assertEqual(split(document), files, f"{directory} is not its own split")
            text = render(document)
            self.assertEqual(split(json.loads(text)), files,
                             f"{directory} does not survive a trip through the single file")
        self.assertEqual(store.split_fingerprints(store.load_fingerprints(ROOT)),
                         store.read_dir(ROOT / store.FINGERPRINT_DIR))

    def test_migrate_moves_the_single_files_and_refuses_a_lossy_split(self) -> None:
        surface_text = store.render_surface(current_surface())
        history_text = store.render_history(current_history())
        with tempfile.TemporaryDirectory(prefix="pulp-capability-store-") as temp:
            root = pathlib.Path(temp)
            write_files(root, {store.SURFACE_LEGACY.as_posix(): surface_text,
                               store.HISTORY_LEGACY.as_posix(): history_text})
            moved = store.migrate(root)
            self.assertEqual(len(moved), 2, moved)
            self.assertFalse((root / store.SURFACE_LEGACY).exists())
            self.assertFalse((root / store.HISTORY_LEGACY).exists())
            self.assertEqual(store.render_surface(store.load_surface(root)), surface_text)
            self.assertEqual(store.render_history(store.load_history(root)), history_text)

        # Negative control: a split that drops one stored object is caught by
        # the same byte comparison, and the single file is left in place.
        original = store.split_history

        def lossy(document: dict) -> dict[str, str]:
            files = original(document)
            first = sorted(files)[0]
            entry = json.loads(files[first])
            entry["objects"].pop(sorted(entry["objects"])[-1])
            files[first] = store.history_entry_text(
                entry["entry_digest"], entry["parent"], entry["objects"])
            return files

        with tempfile.TemporaryDirectory(prefix="pulp-capability-store-") as temp:
            root = pathlib.Path(temp)
            write_files(root, {store.HISTORY_LEGACY.as_posix(): history_text})
            with mock.patch.object(store, "split_history", lossy):
                with self.assertRaises(store.StoreError):
                    store.migrate(root)
            self.assertEqual((root / store.HISTORY_LEGACY).read_text(encoding="utf-8"), history_text)

    def test_a_legacy_revision_still_reads(self) -> None:
        """A protected base older than the split holds the single files."""
        surface_text = store.render_surface(current_surface())
        with tempfile.TemporaryDirectory(prefix="pulp-capability-store-") as temp:
            root = pathlib.Path(temp)
            git(root, "init", "-q", "-b", "main")
            write_files(root, {store.SURFACE_LEGACY.as_posix(): surface_text})
            git(root, "add", "-A")
            git(root, "commit", "-qm", "single file")
            self.assertEqual(store.text_at(root, "HEAD", store.SURFACE_LEGACY), surface_text)
            self.assertEqual(store.render_surface(store.load_surface(root)), surface_text)
            git(root, "rm", "-q", store.SURFACE_LEGACY.as_posix())
            write_files(root, tree(store.SURFACE_DIR, store.split_surface(json.loads(surface_text))))
            git(root, "add", "-A")
            git(root, "commit", "-qm", "fragments")
            self.assertEqual(store.text_at(root, "HEAD", store.SURFACE_LEGACY), surface_text)
            self.assertEqual(store.text_at(root, "HEAD~1", store.SURFACE_LEGACY), surface_text)


class CanonicalTests(unittest.TestCase):
    def test_a_stray_file_a_reformatted_record_and_a_retired_file_are_reported(self) -> None:
        document = current_surface()
        with tempfile.TemporaryDirectory(prefix="pulp-capability-store-") as temp:
            root = pathlib.Path(temp)
            store.replace_directory(root, store.SURFACE_DIR, store.split_surface(document))
            self.assertEqual(store.layout_problems(root, document, None, None), [])
            first = sorted((root / store.SURFACE_DIR / "headers").rglob("*.json"))[0]
            first.write_text(json.dumps(json.loads(first.read_text()), indent=4) + "\n")
            (root / store.SURFACE_DIR / "headers/pulp/extra.hpp.json").write_text("{}\n")
            (root / store.SURFACE_LEGACY).write_text("{}\n")
            problems = "\n".join(store.layout_problems(root, document, None, None))
            self.assertIn("is not in its rendered form", problems)
            self.assertIn("extra.hpp.json is not part of the generated record", problems)
            self.assertIn("retired single-file layout", problems)

    def test_a_record_in_the_wrong_file_does_not_load(self) -> None:
        files = store.split_surface(current_surface())
        rel = sorted(name for name in files if name.startswith("headers/"))[0]
        moved = dict(files)
        moved["headers/pulp/elsewhere.hpp.json"] = moved.pop(rel)
        with self.assertRaises(store.StoreError):
            store.join_surface(moved)

    def test_the_history_chain_reports_a_fork_and_the_split_rechains_it(self) -> None:
        document = current_history()
        entries = history.history_entries(document)
        files = store.split_history(document)
        tip = document["entries"][-1]
        manifest_a = copy.deepcopy(entries[-1]["manifest"])
        manifest_a["manifest_revision"] += 1
        manifest_b = copy.deepcopy(manifest_a)
        manifest_b["counts"]["total"] += 1
        for extra_manifest in (manifest_a, manifest_b):
            extra = {"manifest": extra_manifest, "surface": entries[-1]["surface"]}
            extended = history.history_document(entries + [
                {**extra, "entry_digest": surface.canonical_digest(extra)}])
            new_files = store.split_history(extended)
            added = set(new_files) - set(files)
            self.assertEqual(len(added), 1)
            self.assertEqual(json.loads(new_files[next(iter(added))])["parent"], tip)
            files = {**files, **{rel: new_files[rel] for rel in added}}
        joined, problems = store.join_history(files)
        self.assertEqual(len(problems), 1, problems)
        self.assertIn("forks after " + tip, problems[0])
        rechained = store.split_history(joined)
        self.assertEqual(store.join_history(rechained)[1], [])
        self.assertEqual(store.join_history(rechained)[0], joined)
        self.assertEqual(history.history_entries(joined)[:len(entries)], entries)

    def test_a_broken_chain_does_not_load(self) -> None:
        files = store.split_history(current_history())
        second = sorted(files, key=lambda rel: json.loads(files[rel])["parent"] or "")[1]
        broken = dict(files)
        entry = json.loads(broken[second])
        broken[second] = store.history_entry_text(entry["entry_digest"], "sha256:" + "0" * 64,
                                                  entry["objects"])
        with self.assertRaises(store.StoreError):
            store.join_history(broken)


def _add_header(document: dict, include: str) -> dict:
    record = {
        "domain": "audio",
        "source": f"core/audio/include/{include}",
        "fingerprint": "sha256:" + "c" * 64,
        "include": include,
        "disposition": "capability_entrypoint",
        "capability_keys": [f"audio.{include.rsplit('/', 1)[-1].split('.')[0]}"],
        "rationale": "Public binding for a curated consumer capability.",
    }
    return surface.assemble_surface_document(
        document["headers"] + [record], document["tombstones"], document["inventory_version"])


def _record_state(document: dict, manifest_total: int) -> dict:
    """The history with one more recorded state after its tip."""
    entries = history.history_entries(document)
    material = {"manifest": copy.deepcopy(entries[-1]["manifest"]),
                "surface": copy.deepcopy(entries[-1]["surface"])}
    material["manifest"]["manifest_revision"] += 1
    material["manifest"]["counts"]["total"] = manifest_total
    return history.history_document(
        entries + [{**material, "entry_digest": surface.canonical_digest(material)}])


class DisjointTests(unittest.TestCase):
    """Two branches each add a different capability header."""

    INCLUDE_A = "pulp/audio/zz_branch_a.hpp"
    INCLUDE_B = "pulp/audio/zz_branch_b.hpp"

    def _branches(self) -> tuple[dict, dict, dict]:
        base_surface = current_surface()
        base_history = current_history()
        base_prints = store.load_fingerprints(ROOT)
        total = history.history_entries(base_history)[-1]["manifest"]["counts"]["total"]
        states = {}
        for name, include in (("a", self.INCLUDE_A), ("b", self.INCLUDE_B)):
            states[name] = (
                _add_header(base_surface, include),
                _record_state(base_history, total + (1 if name == "a" else 2)),
                {**base_prints, include: "sha256:" + ("a" if name == "a" else "b") * 64},
                f"core/audio/include/{include}",
            )
        return {"surface": base_surface, "history": base_history, "prints": base_prints}, \
            states["a"], states["b"]

    @staticmethod
    def _fragment_tree(surface_document: dict, history_document: dict, prints: dict) -> dict:
        return {**tree(store.SURFACE_DIR, store.split_surface(surface_document)),
                **tree(store.HISTORY_DIR, store.split_history(history_document)),
                **tree(store.FINGERPRINT_DIR, store.split_fingerprints(prints))}

    @staticmethod
    def _single_file_tree(surface_document: dict, history_document: dict, prints: dict) -> dict:
        lines = "".join(f"{include} {value}\n" for include, value in sorted(prints.items()))
        return {store.SURFACE_LEGACY.as_posix(): store.render_surface(surface_document),
                store.HISTORY_LEGACY.as_posix(): store.render_history(history_document),
                "fingerprints-as-one-file.txt": lines}

    def _repo(self, root: pathlib.Path, layout, base: dict, a: tuple, b: tuple) -> None:
        git(root, "init", "-q", "-b", "main")
        write_files(root, layout(base["surface"], base["history"], base["prints"]))
        git(root, "add", "-A")
        git(root, "commit", "-qm", "base")
        for name, (surface_document, history_document, prints, header) in (("a", a), ("b", b)):
            git(root, "checkout", "-q", "-b", name, "main")
            for path in (root / store.SURFACE_DIR, root / store.HISTORY_DIR, root / store.FINGERPRINT_DIR):
                if path.is_dir():
                    for item in sorted(path.rglob("*"), reverse=True):
                        if item.is_file():
                            item.unlink()
            write_files(root, layout(surface_document, history_document, prints))
            write_files(root, {header: f"// {name}\n"})
            git(root, "add", "-A")
            git(root, "commit", "-qm", f"capability {name}")

    def test_two_capability_branches_touch_disjoint_files_and_merge_cleanly(self) -> None:
        base, a, b = self._branches()
        with tempfile.TemporaryDirectory(prefix="pulp-capability-disjoint-") as temp:
            root = pathlib.Path(temp)
            self._repo(root, self._fragment_tree, base, a, b)
            touched_a = set(git(root, "diff", "--name-only", "main", "a").stdout.split())
            touched_b = set(git(root, "diff", "--name-only", "main", "b").stdout.split())
            self.assertEqual(len(touched_a), 4, touched_a)  # header, record, fingerprint, history entry
            self.assertEqual(touched_a & touched_b, set(), "two capability branches share a file")
            merged = git(root, "merge-tree", "--write-tree", "a", "b", check=False)
            self.assertEqual(merged.returncode, 0, merged.stdout)
            merged_tree = merged.stdout.split()[0]
            git(root, "checkout", "-q", "--detach", "a")
            git(root, "read-tree", "-u", "--reset", merged_tree)
            joined = store.load_surface(root)
            includes = {row["include"] for row in joined["headers"]}
            self.assertTrue({self.INCLUDE_A, self.INCLUDE_B} <= includes)
            self.assertEqual(set(store.load_fingerprints(root)),
                             set(a[2]) | set(b[2]))
            # Both branches recorded a state after the same tip; the merge holds
            # both, reports the fork, and the next --write re-chains it.
            _, problems = store.join_history(store.read_dir(root / store.HISTORY_DIR))
            self.assertEqual(len(problems), 1, problems)

    def test_the_same_two_branches_conflict_in_the_single_file_layout(self) -> None:
        """The negative control: the instrument above can see a conflict."""
        base, a, b = self._branches()
        with tempfile.TemporaryDirectory(prefix="pulp-capability-disjoint-") as temp:
            root = pathlib.Path(temp)
            self._repo(root, self._single_file_tree, base, a, b)
            merged = git(root, "merge-tree", "--write-tree", "--name-only", "a", "b", check=False)
            self.assertEqual(merged.returncode, 1, merged.stdout)
            for legacy in (store.SURFACE_LEGACY, store.HISTORY_LEGACY):
                self.assertIn(f"CONFLICT (content): Merge conflict in {legacy.as_posix()}",
                              merged.stdout)


if __name__ == "__main__":
    unittest.main()

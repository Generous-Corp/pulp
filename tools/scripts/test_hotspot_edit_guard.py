#!/usr/bin/env python3
"""Tests for hotspot_edit_guard.py against real git repositories."""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = HERE / "hotspot_edit_guard.py"
sys.path.insert(0, str(HERE))
import hotspot_edit_guard as guard  # noqa: E402

GIT_ENV = {"PATH": "/usr/bin:/bin:/opt/homebrew/bin:/usr/local/bin", "HOME": "/nonexistent",
           "GIT_CONFIG_NOSYSTEM": "1", "GIT_AUTHOR_NAME": "t", "GIT_AUTHOR_EMAIL": "t@t",
           "GIT_COMMITTER_NAME": "t", "GIT_COMMITTER_EMAIL": "t@t"}
AGGREGATE = "test/ctest_script_inputs.json"
FRAGMENT = "test/ctest_script_inputs/tests/alpha.json"


class Repo:
    def __init__(self, root: Path) -> None:
        self.root = root
        self.git("init", "-q", "-b", "main")
        self.write(guard.REGISTRY.as_posix(), (HERE / "hotspot_edit_guard.json").read_text(encoding="utf-8"))

    def git(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run(["git", "-C", str(self.root), *args], env=GIT_ENV, capture_output=True,
                              text=True, encoding="utf-8", check=True)

    def write(self, rel: str, text: str) -> None:
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, encoding="utf-8")

    def commit(self, msg: str, *paths: str) -> None:
        self.git("add", "--all", "--", *paths)
        self.git("commit", "-q", "-m", msg)

    def run(self, *args: str, env: dict | None = None) -> subprocess.CompletedProcess:
        return subprocess.run([sys.executable, str(SCRIPT), "--root", str(self.root), *args],
                              capture_output=True, text=True, encoding="utf-8", env={**GIT_ENV, **(env or {})}, timeout=60)


def repo_with_aggregate(tmp: str) -> Repo:
    """main as it was before the split: the single aggregate file."""
    repo = Repo(Path(tmp))
    repo.write(AGGREGATE, json.dumps({"tests": {"alpha": {}}}) + "\n")
    repo.commit("base", ".")
    repo.git("branch", "base-ref")
    return repo


class GuardTests(unittest.TestCase):
    def test_modifying_the_aggregate_is_refused_and_names_the_fragments(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = repo_with_aggregate(tmp)
            repo.write(AGGREGATE, json.dumps({"tests": {"alpha": {}, "bravo": {}}}) + "\n")
            repo.commit("hand edit", AGGREGATE)
            proc = repo.run("--base", "base-ref")
            self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
            self.assertIn(f"file={AGGREGATE}; cause_class=HOTSPOT_AGGREGATE_EDIT", proc.stdout)
            self.assertIn("test/ctest_script_inputs/", proc.stdout)
            self.assertIn("script_test_inputs.py", proc.stdout)

    def test_an_uncommitted_edit_is_refused_too(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = repo_with_aggregate(tmp)
            repo.write(AGGREGATE, "{}\n")
            self.assertEqual(repo.run("--base", "base-ref").returncode, 1)

    def test_deleting_the_aggregate_and_writing_fragments_passes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = repo_with_aggregate(tmp)
            repo.git("rm", "-q", AGGREGATE)
            repo.write(FRAGMENT, "{}\n")
            repo.commit("split", "test")
            proc = repo.run("--base", "base-ref")
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            self.assertIn("OK", proc.stdout)

    def test_re_adding_the_aggregate_after_the_split_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = repo_with_aggregate(tmp)
            repo.git("rm", "-q", AGGREGATE)
            repo.write(FRAGMENT, "{}\n")
            repo.commit("split", "test")
            repo.git("branch", "-f", "base-ref", "HEAD")
            repo.write(AGGREGATE, "{}\n")
            repo.commit("tool writes the old single file", AGGREGATE)
            proc = repo.run("--base", "base-ref")
            self.assertEqual(proc.returncode, 1, proc.stdout)
            self.assertIn("re-added", proc.stdout)

    def test_a_fragment_edit_and_unrelated_edits_pass(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = repo_with_aggregate(tmp)
            repo.write(FRAGMENT, "{}\n")
            repo.write("README.md", "x\n")
            repo.commit("normal change", ".")
            self.assertEqual(repo.run("--base", "base-ref").returncode, 0)

    def test_the_base_is_the_merge_base_not_the_moving_tip(self) -> None:
        """main editing the aggregate after the branch point is not this branch's edit."""
        with tempfile.TemporaryDirectory() as tmp:
            repo = repo_with_aggregate(tmp)
            repo.git("checkout", "-q", "-b", "pr")
            repo.write("README.md", "x\n")
            repo.commit("pr", "README.md")
            repo.git("checkout", "-q", "main")
            repo.write(AGGREGATE, "{}\n")
            repo.commit("main moves", AGGREGATE)
            repo.git("checkout", "-q", "pr")
            self.assertEqual(repo.run("--base", "main").returncode, 0)

    def test_a_merge_checkout_resolves_its_first_parent(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = repo_with_aggregate(tmp)
            repo.git("checkout", "-q", "-b", "pr")
            repo.write(AGGREGATE, "{}\n")
            repo.commit("pr edits", AGGREGATE)
            repo.git("checkout", "-q", "main")
            repo.write("README.md", "x\n")
            repo.commit("main", "README.md")
            repo.git("merge", "-q", "--no-ff", "--no-edit", "pr")
            proc = repo.run(env={"GITHUB_EVENT_NAME": "pull_request"})
            self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
            self.assertIn("HEAD^1", proc.stdout)

    def test_no_base_and_a_bad_registry_are_errors_not_passes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = repo_with_aggregate(tmp)
            self.assertEqual(repo.run().returncode, 2)  # no origin/main, no event
            skipped = repo.run("--skip-without-base")
            self.assertEqual(skipped.returncode, 77)
            self.assertIn("SKIPPED", skipped.stdout)
            repo.write(guard.REGISTRY.as_posix(), json.dumps({"schema": "nope"}))
            self.assertEqual(repo.run("--base", "base-ref").returncode, 2)

    def test_adding_a_catalog_fingerprint_literal_is_refused(self) -> None:
        """A literal-pattern hotspot refuses the added line, not the file."""
        catalog = "tools/scripts/agent_capability_catalog_signal.py"
        row = 'binding(include="pulp/signal/x.hpp",\n'
        with tempfile.TemporaryDirectory() as tmp:
            repo = repo_with_aggregate(tmp)
            repo.write(catalog, row + '        header_fingerprint="sha256:' + "0" * 64 + '",\n)\n')
            repo.commit("old layout", catalog)
            repo.git("branch", "-f", "base-ref", "HEAD")
            # Deleting the literal (the migration) and unrelated catalog edits pass.
            repo.write(catalog, row + ")\n# another row\n")
            repo.commit("migrate", catalog)
            proc = repo.run("--base", "base-ref")
            self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
            repo.git("branch", "-f", "base-ref", "HEAD")
            # Re-adding one, committed or not, is refused and names the fragments.
            repo.write(catalog, row + '        header_fingerprint="sha256:' + "1" * 64 + '",\n)\n')
            proc = repo.run("--base", "base-ref")
            self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
            self.assertIn(f"file={catalog}; cause_class=HOTSPOT_LITERAL_EDIT", proc.stdout)
            self.assertIn("tools/agent-capabilities/fingerprints/", proc.stdout)
            repo.commit("re-add", catalog)
            self.assertEqual(repo.run("--base", "base-ref").returncode, 1)
            # The same line in a file the glob does not match is not this hotspot.
            other = "tools/scripts/agent_capability_registry_types.py"
            repo.write(other, "    header_fingerprint: str | None = None,\n")
            repo.commit("unrelated", other)
            repo.git("branch", "-f", "base-ref", "HEAD~1")
            proc = repo.run("--base", "base-ref")
            self.assertNotIn(other, proc.stdout)

    def test_a_bad_added_line_pattern_is_a_registry_error(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = repo_with_aggregate(tmp)
            repo.write(guard.REGISTRY.as_posix(), json.dumps({
                "schema": guard.SCHEMA,
                "hotspots": [{"path": "a/*.py", "fragments": "f/", "writer": "w",
                              "added_line_pattern": "("}]}))
            self.assertEqual(repo.run("--base", "base-ref").returncode, 2)

    def test_the_checked_in_registry_is_valid_and_its_fragments_exist(self) -> None:
        root = HERE.parents[1]
        for spot in guard.load_registry(root):
            if spot.get("mode") == "serialized":
                self.assertTrue((root / spot["path"]).is_file())
                continue
            self.assertTrue((root / spot["fragments"]).is_dir(),
                            f"{spot['path']} is registered but {spot['fragments']} does not exist: "
                            "register a hotspot only once its fragments exist")
            self.assertFalse((root / spot["path"]).exists(), f"{spot['path']} is back beside its fragments")

    def _serialized_repo(self, tmp: str) -> tuple[Repo, str, Path]:
        repo = Repo(Path(tmp))
        serialized = "docs/status/consumption-profiles.json"
        repo.write(serialized, "{}\n")
        repo.commit("base", ".")
        repo.git("branch", "base-ref")
        registry = Path(tmp) / "registry.json"
        registry.write_text(json.dumps({
            "schema": guard.SCHEMA,
            "hotspots": [{
                "mode": "serialized", "path": serialized,
                "writer": "single owner",
            }],
        }), encoding="utf-8")
        return repo, serialized, registry

    def test_serialized_hotspot_is_refused_without_a_trailer_even_with_the_old_env_var(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo, serialized, registry = self._serialized_repo(tmp)
            repo.write(serialized, "{\"changed\": true}\n")
            repo.commit("regenerate", serialized)
            blocked = repo.run("--base", "base-ref", "--registry", str(registry))
            self.assertEqual(blocked.returncode, 1, blocked.stdout + blocked.stderr)
            self.assertIn("SERIALIZED_HOTSPOT_EDIT", blocked.stdout)
            self.assertIn("Serialized-Hotspot: " + serialized + " reason=", blocked.stdout)
            # A process env var is invisible to CI and to a reviewer; it is not an override.
            env_only = repo.run("--base", "base-ref", "--registry", str(registry),
                                env={"PULP_ALLOW_SERIALIZED_HOTSPOT": "1"})
            self.assertEqual(env_only.returncode, 1, env_only.stdout + env_only.stderr)

    def test_serialized_hotspot_override_is_read_from_the_branch_commits(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo, serialized, registry = self._serialized_repo(tmp)
            repo.write(serialized, "{\"changed\": true}\n")
            repo.commit("regenerate census\n\nSerialized-Hotspot: " + serialized
                        + " reason=single writer for the census refresh", serialized)
            allowed = repo.run("--base", "base-ref", "--registry", str(registry))
            self.assertEqual(allowed.returncode, 0, allowed.stdout + allowed.stderr)
            self.assertIn("reason=single writer for the census refresh", allowed.stdout)

    def test_serialized_hotspot_override_needs_a_reason_and_the_exact_path(self) -> None:
        for trailer in ("Serialized-Hotspot: docs/status/consumption-profiles.json",
                        "Serialized-Hotspot: docs/status/consumption-profiles.json reason=",
                        "Serialized-Hotspot: docs/status/other.json reason=wrong file"):
            with self.subTest(trailer=trailer), tempfile.TemporaryDirectory() as tmp:
                repo, serialized, registry = self._serialized_repo(tmp)
                repo.write(serialized, "{\"changed\": true}\n")
                repo.commit("regenerate\n\n" + trailer, serialized)
                res = repo.run("--base", "base-ref", "--registry", str(registry))
                self.assertEqual(res.returncode, 1, res.stdout + res.stderr)

    def test_serialized_hotspot_override_on_the_base_side_does_not_count(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo, serialized, registry = self._serialized_repo(tmp)
            repo.git("checkout", "-q", "base-ref")
            repo.git("commit", "-q", "--allow-empty", "-m", "main\n\nSerialized-Hotspot: "
                     + serialized + " reason=someone else's refresh")
            repo.git("checkout", "-q", "-b", "topic", "HEAD~1")
            repo.write(serialized, "{\"changed\": true}\n")
            repo.commit("regenerate", serialized)
            res = repo.run("--base", "base-ref", "--registry", str(registry))
            self.assertEqual(res.returncode, 1, res.stdout + res.stderr)

if __name__ == "__main__":
    unittest.main()

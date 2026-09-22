#!/usr/bin/env python3
"""Tests for the advisory Vellum watch-event pre-push hint.

The hint's whole value is that it fires BEFORE a push, and its whole risk is
that a local copy of a trusted-root checker manufactures a false red on a
required-gate surface. Both properties are covered here, in throwaway git
repositories seeded with this repo's real acceptance artefact and checker — no
network, no build, and no dependency on this checkout's branch state.
"""

from __future__ import annotations

import io
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
PREFLIGHT = ROOT / "tools" / "scripts" / "vellum_watch_preflight.py"
CHECKER = ROOT / "tools" / "scripts" / "vellum_expansion_watch_check.py"
WATCH_DIR = ROOT / ".github" / "vellum-expansion-watch"
EVENT_DIR_REL = ".github/vellum-expansion-watch-events"

# A path inside EXPECTED_SCOPES, and one outside it. If the selectors are ever
# re-pinned these are the two lines to re-derive; test_the_fixture_paths_are_
# still_classified_as_expected fails loudly rather than silently passing.
WATCHED_PATH = "tools/import-design/fixture_probe.py"
WATCHED_FAMILIES = ["design-output-and-packaging", "design-source-ingest"]
UNWATCHED_PATH = "docs/guides/fixture-probe.md"


def _git(repo: Path, *args: str) -> str:
    out = subprocess.run(
        ["git", "-C", str(repo), *args],
        check=True, capture_output=True, text=True,
    )
    return out.stdout.strip()


class VellumWatchPreflightTest(unittest.TestCase):
    def setUp(self) -> None:
        if not CHECKER.is_file() or not WATCH_DIR.is_dir():
            self.skipTest("this checkout has no Vellum watch acceptance to seed from")
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self._tmp.name) / "repo"
        self.repo.mkdir()
        _git(self.repo, "init", "-q", "-b", "main")
        _git(self.repo, "config", "user.email", "fixture@example.invalid")
        _git(self.repo, "config", "user.name", "Fixture")
        # Seed the real acceptance and the real checker: load_acceptance pins
        # their SHA-256, so a hand-written stand-in would not load.
        (self.repo / ".github").mkdir()
        shutil.copytree(WATCH_DIR, self.repo / ".github" / "vellum-expansion-watch")
        (self.repo / EVENT_DIR_REL).mkdir(parents=True)
        (self.repo / "tools" / "scripts").mkdir(parents=True)
        shutil.copy2(CHECKER, self.repo / "tools" / "scripts" / CHECKER.name)
        self._commit("seed the watch acceptance")

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def _write(self, rel: str, text: str) -> None:
        path = self.repo / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)

    def _commit(self, message: str) -> str:
        _git(self.repo, "add", "-A")
        _git(self.repo, "commit", "-q", "-m", message)
        return _git(self.repo, "rev-parse", "HEAD")

    def _run(self, base: str = "main", head: str = "HEAD") -> tuple[int, str]:
        """Invoke the preflight as the hooks do: as a subprocess, from the repo."""
        completed = subprocess.run(
            ["python3", str(PREFLIGHT), "--repo", str(self.repo),
             "--base", base, "--head", head],
            cwd=self.repo, check=False, capture_output=True, text=True,
        )
        return completed.returncode, completed.stdout + completed.stderr

    def _event(self, event_id: str, families: list[str]) -> str:
        import json
        checker = self._import_checker()
        return json.dumps({
            "schema_version": 1,
            "kind": "authority-expansion-watch-change",
            "event_id": event_id,
            "created_at": "2026-09-20T00:00:00Z",
            "acceptance_id": "full-design-import-render-v1-pulp-watch",
            "acceptance_sha256": checker.EXPECTED_ACCEPTANCE_SHA256,
            "capability_families": sorted(families),
            "rationale": "Fixture event for the advisory pre-push hint's own tests.",
            "tests": ["vellum-watch-preflight"],
            "disposition": "watch-only-no-authority",
            "authority_effect": "none",
        }, indent=2) + "\n"

    @staticmethod
    def _import_checker():
        import importlib.util
        spec = importlib.util.spec_from_file_location("_fixture_checker", CHECKER)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    # ── the path filter ────────────────────────────────────────────────────

    def test_the_fixture_paths_are_still_classified_as_expected(self) -> None:
        """Control for every other test in this file.

        If the pinned selectors move, WATCHED_PATH could silently stop being
        watched — and then the "no hint" assertions below would all pass for
        the wrong reason.
        """
        checker = self._import_checker()
        scopes, _ = checker.load_acceptance(ROOT)
        self.assertIsNotNone(scopes, "the acceptance did not load from this checkout")
        self.assertEqual(
            sorted(checker._affected(scopes, {WATCHED_PATH})), WATCHED_FAMILIES)
        self.assertEqual(checker._affected(scopes, {UNWATCHED_PATH}), set())

    def test_a_range_touching_nothing_watched_is_silent(self) -> None:
        self._write(UNWATCHED_PATH, "an ordinary docs edit\n")
        _git(self.repo, "checkout", "-q", "-b", "topic")
        self._commit("docs: an edit outside every watched tree")
        code, output = self._run()
        self.assertEqual(code, 0)
        self.assertEqual(output.strip(), "", msg=f"unexpected output:\n{output}")

    def test_a_range_touching_a_watched_tree_names_the_families(self) -> None:
        _git(self.repo, "checkout", "-q", "-b", "topic")
        self._write(WATCHED_PATH, "# a one-line edit under a watched tree\n")
        self._commit("feat: touch a watched tree")
        code, output = self._run()
        self.assertEqual(code, 10)
        for family in WATCHED_FAMILIES:
            self.assertIn(family, output)
        self.assertIn("ADVISORY", output)
        # The trap that makes a correct event file look broken.
        self.assertIn("COMMIT RANGE", output)
        # The hint must route to the authoring rules, not just complain.
        self.assertIn("pulp-vellum-change-routing", output)
        # And to the command that reproduces CI's verdict exactly.
        self.assertIn("vellum_expansion_watch_check.py", output)
        self.assertIn("merge-base", output)

    def test_a_committed_event_covering_the_families_clears_the_hint(self) -> None:
        _git(self.repo, "checkout", "-q", "-b", "topic")
        self._write(WATCHED_PATH, "# a one-line edit under a watched tree\n")
        self._write(f"{EVENT_DIR_REL}/20260920-fixture.json",
                    self._event("20260920-fixture", WATCHED_FAMILIES))
        self._commit("feat: touch a watched tree, with its event")
        code, output = self._run()
        self.assertEqual(code, 0, msg=output)
        self.assertIn("cover", output)

    def test_an_uncommitted_event_does_not_clear_the_hint(self) -> None:
        """The second-order trap, asserted rather than only documented.

        The checker reads the commit range, so an event written but not
        committed scores exactly like a missing one. A reader who does not know
        that debugs a correct file.
        """
        _git(self.repo, "checkout", "-q", "-b", "topic")
        self._write(WATCHED_PATH, "# a one-line edit under a watched tree\n")
        self._commit("feat: touch a watched tree")
        # Written, deliberately NOT committed.
        self._write(f"{EVENT_DIR_REL}/20260920-fixture.json",
                    self._event("20260920-fixture", WATCHED_FAMILIES))
        code, output = self._run()
        self.assertEqual(code, 10)
        self.assertIn("COMMIT RANGE", output)

    # ── the constraint that keeps it from manufacturing false reds ─────────

    def test_the_base_is_the_merge_base_not_the_base_refs_tip(self) -> None:
        """A stale branch must not inherit main's later changes.

        Comparing against `origin/main`'s tip instead of the merge-base is what
        produced spurious `watch events are append-only` verdicts before. Here
        it would also misattribute main's watched edit to this branch.
        """
        _git(self.repo, "checkout", "-q", "-b", "topic")
        self._write(UNWATCHED_PATH, "an ordinary docs edit\n")
        self._commit("docs: an edit outside every watched tree")
        # main moves on, touching a watched tree AND adding its own event.
        _git(self.repo, "checkout", "-q", "main")
        self._write(WATCHED_PATH, "# main's own edit under a watched tree\n")
        self._write(f"{EVENT_DIR_REL}/20260920-main.json",
                    self._event("20260920-main", WATCHED_FAMILIES))
        self._commit("feat: main touches a watched tree")
        _git(self.repo, "checkout", "-q", "topic")

        code, output = self._run(base="main")
        self.assertEqual(
            code, 0,
            msg="the stale branch inherited main's watched change — the base is "
                f"not the merge-base:\n{output}")
        self.assertEqual(output.strip(), "")

        # Control: the instrument is live. Measured against main's TIP, the
        # same branch and the same command do produce a finding.
        checker = self._import_checker()
        tip = _git(self.repo, "rev-parse", "main")
        head = _git(self.repo, "rev-parse", "HEAD")
        report = checker.verify(self.repo, tip, head)
        self.assertEqual(
            report["status"], "fail",
            msg="the tip-based comparison came back clean, so this test would "
                "pass even if the preflight used the tip")

    # ── advisory means advisory ────────────────────────────────────────────

    def test_a_missing_checker_yields_no_verdict_and_no_traceback(self) -> None:
        (self.repo / "tools" / "scripts" / CHECKER.name).unlink()
        _git(self.repo, "checkout", "-q", "-b", "topic")
        self._write(WATCHED_PATH, "# a one-line edit under a watched tree\n")
        self._commit("feat: touch a watched tree")
        code, output = self._run()
        self.assertEqual(code, 20)
        self.assertNotIn("Traceback", output)

    def test_an_unresolvable_range_yields_no_verdict(self) -> None:
        _git(self.repo, "checkout", "-q", "-b", "topic")
        self._write(WATCHED_PATH, "# a one-line edit under a watched tree\n")
        self._commit("feat: touch a watched tree")
        code, output = self._run(base="refs/heads/no-such-branch")
        self.assertEqual(code, 20)
        self.assertNotIn("Traceback", output)

    def test_the_callers_never_let_it_block_a_push(self) -> None:
        """The hint is wired into two shared enforcement surfaces.

        Its authoritative counterpart runs from a trusted root in two REQUIRED
        GitHub checks. A local copy holding a veto would be a downgrade, so the
        call sites must not be able to set a failure — asserted at the call
        site, because that is where a later edit would break it.
        """
        import re
        for rel in ("tools/scripts/gates.sh", ".githooks/pre-push"):
            source = (ROOT / rel).read_text()
            self.assertIn("vellum_watch_preflight.py", source,
                          msg=f"{rel} does not run the advisory hint")
            # Both call sites reach the script through a variable, so scanning
            # for the literal filename finds only the assignment and would
            # declare any invocation clean. Resolve one hop of indirection —
            # the same thing build_parallelism_guard.py has to do — and require
            # that at least one real invocation line was examined.
            names = {"vellum_watch_preflight.py"}
            for match in re.finditer(
                    r'^\s*([A-Za-z_][A-Za-z0-9_]*)=.*vellum_watch_preflight\.py',
                    source, re.MULTILINE):
                names.add(match.group(1))
            invocations = [
                line for line in source.splitlines()
                if any(n in line for n in names) and not re.match(
                    r'^\s*[A-Za-z_][A-Za-z0-9_]*=', line)
                and not line.lstrip().startswith("#")
            ]
            self.assertTrue(
                invocations,
                msg=f"{rel}: found no invocation line for the hint — this check "
                    "is measuring nothing")
            for line in invocations:
                self.assertNotIn(
                    "fail=1", line,
                    msg=f"{rel} lets the advisory Vellum hint block a push: {line}")


if __name__ == "__main__":
    unittest.main()

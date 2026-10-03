#!/usr/bin/env python3
"""Tests for the local Vellum watch-event gate.

The gate's whole value is that it fires BEFORE a push and says exactly what
event to add, and its whole risk is that a local copy of a trusted-root checker
manufactures a false red on a required-gate surface. Both properties are covered here, in throwaway git
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
FREEZE_CHECKER = ROOT / "tools" / "scripts" / "vellum_freeze_check.py"
# Everything vellum_freeze_check.py reads, seeded verbatim from this checkout.
FREEZE_SEED = (
    "tools/scripts/vellum_freeze_check.py",
    ".agents/skills/pulp-vellum-change-routing",
    ".github/vellum-ownership.json",
    "docs/contracts/vellum-initial-cut-manifest.json",
)
CHANGE_EVENT_DIR_REL = ".github/vellum-change-events"
# A path in a `framework-authoritative-transferred` slice — the path that sent
# a real pull request red on `Vellum freeze`. It is also under a watched
# family, so a fully green range needs a watch event as well.
SLICE_PATH = "core/view/src/pointer_dispatch.cpp"
SLICE_ID = "retained-ui-kernel"
SLICE_WATCH_FAMILIES = ["render-assets-and-backends"]
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


class _PreflightRepo:
    """A throwaway repo seeded with the real watch acceptance and checker."""

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

    def _run(self, base: str = "main", head: str = "HEAD",
             *extra: str) -> tuple[int, str]:
        """Invoke the preflight as the hooks do: as a subprocess, from the repo."""
        completed = subprocess.run(
            ["python3", str(PREFLIGHT), "--repo", str(self.repo),
             "--base", base, "--head", head, *extra],
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


class VellumWatchPreflightTest(_PreflightRepo, unittest.TestCase):
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
        self.assertIn("ADVISORY", output)  # without --enforce
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

    # ── the exact event to add ─────────────────────────────────────────────

    def _suggested_event(self, output: str) -> dict:
        import json
        start = output.index("{")
        end = output.index("\n       }") + len("\n       }")
        return json.loads(output[start:end])

    def test_an_uncovered_change_prints_the_exact_event_json(self) -> None:
        _git(self.repo, "checkout", "-q", "-b", "topic")
        self._write(WATCHED_PATH, "# a one-line edit under a watched tree\n")
        self._write("tools/import-design/test_fixture_probe.py", "# a test\n")
        self._commit("feat: touch a watched tree")
        code, output = self._run("main", "HEAD", "--enforce")
        self.assertEqual(code, 10, msg=output)
        self.assertIn("BLOCKING", output)
        event = self._suggested_event(output)
        checker = self._import_checker()
        self.assertEqual(event["capability_families"], WATCHED_FAMILIES)
        self.assertEqual(event["acceptance_id"], "full-design-import-render-v1-pulp-watch")
        self.assertEqual(event["acceptance_sha256"], checker.EXPECTED_ACCEPTANCE_SHA256)
        self.assertEqual(event["disposition"], "watch-only-no-authority")
        self.assertEqual(event["authority_effect"], "none")
        self.assertIn("python3 tools/import-design/test_fixture_probe.py", event["tests"])
        self.assertTrue(event["event_id"].endswith("-topic-watch"), event["event_id"])
        # Every key the checker demands, and nothing else: with a real rationale
        # the suggestion must validate as-is.
        event["rationale"] = "A fixture rationale long enough to satisfy the checker."
        self.assertEqual(
            checker.validate_event(event, f"{event['event_id']}.json"),
            set(WATCHED_FAMILIES))

    def test_write_event_creates_an_event_that_clears_the_gate(self) -> None:
        _git(self.repo, "checkout", "-q", "-b", "topic")
        self._write(WATCHED_PATH, "# a one-line edit under a watched tree\n")
        self._commit("feat: touch a watched tree")
        code, output = self._run(
            "main", "HEAD", "--write-event", "--event-id", "20261002-fixture-watch",
            "--rationale", "Fixture change under a watched tree; watch-only, no authority moves.")
        self.assertEqual(code, 0, msg=output)
        written = self.repo / EVENT_DIR_REL / "20261002-fixture-watch.json"
        self.assertTrue(written.is_file(), output)
        self._commit("ci: record the watch event")
        code, output = self._run("main", "HEAD", "--enforce")
        self.assertEqual(code, 0, msg=output)
        # And the authoritative checker agrees on the same range.
        checker = self._import_checker()
        report = checker.verify(
            self.repo, _git(self.repo, "merge-base", "main", "HEAD"),
            _git(self.repo, "rev-parse", "HEAD"))
        self.assertEqual(report["status"], "pass", msg=report)

    def test_write_event_refuses_a_placeholder_rationale(self) -> None:
        _git(self.repo, "checkout", "-q", "-b", "topic")
        self._write(WATCHED_PATH, "# a one-line edit under a watched tree\n")
        self._commit("feat: touch a watched tree")
        for rationale in ("too short", "<REPLACE: something long enough to pass>"):
            code, output = self._run("main", "HEAD", "--write-event",
                                     "--rationale", rationale)
            self.assertEqual(code, 2, msg=output)
        self.assertEqual(list((self.repo / EVENT_DIR_REL).iterdir()), [])

    def test_write_event_with_nothing_owed_writes_nothing(self) -> None:
        _git(self.repo, "checkout", "-q", "-b", "topic")
        self._write(UNWATCHED_PATH, "an ordinary docs edit\n")
        self._commit("docs: an edit outside every watched tree")
        code, output = self._run("main", "HEAD", "--write-event", "--rationale",
                                 "Nothing is owed here, so nothing must be written.")
        self.assertEqual(code, 0, msg=output)
        self.assertEqual(list((self.repo / EVENT_DIR_REL).iterdir()), [])

    def test_an_over_claiming_event_is_named(self) -> None:
        _git(self.repo, "checkout", "-q", "-b", "topic")
        self._write(WATCHED_PATH, "# a one-line edit under a watched tree\n")
        self._write(f"{EVENT_DIR_REL}/20260920-fixture.json",
                    self._event("20260920-fixture",
                                WATCHED_FAMILIES + ["design-ir-contract"]))
        self._commit("feat: touch a watched tree, over-claiming")
        code, output = self._run("main", "HEAD", "--enforce")
        self.assertEqual(code, 10, msg=output)
        self.assertIn("design-ir-contract", output)
        self.assertIn("EXACT", output)

    # ── the freeze job's inventories ───────────────────────────────────────

    def _verifier(self, rel: str, *, stale: bool) -> None:
        body = ("import sys\nprint('inventory is stale; run --write', file=sys.stderr)\n"
                "sys.exit(1)\n") if stale else "print('ok')\n"
        self._write(rel, body)

    def test_a_stale_inventory_fails_with_its_regenerate_command(self) -> None:
        _git(self.repo, "checkout", "-q", "-b", "topic")
        self._verifier("tools/scripts/pulp_tooling_disposition.py", stale=True)
        self._verifier("tools/scripts/generate_vellum_ownership_projection.py", stale=False)
        self._commit("chore: a tooling change")
        code, output = self._run("main", "HEAD", "--enforce", "--inventories")
        self.assertEqual(code, 11, msg=output)
        self.assertIn("python3 tools/scripts/pulp_tooling_disposition.py --write", output)
        self.assertIn("inventory is stale", output)
        self.assertNotIn("ownership projection is stale", output)

    def test_current_inventories_pass_and_absent_ones_are_skipped(self) -> None:
        _git(self.repo, "checkout", "-q", "-b", "topic")
        self._verifier("tools/scripts/pulp_tooling_disposition.py", stale=False)
        self._commit("chore: a tooling change")
        code, output = self._run("main", "HEAD", "--enforce", "--inventories")
        self.assertEqual(code, 0, msg=output)

    def test_an_owed_event_and_a_stale_inventory_are_both_reported(self) -> None:
        _git(self.repo, "checkout", "-q", "-b", "topic")
        self._write(WATCHED_PATH, "# a one-line edit under a watched tree\n")
        self._verifier("tools/scripts/pulp_tooling_disposition.py", stale=True)
        self._commit("feat: touch a watched tree and a tooling inventory")
        code, output = self._run("main", "HEAD", "--enforce", "--inventories")
        self.assertEqual(code, 10, msg=output)
        self.assertIn("--write-event", output)
        self.assertIn("pulp_tooling_disposition.py --write", output)

    # ── only a positive verdict blocks ─────────────────────────────────────

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

    def test_the_callers_block_only_on_an_owed_event(self) -> None:
        """Both shared enforcement surfaces fail on exit 10 and on nothing else.

        Exit 20 ("no verdict") must never block: a local copy of a trusted-root
        checker that cannot reach a verdict has no business refusing a push.
        Asserted at the call site, because that is where a later edit would
        break it.
        """
        import re
        for rel in ("tools/scripts/gates.sh", ".githooks/pre-push"):
            source = (ROOT / rel).read_text()
            self.assertIn("vellum_watch_preflight.py", source,
                          msg=f"{rel} does not run the Vellum gate")
            # Both call sites reach the script through a variable, so scanning
            # for the literal filename finds only the assignment. Resolve one
            # hop of indirection and require a real invocation line.
            names = {"vellum_watch_preflight.py"}
            for match in re.finditer(
                    r'^\s*([A-Za-z_][A-Za-z0-9_]*)=.*vellum_watch_preflight\.py',
                    source, re.MULTILINE):
                names.add(match.group(1))
            invocations = [
                line for line in source.splitlines()
                if any(n in line for n in names)
                and not re.match(r'^\s*[A-Za-z_][A-Za-z0-9_]*=', line)
                and not line.lstrip().startswith(("#", "echo"))
                and not line.lstrip().startswith("if [ -f")
            ]
            self.assertTrue(all("--enforce" in line for line in invocations),
                            msg=f"{rel}: an invocation runs without --enforce")
            self.assertEqual(
                len(invocations), 1,
                msg=f"{rel}: expected exactly one --enforce invocation")
            self.assertNotIn("|| true", invocations[0],
                             msg=f"{rel} discards the gate's verdict")
            block = source[source.index(invocations[0]):][:400]
            self.assertIn("--inventories", invocations[0],
                          msg=f"{rel} does not verify the freeze inventories")
            self.assertRegex(
                block,
                r'vellum_rc"? -eq 10 \] \|\| \[ "\$vellum_rc"? -eq 11 \] '
                r'\|\| \[ "\$vellum_rc"? -eq 12 \]; then\s+fail=1',
                msg=f"{rel} does not fail on an owed watch event, a stale "
                    "inventory, or an owed change event")
            self.assertNotRegex(block, r"-eq 20|-ne 0|-gt 0",
                                msg=f"{rel} blocks on a no-verdict exit")


class VellumFreezePreflightTest(_PreflightRepo, unittest.TestCase):
    """The change-event half: `vellum_freeze_check.py`, run like `Vellum freeze`."""

    def setUp(self) -> None:
        super().setUp()
        for rel in FREEZE_SEED:
            if not (ROOT / rel).exists():
                self.skipTest(f"this checkout has no {rel} to seed from")
        for rel in FREEZE_SEED:
            source, target = ROOT / rel, self.repo / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            if source.is_dir():
                shutil.copytree(source, target)
            else:
                shutil.copy2(source, target)
        self._commit("seed the Vellum ownership projection and freeze checker")
        _git(self.repo, "checkout", "-q", "-b", "topic")

    @staticmethod
    def _import_freeze():
        import importlib.util
        import sys
        spec = importlib.util.spec_from_file_location("_fixture_freeze", FREEZE_CHECKER)
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
        return module

    def _change_event(self, event_id: str, slices: list[str],
                      rationale: str = "Fixture slice edit; Pulp-only, no "
                                       "authority boundary moves.") -> str:
        import json
        return json.dumps({
            "schema_version": 1,
            "event_id": event_id,
            "kind": "change",
            "created_at": "2026-09-20T00:00:00Z",
            "slices": sorted(slices),
            "rationale": rationale,
            "tests": ["Vellum freeze"],
            "disposition": "pulp-only",
        }, indent=2) + "\n"

    def _watch_event_for_slice_path(self) -> None:
        self._write(f"{EVENT_DIR_REL}/20260920-slice-watch.json",
                    self._event("20260920-slice-watch", SLICE_WATCH_FAMILIES))

    def _freeze_suggestion(self, output: str) -> dict:
        import json
        marker = f"{CHANGE_EVENT_DIR_REL}/"
        tail = output[output.index(marker):]
        start = tail.index("{")
        end = tail.index("\n       }") + len("\n       }")
        return json.loads(tail[start:end])

    def test_the_slice_path_is_still_classified_as_expected(self) -> None:
        """Control: if the ownership map moves, every assertion below would
        otherwise pass for the wrong reason."""
        import json
        freeze = self._import_freeze()
        mapping = json.loads((ROOT / ".github/vellum-ownership.json").read_text())
        self.assertEqual(list(freeze.affected_slices([mapping], [SLICE_PATH])),
                         [SLICE_ID])
        self.assertEqual(freeze.affected_slices([mapping], [UNWATCHED_PATH]), {})

    def test_a_slice_change_without_a_change_event_fails_with_the_json(self) -> None:
        self._write(SLICE_PATH, "// a one-line edit in a transferred slice\n")
        self._write("test/test_pointer_dispatch.cpp", "// its test\n")
        self._watch_event_for_slice_path()
        self._commit("fix: touch a transferred slice")
        code, output = self._run("main", "HEAD", "--enforce", "--inventories")
        self.assertEqual(code, 12, msg=output)
        self.assertIn("vellum-freeze (BLOCKING", output)
        self.assertIn(SLICE_ID, output)
        self.assertIn(SLICE_PATH, output)
        self.assertIn("--write-change-event", output)
        event = self._freeze_suggestion(output)
        self.assertEqual(event["slices"], [SLICE_ID])
        self.assertEqual(event["disposition"], "pulp-only")
        self.assertEqual(event["kind"], "change")
        self.assertTrue(event["rationale"].startswith("<REPLACE"), event)
        self.assertIn("ctest --test-dir build -R pulp-test-pointer-dispatch",
                      event["tests"])
        self.assertIn("Vellum freeze", event["tests"])
        self.assertEqual(event["event_id"], event["event_id"].lower())
        # With a real rationale the suggestion must validate as-is.
        freeze = self._import_freeze()
        event["rationale"] = "A fixture rationale long enough to be a real one."
        freeze.validate_event(event, f"{CHANGE_EVENT_DIR_REL}/{event['event_id']}.json")

    def test_a_committed_change_event_passes(self) -> None:
        self._write(SLICE_PATH, "// a one-line edit in a transferred slice\n")
        self._watch_event_for_slice_path()
        self._write(f"{CHANGE_EVENT_DIR_REL}/20260920-slice-fixture.json",
                    self._change_event("20260920-slice-fixture", [SLICE_ID]))
        self._commit("fix: touch a transferred slice, with its events")
        code, output = self._run("main", "HEAD", "--enforce", "--inventories")
        self.assertEqual(code, 0, msg=output)
        self.assertNotIn("vellum-freeze", output)

    def test_an_uncommitted_change_event_still_fails(self) -> None:
        self._write(SLICE_PATH, "// a one-line edit in a transferred slice\n")
        self._watch_event_for_slice_path()
        self._commit("fix: touch a transferred slice")
        self._write(f"{CHANGE_EVENT_DIR_REL}/20260920-slice-fixture.json",
                    self._change_event("20260920-slice-fixture", [SLICE_ID]))
        code, output = self._run("main", "HEAD", "--enforce")
        self.assertEqual(code, 12, msg=output)
        self.assertIn("COMMIT RANGE", output)

    def test_a_committed_placeholder_rationale_fails(self) -> None:
        """The checker accepts any non-empty rationale, so only this catches it."""
        self._write(SLICE_PATH, "// a one-line edit in a transferred slice\n")
        self._watch_event_for_slice_path()
        rel = f"{CHANGE_EVENT_DIR_REL}/20260920-slice-fixture.json"
        self._write(rel, self._change_event(
            "20260920-slice-fixture", [SLICE_ID],
            rationale="<REPLACE: what changed in the slice>"))
        self._commit("fix: touch a transferred slice, placeholder event")
        code, output = self._run("main", "HEAD", "--enforce")
        self.assertEqual(code, 12, msg=output)
        self.assertIn(rel, output)
        self.assertIn("placeholder rationale", output)

    def test_write_change_event_creates_an_event_that_clears_the_gate(self) -> None:
        self._write(SLICE_PATH, "// a one-line edit in a transferred slice\n")
        self._watch_event_for_slice_path()
        self._commit("fix: touch a transferred slice")
        code, output = self._run(
            "main", "HEAD", "--write-change-event", "--event-id",
            "20261003-slice-fixture",
            "--rationale", "Fixture edit in a transferred slice; Pulp-only, no authority moves.")
        self.assertEqual(code, 0, msg=output)
        written = self.repo / CHANGE_EVENT_DIR_REL / "20261003-slice-fixture.json"
        self.assertTrue(written.is_file(), output)
        self._commit("ci: record the change event")
        code, output = self._run("main", "HEAD", "--enforce", "--inventories")
        self.assertEqual(code, 0, msg=output)
        # And the authoritative checker agrees on the same range.
        done = subprocess.run(
            ["python3", "tools/scripts/vellum_freeze_check.py",
             "--base", _git(self.repo, "merge-base", "main", "HEAD"),
             "--head", _git(self.repo, "rev-parse", "HEAD"), "--output", os.devnull],
            cwd=self.repo, capture_output=True, text=True, check=False)
        self.assertEqual(done.returncode, 0, msg=done.stderr)

    def test_write_change_event_refuses_a_placeholder_or_nothing_owed(self) -> None:
        self._write(SLICE_PATH, "// a one-line edit in a transferred slice\n")
        self._commit("fix: touch a transferred slice")
        for rationale in ("too short", "<REPLACE: something long enough to pass>"):
            code, output = self._run("main", "HEAD", "--write-change-event",
                                     "--rationale", rationale)
            self.assertEqual(code, 2, msg=output)
        self.assertFalse((self.repo / CHANGE_EVENT_DIR_REL).exists())
        _git(self.repo, "checkout", "-q", "-b", "docs-only", "main")
        self._write(UNWATCHED_PATH, "an ordinary docs edit\n")
        self._commit("docs: outside every slice")
        code, output = self._run("main", "HEAD", "--write-change-event", "--rationale",
                                 "Nothing is owed here, so nothing must be written.")
        self.assertEqual(code, 0, msg=output)
        self.assertFalse((self.repo / CHANGE_EVENT_DIR_REL).exists())

    def test_the_freeze_base_is_the_merge_base(self) -> None:
        """main's own slice change and event must not be charged to a stale branch."""
        self._write(UNWATCHED_PATH, "an ordinary docs edit\n")
        self._commit("docs: an edit outside every slice")
        _git(self.repo, "checkout", "-q", "main")
        self._write(SLICE_PATH, "// main's own slice edit\n")
        self._watch_event_for_slice_path()
        self._write(f"{CHANGE_EVENT_DIR_REL}/20260920-main-slice.json",
                    self._change_event("20260920-main-slice", [SLICE_ID]))
        self._commit("fix: main touches a transferred slice")
        _git(self.repo, "checkout", "-q", "topic")
        code, output = self._run("main", "HEAD", "--enforce")
        self.assertEqual(code, 0, msg=output)
        self.assertEqual(output.strip(), "")
        # Control: against main's TIP the checker does refuse this branch.
        done = subprocess.run(
            ["python3", "tools/scripts/vellum_freeze_check.py",
             "--base", _git(self.repo, "rev-parse", "main"),
             "--head", _git(self.repo, "rev-parse", "HEAD"), "--output", os.devnull],
            cwd=self.repo, capture_output=True, text=True, check=False)
        self.assertEqual(done.returncode, 1, msg="the tip comparison came back clean")

    def test_a_crashing_freeze_checker_never_blocks(self) -> None:
        self._write("tools/scripts/vellum_freeze_check.py", "raise SystemExit(3)\n")
        self._write(SLICE_PATH, "// a one-line edit in a transferred slice\n")
        self._watch_event_for_slice_path()
        self._commit("fix: touch a slice with a broken checker")
        code, output = self._run("main", "HEAD", "--enforce")
        self.assertEqual(code, 0, msg=output)
        self.assertNotIn("Traceback", output)


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Prove the GPU handoff provenance generator is deterministic and fail-closed."""

from __future__ import annotations

import contextlib
import hashlib
import inspect
import io
import json
import os
import pathlib
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest


sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import gpu_handoff_provenance as provenance  # noqa: E402
import gpu_ledger_sentinel_check as sentinel_check  # noqa: E402


def git(root: pathlib.Path, *arguments: str) -> str:
    completed = subprocess.run(
        ["git", *arguments],
        cwd=root,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=True,
    )
    return completed.stdout.strip()


def build_row(path: str, identity: provenance.Identity | None = None) -> dict[str, object]:
    row: dict[str, object] = {
        "repo": provenance.HANDOFF_REPO,
        "path": path,
        "state": "pulp-owned-retained",
    }
    row.update(
        identity.as_dict()
        if identity is not None
        else {"revision": "0" * 40, "object_id": "0" * 40, "object_type": "blob"}
    )
    return row


def stale_pin_remedy(check_report: str) -> str:
    """Render what an author needs when `check` reports the ledger is stale.

    A drift report is a list of fields, which is the right shape for a machine
    and the wrong one for the person who caused it: the rows that go stale sit
    far from the edit, and nothing in a field list says the repair is
    regeneration rather than a hand edit. Returns "" when the report names no
    drift, so a failure with another cause never acquires a remedy that does
    not apply to it. One stale path produces several drift rows -- a revision
    field and an object_id field name the same path -- so the paths are
    deduplicated rather than listed once per field.
    """

    try:
        report = json.loads(check_report)
    except json.JSONDecodeError:
        return ""
    if not isinstance(report, dict):
        return ""
    drifted = sorted(
        {str(drift["path"]) for drift in report.get("drifts", ()) if "path" in drift}
    )
    command = report.get("repair_command")
    if not drifted or not command:
        return ""
    return "\n".join(
        [
            "",
            "the GPU handoff ledger is stale: this commit changed a path the "
            "ledger pins, so the row pinning it no longer describes HEAD. "
            "Regenerate the ledger and its receipt, and commit both:",
            f"  {command}",
            "pinned paths that moved:",
            *(f"  {path}" for path in drifted),
        ]
    )


class FixtureRepository(unittest.TestCase):
    """Exercise the generator against a purpose-built repository."""

    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = pathlib.Path(self.directory.name)
        git(self.root, "init", "--quiet", "--initial-branch=main")
        git(self.root, "config", "user.email", "provenance@example.invalid")
        git(self.root, "config", "user.name", "Provenance Fixture")

        (self.root / "leaf.txt").write_text("leaf one\n", encoding="utf-8")
        git(self.root, "add", "leaf.txt")
        git(self.root, "commit", "--quiet", "-m", "add leaf")
        self.first_commit = git(self.root, "rev-parse", "HEAD")

        nested = self.root / "nested"
        nested.mkdir()
        (nested / "inner.txt").write_text("inner one\n", encoding="utf-8")
        git(self.root, "add", "nested/inner.txt")
        git(self.root, "commit", "--quiet", "-m", "add nested")
        self.second_commit = git(self.root, "rev-parse", "HEAD")

    def identity_for(self, path: str) -> provenance.Identity:
        return provenance.resolve_identity(self.root, "HEAD", path)

    def document(self, rows: list[dict[str, object]]) -> dict[str, object]:
        return {"schema": "fixture", "entries": [{"pulp_paths": rows}]}

    def test_identity_names_the_owning_commit_and_object(self) -> None:
        leaf = self.identity_for("leaf.txt")
        self.assertEqual(leaf.revision, self.first_commit)
        self.assertEqual(leaf.object_type, "blob")
        self.assertEqual(leaf.object_id, git(self.root, "rev-parse", "HEAD:leaf.txt"))

        nested = self.identity_for("nested")
        self.assertEqual(nested.revision, self.second_commit)
        self.assertEqual(nested.object_type, "tree")

    def test_orphaned_pin_error_names_the_cause_and_a_usable_commit(self) -> None:
        """The error a rewritten pin produces has to end the search, not start it.

        Three lanes independently spent a day on this because the message said
        only that the commit was unreachable. Regenerating is the obvious
        repair and it cannot converge -- the new commit is not an ancestor of
        HEAD either -- so the message must say which operation orphaned the pin
        and name a commit that already satisfies the rule.
        """

        orphan = git(self.root, "rev-parse", "HEAD")
        git(self.root, "commit", "--quiet", "--amend", "-m", "rewrite nested")
        self.assertNotEqual(git(self.root, "rev-parse", "HEAD"), orphan)

        with self.assertRaises(provenance.ProvenanceError) as caught:
            provenance.resolve_source_commit(
                self.root, orphan, ["leaf.txt", "nested"]
            )
        message = str(caught.exception)
        self.assertIn(orphan, message)
        for cause in ("rebase", "amend", "merging origin/main"):
            self.assertIn(cause, message, f"the error does not name {cause!r}")
        suggested = provenance.newest_pinned_ancestor(
            self.root, ["leaf.txt", "nested"]
        )
        self.assertIsNotNone(suggested)
        self.assertIn(suggested, message)

    def test_suggested_commit_satisfies_the_rule_it_is_offered_for(self) -> None:
        """A suggestion that does not resolve is worse than none.

        The commit is correct because nothing later touched a pinned path, so
        every identity it yields is the identity HEAD yields. Assert both
        halves: it resolves, and it produces HEAD's identities.
        """

        (self.root / "unpinned.txt").write_text(
            "unrelated\n", encoding="utf-8"
        )
        git(self.root, "add", "unpinned.txt")
        git(self.root, "commit", "--quiet", "-m", "land an unrelated commit")

        paths = ["leaf.txt", "nested"]
        suggested = provenance.newest_pinned_ancestor(self.root, paths)
        self.assertEqual(suggested, self.second_commit)
        self.assertNotEqual(
            suggested,
            git(self.root, "rev-parse", "HEAD"),
            "the fixture no longer distinguishes the suggestion from HEAD",
        )
        self.assertEqual(
            provenance.resolve_source_commit(self.root, suggested, paths), suggested
        )
        for path in paths:
            self.assertEqual(
                provenance.resolve_identity(self.root, suggested, path),
                provenance.resolve_identity(self.root, "HEAD", path),
            )

    def test_missing_inventory_still_reports_the_ancestry_failure(self) -> None:
        """Enrichment must not be able to convert the error into a different one.

        `resolve_source_commit` is called from a test with two positional
        arguments, and a caller may hold no inventory at all. Both must still
        get the ancestry refusal.
        """

        orphan = git(self.root, "rev-parse", "HEAD")
        git(self.root, "commit", "--quiet", "--amend", "-m", "rewrite nested")

        self.assertIsNone(provenance.newest_pinned_ancestor(self.root, []))
        for canonical in (None, []):
            with self.assertRaises(provenance.ProvenanceError) as caught:
                provenance.resolve_source_commit(self.root, orphan, canonical)
            self.assertIn("is not an ancestor of HEAD", str(caught.exception))

    def test_identity_tracks_a_later_edit(self) -> None:
        (self.root / "leaf.txt").write_text("leaf two\n", encoding="utf-8")
        git(self.root, "add", "leaf.txt")
        git(self.root, "commit", "--quiet", "-m", "edit leaf")
        third = git(self.root, "rev-parse", "HEAD")
        self.assertEqual(self.identity_for("leaf.txt").revision, third)

    def test_identity_from_a_superseded_commit_separates_the_two_tiers(self) -> None:
        """A commit that a later commit moved past still has an identity.

        This is the shape every commit touching a pinned path produces, and it
        is the one a reader of a published receipt must be able to resolve: the
        receipt names the commit its ledger was generated from, so the question
        is what that commit held, not what HEAD holds now. Only a generator
        asks the stronger question, and it has to opt in.
        """

        (self.root / "leaf.txt").write_text("leaf two\n", encoding="utf-8")
        git(self.root, "add", "leaf.txt")
        git(self.root, "commit", "--quiet", "-m", "edit leaf")

        historical = provenance.resolve_identity(
            self.root, self.first_commit, "leaf.txt"
        )
        self.assertEqual(historical.revision, self.first_commit)
        self.assertEqual(
            historical.object_id,
            git(self.root, "rev-parse", f"{self.first_commit}:leaf.txt"),
        )
        self.assertNotEqual(
            historical.object_id, git(self.root, "rev-parse", "HEAD:leaf.txt")
        )

        with self.assertRaises(provenance.ProvenanceError) as caught:
            provenance.resolve_identity(
                self.root, self.first_commit, "leaf.txt", require_current=True
            )
        self.assertIn("at HEAD", str(caught.exception))

    def test_missing_path_fails_closed(self) -> None:
        with self.assertRaises(provenance.ProvenanceError):
            provenance.resolve_identity(self.root, "HEAD", "absent.txt")

    def test_inventory_rejects_a_circular_self_reference(self) -> None:
        document = self.document([build_row(provenance.HANDOFF_SELF_PATH)])
        with self.assertRaises(provenance.ProvenanceError) as caught:
            provenance.canonical_inventory(document)
        self.assertIn("circular self-reference", str(caught.exception))

    def test_inventory_rejects_a_foreign_repository(self) -> None:
        row = build_row("leaf.txt")
        row["repo"] = "Generous-Corp/vellum"
        with self.assertRaises(provenance.ProvenanceError):
            provenance.canonical_inventory(self.document([row]))

    def test_inventory_rejects_an_empty_path_set(self) -> None:
        with self.assertRaises(provenance.ProvenanceError):
            provenance.canonical_inventory({"entries": [{"pulp_paths": []}]})

    def test_drift_is_reported_per_field(self) -> None:
        document = self.document([build_row("leaf.txt")])
        inventory = provenance.canonical_inventory(document)
        identities = provenance.resolve_inventory_identities(
            self.root, "HEAD", inventory
        )
        drifts = provenance.compare_inventory(document, inventory, identities)
        self.assertEqual({drift.field for drift in drifts}, {"revision", "object_id"})
        self.assertEqual(drifts[0].derived, self.first_commit)

    def test_regeneration_repairs_every_planted_field(self) -> None:
        document = self.document([build_row("leaf.txt"), build_row("nested")])
        inventory = provenance.canonical_inventory(document)
        identities = provenance.resolve_inventory_identities(
            self.root, "HEAD", inventory
        )
        updated = provenance.apply_identities(document, inventory, identities)
        self.assertEqual(
            provenance.compare_inventory(updated, inventory, identities), []
        )
        self.assertEqual(
            updated["entries"][0]["pulp_paths"][1]["object_type"], "tree"
        )

    def test_regeneration_is_byte_stable(self) -> None:
        document = self.document([build_row("leaf.txt"), build_row("nested")])
        inventory = provenance.canonical_inventory(document)
        identities = provenance.resolve_inventory_identities(
            self.root, "HEAD", inventory
        )
        once = provenance.serialize_handoff(
            provenance.apply_identities(document, inventory, identities)
        )
        twice = provenance.serialize_handoff(
            provenance.apply_identities(
                json.loads(once), inventory, identities
            )
        )
        self.assertEqual(once, twice)

    def test_regeneration_preserves_unrelated_fields_and_order(self) -> None:
        row = build_row("leaf.txt")
        row["note"] = "human authored"
        document = self.document([row])
        inventory = provenance.canonical_inventory(document)
        identities = provenance.resolve_inventory_identities(
            self.root, "HEAD", inventory
        )
        updated = provenance.apply_identities(document, inventory, identities)
        emitted = updated["entries"][0]["pulp_paths"][0]
        self.assertEqual(emitted["note"], "human authored")
        self.assertEqual(list(emitted), list(row))

    def test_source_commit_must_resolve_and_be_reachable(self) -> None:
        self.assertEqual(
            provenance.resolve_source_commit(self.root, "HEAD"), self.second_commit
        )
        with self.assertRaises(provenance.ProvenanceError):
            provenance.resolve_source_commit(self.root, "0" * 40)

        git(self.root, "checkout", "--quiet", "-b", "sibling", self.first_commit)
        (self.root / "sibling.txt").write_text("sibling\n", encoding="utf-8")
        git(self.root, "add", "sibling.txt")
        git(self.root, "commit", "--quiet", "-m", "sibling work")
        sibling = git(self.root, "rev-parse", "HEAD")
        git(self.root, "checkout", "--quiet", "main")
        with self.assertRaises(provenance.ProvenanceError) as caught:
            provenance.resolve_source_commit(self.root, sibling)
        self.assertIn("not an ancestor of HEAD", str(caught.exception))

    def test_dirty_canonical_paths_are_detected(self) -> None:
        self.assertEqual(
            provenance.dirty_canonical_paths(self.root, ["leaf.txt", "nested"]), []
        )
        (self.root / "leaf.txt").write_text("modified\n", encoding="utf-8")
        (self.root / "nested" / "extra.txt").write_text("extra\n", encoding="utf-8")
        self.assertEqual(
            provenance.dirty_canonical_paths(self.root, ["leaf.txt", "nested"]),
            ["leaf.txt", "nested"],
        )

    def write_fixture_handoff(self, rows: list[dict[str, object]]) -> pathlib.Path:
        handoff = self.root / "handoff.json"
        handoff.write_text(
            provenance.serialize_handoff(self.document(rows)), encoding="utf-8"
        )
        return handoff

    def test_write_refuses_output_the_validator_would_reject(self) -> None:
        """Fail-closed: the existing validator stays the acceptance authority.

        The fixture ledger cannot satisfy the closed handoff contract, so a
        correct generator must decline to write it even though every identity
        it derived is accurate.
        """

        handoff = self.write_fixture_handoff([build_row("leaf.txt")])
        before = handoff.read_text(encoding="utf-8")
        exit_code, refusal = self.run_refusal("--handoff", str(handoff), "write")
        self.assertEqual(exit_code, 1)
        self.assertIn("still fails the handoff validator", refusal)
        self.assertEqual(handoff.read_text(encoding="utf-8"), before)

    def run_refusal(self, *arguments: str) -> tuple[int, str]:
        """Drive a refusal path and keep its text out of the suite's stderr.

        These cases exercise the refusals deliberately, so the output is
        expected -- but left on the real stderr it is indistinguishable from a
        failure, and it reads like one: a null source commit reaching Git, and
        an unclean-checkout refusal naming `leaf.txt`, which is this fixture's
        file and not the reader's. Authors debugging a red run have chased that
        text instead of the one assertion that actually failed. Capturing it
        and asserting it keeps the evidence and drops the decoy.
        """

        stream = io.StringIO()
        with contextlib.redirect_stdout(stream), contextlib.redirect_stderr(stream):
            exit_code = provenance.main(["--root", str(self.root), *arguments])
        return exit_code, stream.getvalue()

    def run_cli(self, handoff: pathlib.Path, *arguments: str) -> tuple[int, str]:
        stream = io.StringIO()
        with contextlib.redirect_stdout(stream):
            exit_code = provenance.main(
                ["--root", str(self.root), "--handoff", str(handoff), *arguments]
            )
        return exit_code, stream.getvalue()

    def test_paths_subcommand_lists_the_inventory(self) -> None:
        handoff = self.write_fixture_handoff(
            [build_row("nested"), build_row("leaf.txt")]
        )
        exit_code, text = self.run_cli(handoff, "paths")
        self.assertEqual(exit_code, 0)
        self.assertEqual(text.split(), ["leaf.txt", "nested"])

        exit_code, text = self.run_cli(handoff, "paths", "--json")
        self.assertEqual(exit_code, 0)
        self.assertEqual(
            json.loads(text), {"count": 2, "paths": ["leaf.txt", "nested"]}
        )

    def test_check_subcommand_reports_the_drifted_fields(self) -> None:
        """Exit status alone cannot distinguish drift from a contract failure.

        The fixture ledger always violates the closed contract, so asserting
        only the exit code would pass with the identity derivation removed.
        """

        handoff = self.write_fixture_handoff([build_row("leaf.txt")])
        exit_code, text = self.run_cli(handoff, "check", "--json")
        self.assertEqual(exit_code, 1)
        report = json.loads(text)
        self.assertEqual(report["row_count"], 1)
        self.assertEqual(report["drift_count"], 2)
        self.assertEqual(
            {drift["field"] for drift in report["drifts"]},
            {"revision", "object_id"},
        )
        self.assertEqual(
            [drift["derived"] for drift in report["drifts"] if drift["field"] == "revision"],
            [self.first_commit],
        )

        exit_code, text = self.run_cli(handoff, "check")
        self.assertEqual(exit_code, 1)
        self.assertIn("STALE", text)
        self.assertIn("leaf.txt", text)

    def test_a_stale_pin_failure_names_the_regenerate_command(self) -> None:
        """What an author reads has to carry the repair, not a verdict word.

        A pinned row goes stale whenever a commit edits the path it pins, which
        is an ordinary thing for a branch to do. The two facts that let the
        author act are the path that moved and the exact invocation that
        re-pins it; a drift field list carries neither, and sends them looking
        for a hand edit that does not exist.
        """

        handoff = self.write_fixture_handoff([build_row("leaf.txt")])
        exit_code, report = self.run_cli(handoff, "check", "--json")
        self.assertEqual(exit_code, 1)

        remedy = stale_pin_remedy(report)
        self.assertIn(
            "leaf.txt", remedy, "the remedy does not name the path that moved"
        )
        self.assertRegex(
            remedy,
            r"gpu_handoff_provenance\.py .*write --source-commit [0-9a-f]{40} "
            r"--receipt",
            "the remedy does not carry the exact regenerate invocation",
        )
        self.assertIn("Regenerate the ledger and its receipt", remedy)

        # Negative controls: nothing else may acquire a remedy. A report with
        # no drift is not stale, and an unreadable one is not evidence.
        self.assertEqual(
            stale_pin_remedy(json.dumps({"drifts": [], "repair_command": "cmd"})),
            "",
            "a report naming no drift was decorated with a regenerate command",
        )
        self.assertEqual(stale_pin_remedy("not json"), "")
        self.assertEqual(
            stale_pin_remedy(json.dumps({"drifts": [{"path": "leaf.txt"}]})),
            "",
            "a remedy was rendered without the command it is supposed to give",
        )

    def test_the_printed_repair_command_actually_parses(self) -> None:
        """A repair command that argparse rejects is worse than none."""

        command = provenance.repair_command(provenance.DEFAULT_HANDOFF, "HEAD")
        arguments = shlex.split(command)[2:]
        parsed = provenance.build_parser().parse_args(arguments)
        self.assertEqual(parsed.command, "write")
        self.assertEqual(parsed.source_commit, "HEAD")
        self.assertEqual(parsed.receipt, provenance.DEFAULT_RECEIPT)

        elsewhere = self.root / "handoff.json"
        alternate = provenance.repair_command(elsewhere, "HEAD")
        parsed = provenance.build_parser().parse_args(shlex.split(alternate)[2:])
        self.assertEqual(parsed.command, "write")
        self.assertEqual(parsed.handoff, elsewhere)

    def test_receipt_subcommand_writes_a_receipt(self) -> None:
        handoff = self.write_fixture_handoff([build_row("leaf.txt")])
        output = self.root / "receipts" / "receipt.json"
        self.assertEqual(
            provenance.main(
                [
                    "--root",
                    str(self.root),
                    "--handoff",
                    str(handoff),
                    "receipt",
                    "--output",
                    str(output),
                ]
            ),
            0,
        )
        receipt = json.loads(output.read_text(encoding="utf-8"))
        self.assertEqual(receipt["source_commit"], self.second_commit)
        self.assertEqual(receipt["canonical_paths"], ["leaf.txt"])

    def test_check_binds_the_receipt_to_the_ledger_bytes(self) -> None:
        """`check` must not exit 0 while the receipt names other bytes.

        The fixture ledger always fails the closed contract, so the exit code
        alone cannot carry this claim; the receipt field in the report is the
        discriminating assertion, and the RECEIPT line is what a reader sees.
        """

        handoff = self.write_fixture_handoff([build_row("leaf.txt")])
        receipt = self.root / "receipts" / "receipt.json"
        self.assertEqual(
            provenance.main(
                ["--root", str(self.root), "--handoff", str(handoff),
                 "receipt", "--output", str(receipt)]
            ),
            0,
        )
        _, text = self.run_cli(handoff, "check", "--json", "--receipt", str(receipt))
        self.assertEqual(json.loads(text)["receipt"]["state"], "bound")
        _, text = self.run_cli(handoff, "check", "--receipt", str(receipt))
        self.assertNotIn("RECEIPT", text)

        # Regenerate the ledger without its receipt: the receipt now describes
        # bytes that no longer exist, the exact state a `--theirs` merge or a
        # bare `write` leaves behind.
        handoff.write_text(
            provenance.serialize_handoff(self.document([build_row("nested")])),
            encoding="utf-8",
        )
        exit_code, text = self.run_cli(
            handoff, "check", "--json", "--receipt", str(receipt)
        )
        self.assertEqual(exit_code, 1)
        report = json.loads(text)["receipt"]
        self.assertEqual(report["state"], "stale")
        self.assertTrue(any("sha256" in problem for problem in report["problems"]))
        self.assertTrue(
            any("canonical_paths" in problem for problem in report["problems"])
        )
        exit_code, text = self.run_cli(handoff, "check", "--receipt", str(receipt))
        self.assertEqual(exit_code, 1)
        self.assertIn("RECEIPT", text)
        self.assertIn("repair with:", text)

    def test_check_says_out_loud_when_there_is_no_receipt_to_bind(self) -> None:
        handoff = self.write_fixture_handoff([build_row("leaf.txt")])
        missing = self.root / "no-such-receipt.json"
        _, text = self.run_cli(handoff, "check", "--json", "--receipt", str(missing))
        self.assertEqual(json.loads(text)["receipt"]["state"], "absent")
        _, text = self.run_cli(handoff, "check", "--receipt", str(missing))
        self.assertIn("binding not checked", text)

    def test_an_unresolvable_source_commit_is_an_environment_error(self) -> None:
        handoff = self.write_fixture_handoff([build_row("leaf.txt")])
        exit_code, refusal = self.run_refusal(
            "--handoff",
            str(handoff),
            "check",
            "--source-commit",
            "0" * 40,
        )
        self.assertEqual(exit_code, 2)
        self.assertIn("does not resolve to a commit", refusal)

    def test_write_refuses_an_unclean_checkout(self) -> None:
        handoff = self.root / "handoff.json"
        handoff.write_text(
            provenance.serialize_handoff(self.document([build_row("leaf.txt")])),
            encoding="utf-8",
        )
        (self.root / "leaf.txt").write_text("modified\n", encoding="utf-8")
        exit_code, refusal = self.run_refusal("--handoff", str(handoff), "write")
        self.assertEqual(exit_code, 2)
        self.assertIn("refusing to generate from an unclean checkout", refusal)
        self.assertIn("leaf.txt", refusal)
        self.assertIn('"revision": "' + "0" * 40, handoff.read_text(encoding="utf-8"))


class CheckedInLedger(unittest.TestCase):
    """Bind the shipped ledger to the generator that is supposed to own it."""

    def setUp(self) -> None:
        self.root = provenance.ROOT
        self.handoff = provenance.DEFAULT_HANDOFF

    def test_checked_in_encoding_is_the_deterministic_encoding(self) -> None:
        raw = self.handoff.read_text(encoding="utf-8")
        self.assertEqual(provenance.serialize_handoff(json.loads(raw)), raw)

    def test_canonical_inventory_covers_the_shipped_ledger(self) -> None:
        document = provenance.load_handoff(self.handoff)
        inventory = provenance.canonical_inventory(document)
        self.assertGreater(len(inventory), 0)
        self.assertNotIn(
            provenance.HANDOFF_SELF_PATH, {row.path for row in inventory}
        )
        for path in provenance.canonical_paths(document):
            self.assertTrue((self.root / path).exists(), f"{path} is absent")

    def test_regenerating_the_shipped_ledger_reproduces_it_exactly(self) -> None:
        """Regeneration at HEAD must be a byte-identical no-op.

        This is the currency claim, so it is opt-in. It goes red the moment any
        commit changes a pinned path without regenerating the ledger, which is
        the correct report for a consumer and the wrong one for a gate that
        runs on every commit: it would make each edit to a pinned path a
        two-commit operation and serialize concurrent pull requests. The
        always-on claim lives in the provenance case below.
        """

        self.require_current_opt_in()
        self.require_clean_canonical_paths()
        document = provenance.load_handoff(self.handoff)
        inventory = provenance.canonical_inventory(document)
        commit = provenance.resolve_source_commit(self.root, "HEAD")
        identities = provenance.resolve_inventory_identities(
            self.root, commit, inventory
        )
        drifts = provenance.compare_inventory(document, inventory, identities)
        self.assertEqual(
            [
                f"{drift.row.label} {drift.row.path} {drift.field}"
                for drift in drifts
            ],
            [],
            "regenerate with: "
            + provenance.repair_command(self.handoff, commit),
        )
        self.assertEqual(
            provenance.serialize_handoff(
                provenance.apply_identities(document, inventory, identities)
            ),
            self.handoff.read_text(encoding="utf-8"),
        )

    def require_current_opt_in(self) -> None:
        """Skip unless this run explicitly asked for currency at HEAD.

        Currency is a property of the checkout at a moment: every commit that
        touches a pinned path falsifies it until the ledger is regenerated. The
        required merge gate runs this suite on every commit, so asserting
        currency here would go red for pull requests that have nothing to do
        with the handoff. Consumers that want the stronger claim set this.
        """

        if os.environ.get("PULP_GPU_HANDOFF_REQUIRE_CURRENT") != "1":
            self.skipTest(
                "currency at HEAD is opt-in: set "
                "PULP_GPU_HANDOFF_REQUIRE_CURRENT=1, or run "
                "gpu_handoff_provenance.py check"
            )

    def test_the_shipped_ledger_satisfies_the_provenance_tier(self) -> None:
        """The always-on claim, and what the required gate actually asserts.

        Every pinned revision is still an ancestor and still carries the blob
        or tree it names. Nothing a later commit does can falsify that, so this
        case is safe to run on every commit -- which is the whole reason
        currency was separated out of it.
        """

        self.require_clean_canonical_paths()
        document = provenance.load_handoff(self.handoff)
        self.assertEqual(provenance.validate_with_catalog(document, self.root), [])

    def test_a_pin_a_later_commit_moved_past_fails_only_the_currency_tier(self) -> None:
        """Separate the two tiers on the exact input that used to conflate them.

        Re-pinning a row to an earlier commit that really did carry this blob
        reproduces the state every commit touching a pinned path produces: the
        provenance claim is still true, the currency claim is not. Before the
        split both reported the same stale-identity problem, so an unrelated
        commit could turn the required gate red.
        """

        self.require_clean_canonical_paths()
        document = provenance.load_handoff(self.handoff)
        path = "tools/scripts/gpu_recipe_catalog.py"
        row = next(
            row
            for entry in document["entries"]
            for row in entry["pulp_paths"]
            if row["path"] == path
        )
        history = git(
            self.root, "log", "--format=%H", "HEAD", "--", path
        ).splitlines()
        self.assertGreaterEqual(len(history), 2)
        row["revision"] = history[1]
        row["object_id"] = git(self.root, "rev-parse", f"{history[1]}:{path}")
        self.assertEqual(provenance.validate_with_catalog(document, self.root), [])
        problems = provenance.validate_with_catalog(
            document, self.root, require_current=True
        )
        self.assertTrue(
            any("pin is not current at HEAD" in problem for problem in problems),
            f"expected a currency problem, got {problems}",
        )

    def require_clean_canonical_paths(self) -> None:
        """Skip rather than red misleadingly on a developer's local edits.

        These cases generate against the live checkout, and the generator
        refuses an unclean one by design. Without this the failure reads as a
        generator defect instead of an uncommitted edit to a pinned path.
        """

        document = provenance.load_handoff(self.handoff)
        dirty = provenance.dirty_canonical_paths(
            self.root, provenance.canonical_paths(document)
        )
        if dirty:
            self.skipTest(f"canonical paths are modified locally: {dirty}")

    def test_write_against_a_current_ledger_changes_nothing(self) -> None:
        self.require_current_opt_in()
        self.require_clean_canonical_paths()
        with tempfile.TemporaryDirectory() as directory:
            copy = pathlib.Path(directory) / "gpu-vellum-handoff.yaml"
            shutil.copyfile(self.handoff, copy)
            before = copy.read_text(encoding="utf-8")
            exit_code = provenance.main(
                [
                    "--root",
                    str(self.root),
                    "--handoff",
                    str(copy),
                    "write",
                ]
            )
            self.assertEqual(exit_code, 0)
            self.assertEqual(copy.read_text(encoding="utf-8"), before)

    def test_write_refuses_when_a_pinned_identity_cannot_be_satisfied(self) -> None:
        """Reach the identity gate specifically, not a schema gate.

        A fixture ledger fails the closed contract long before any Git identity
        is compared, so refusing one proves only that problems block a write.
        This corrupts a real pinned row so the refusal has to come from the
        stale-identity check itself.
        """

        self.require_clean_canonical_paths()
        document = provenance.load_handoff(self.handoff)
        document["entries"][0]["pulp_paths"][0]["object_id"] = "0" * 40
        with tempfile.TemporaryDirectory() as directory:
            copy = pathlib.Path(directory) / "gpu-vellum-handoff.yaml"
            copy.write_text(provenance.serialize_handoff(document), encoding="utf-8")
            problems = provenance.validate_with_catalog(document, self.root)
            self.assertTrue(
                any("stale revision/blob/tree identity" in problem for problem in problems),
                f"expected a stale-identity problem, got {problems}",
            )

    def test_receipt_binds_the_source_commit_to_the_emitted_bytes(self) -> None:
        document = provenance.load_handoff(self.handoff)
        rendered = provenance.serialize_handoff(document)
        commit = provenance.resolve_source_commit(self.root, "HEAD")
        receipt = provenance.build_receipt(document, commit, rendered, self.handoff)
        self.assertEqual(receipt["schema"], provenance.RECEIPT_SCHEMA)
        self.assertEqual(receipt["source_commit"], commit)
        self.assertEqual(
            receipt["canonical_path_count"], len(receipt["canonical_paths"])
        )
        self.assertEqual(
            receipt["handoff_sha256"],
            hashlib.sha256(rendered.encode("utf-8")).hexdigest(),
        )
        self.assertEqual(
            receipt["canonical_path_sha256"],
            hashlib.sha256(
                "\n".join(provenance.canonical_paths(document)).encode("utf-8") + b"\n"
            ).hexdigest(),
        )

    def test_published_receipt_binds_the_checked_in_ledger(self) -> None:
        """The published receipt must describe the ledger that is shipped.

        It goes stale when the ledger is regenerated without its receipt, which
        is the only way the receipt could claim a source it did not produce.
        """

        self.assertTrue(
            provenance.DEFAULT_RECEIPT.exists(),
            f"{provenance.DEFAULT_RECEIPT} is absent",
        )
        receipt = json.loads(
            provenance.DEFAULT_RECEIPT.read_text(encoding="utf-8")
        )
        repair = (
            "regenerate with: python3 tools/scripts/gpu_handoff_provenance.py "
            "write --receipt"
        )
        self.assertEqual(receipt["schema"], provenance.RECEIPT_SCHEMA)
        self.assertEqual(
            receipt["handoff_sha256"],
            hashlib.sha256(self.handoff.read_bytes()).hexdigest(),
            repair,
        )
        document = provenance.load_handoff(self.handoff)
        self.assertEqual(
            receipt["canonical_paths"], provenance.canonical_paths(document), repair
        )
        self.assertEqual(
            provenance.resolve_source_commit(self.root, receipt["source_commit"]),
            receipt["source_commit"],
            "the receipt's source commit is not an ancestor of HEAD",
        )
        # Naming a reachable commit is not the claim. The claim is that THIS
        # commit produced THESE bytes, so regenerate from it and compare.
        # Resolving from the receipt's own commit is the provenance question,
        # so it must not ask whether HEAD still agrees -- a later commit
        # touching a pinned path does not make the receipt any less true.
        inventory = provenance.canonical_inventory(document)
        try:
            identities = provenance.resolve_inventory_identities(
                self.root, receipt["source_commit"], inventory
            )
        except provenance.ProvenanceError as error:
            self.fail(
                "the receipt names a source commit that cannot produce this "
                f"ledger ({error}); {repair}"
            )
        regenerated = provenance.serialize_handoff(
            provenance.apply_identities(document, inventory, identities)
        )
        self.assertEqual(
            hashlib.sha256(regenerated.encode("utf-8")).hexdigest(),
            receipt["handoff_sha256"],
            "the receipt names a source commit that does not produce this ledger; "
            + repair,
        )

    def test_check_reports_a_clean_ledger(self) -> None:
        # `check` is a consumer, so it asks for currency; that makes it opt-in
        # here for the same reason the regeneration case above is.
        self.require_current_opt_in()
        self.require_clean_canonical_paths()
        self.assertEqual(
            provenance.main(
                ["--root", str(self.root), "--handoff", str(self.handoff), "check"]
            ),
            0,
        )



class TruncatedCheckoutRefusal(unittest.TestCase):
    """A short history must make the generator refuse, not emit boundary SHAs.

    ``git log -1 --format=%H <commit> -- <path>`` exits 0 on a truncated
    checkout and answers with the graft boundary. Written through, that is a
    well-formed SHA pinned to the oldest commit the checkout happens to hold --
    a correct ledger silently replaced with a wrong one, at exit status 0.
    """

    @classmethod
    def setUpClass(cls) -> None:
        cls._workspace = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls._workspace.cleanup)
        base = pathlib.Path(cls._workspace.name)
        cls.root = base / "truncated"
        # --no-local is load-bearing: with a local clone Git hardlinks the
        # source object store and ignores --depth, so the fixture would carry
        # the full history and every assertion here would be vacuous.
        git(
            provenance.ROOT,
            "clone",
            "--quiet",
            "--depth=1",
            "--no-local",
            f"file://{provenance.ROOT}",
            str(cls.root),
        )
        cls.handoff = cls.root / "docs/status/gpu-vellum-handoff.yaml"

    def setUp(self) -> None:
        provenance._BOUNDARY_CACHE.clear()
        self.assertEqual(git(self.root, "rev-list", "--count", "HEAD"), "1")

    def run_cli(self, *arguments: str) -> tuple[int, str, str]:
        out = io.StringIO()
        err = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            exit_code = provenance.main(
                ["--root", str(self.root), "--handoff", str(self.handoff), *arguments]
            )
        return exit_code, out.getvalue(), err.getvalue()

    def test_shallow_write_refuses_and_leaves_the_ledger_byte_identical(self) -> None:
        before = hashlib.sha256(self.handoff.read_bytes()).hexdigest()
        exit_code, _, errors = self.run_cli("write")
        self.assertEqual(exit_code, 2, errors)
        self.assertIn("history is truncated", errors)
        self.assertIn("git fetch --unshallow", errors)
        # The byte comparison is the whole point: an exit code says the tool
        # reported a problem, only the digest says it did not write first.
        self.assertEqual(
            hashlib.sha256(self.handoff.read_bytes()).hexdigest(), before
        )

    def test_shallow_check_names_the_truncated_checkout(self) -> None:
        exit_code, _, errors = self.run_cli("check")
        self.assertEqual(exit_code, 2, errors)
        self.assertIn("history is truncated", errors)
        self.assertIn("shallow graft boundary", errors)

    def test_shallow_paths_still_lists_the_inventory(self) -> None:
        """``paths`` asks no history question and must keep working."""

        exit_code, text, errors = self.run_cli("paths")
        self.assertEqual(exit_code, 0, errors)
        self.assertGreater(len(text.split()), 0)


class MergeSentinel(unittest.TestCase):
    """Prove the merge-driver sentinel cannot be committed quietly.

    The driver resolves a ledger collision to an identity that is invalid on
    purpose, because the alternatives that produce a *plausible* value -- a
    driver that recomputes the pin, or ``merge=ours`` -- both emit something
    the always-on provenance tier accepts, so Git commits a stale pin and
    nothing says a word. That property is the whole design, and it is only true
    if the validator really does reject the sentinel. These cases are what
    answer that against the shipped generator rather than by assertion.
    """

    SENTINEL = "regenerate-me"
    ROW = "handoff entries[0].pulp_paths[0]"

    def setUp(self) -> None:
        self.root = provenance.ROOT
        self.handoff = provenance.DEFAULT_HANDOFF
        self.driver = self.root / "tools/scripts/gpu_ledger_merge_driver.py"

    def rows_for(self, problems: list[str]) -> list[str]:
        return [problem for problem in problems if problem.startswith(self.ROW)]

    def poison_with_driver(self, source: pathlib.Path, into: pathlib.Path) -> None:
        """Run the real driver over one file, with no disagreement to resolve.

        The driver takes three sides. Handing it the same content as base, ours
        and theirs isolates the normalization -- what it poisons and what it
        leaves alone -- from the merge, which the collision cases cover.
        """

        shutil.copyfile(source, into)
        subprocess.run(
            [str(self.driver), str(source), str(into), str(source)],
            check=True, stdin=subprocess.DEVNULL, timeout=30,
        )

    def test_the_routing_validator_rejects_a_sentinel_identity(self) -> None:
        """The named tier, in isolation, on the real shipped ledger."""

        document = provenance.load_handoff(self.handoff)
        # Control: this row is clean in this checkout, so any rejection below
        # is the sentinel rather than local drift that would fake the result.
        self.assertEqual(
            self.rows_for(catalog_module().validate_handoff_routing(document, self.root)),
            [],
        )
        document["entries"][0]["pulp_paths"][0]["object_id"] = self.SENTINEL
        self.assertEqual(
            self.rows_for(catalog_module().validate_handoff_routing(document, self.root)),
            [f"{self.ROW} has stale revision/blob/tree identity"],
        )

    def test_a_sentinel_ledger_is_rejected_by_both_validator_tiers(self) -> None:
        """What the required gate actually sees, and why no parse error hides it.

        ``validate_with_catalog`` is what the always-on provenance case calls.
        The sentinel is a legal JSON string, so the document still parses and
        both tiers get to speak: the schema tier because the value is not a
        40-character hex object id, and the routing tier because it does not
        match the blob the pinned revision carries. Two independent rejections
        for one sentinel is the belt-and-braces the design wants, and it is a
        fact about the shipped validator, not an intention.
        """

        document = provenance.load_handoff(self.handoff)
        self.assertEqual(
            self.rows_for(provenance.validate_with_catalog(document, self.root)), []
        )
        document["entries"][0]["pulp_paths"][0]["object_id"] = self.SENTINEL
        self.assertEqual(
            self.rows_for(provenance.validate_with_catalog(document, self.root)),
            [
                f"{self.ROW} lacks exact Pulp object identity",
                f"{self.ROW} has stale revision/blob/tree identity",
            ],
        )

    def test_the_driver_poisons_only_what_the_generator_can_regenerate(self) -> None:
        """Structural, per-array, against the parsed documents.

        ``write`` derives ``pulp_paths`` identities from this repository's own
        history, and rewrites nothing else. ``vellum_paths`` rows are constants
        pinned to a fixed foreign revision and checked against a hardcoded
        table, so a sentinel there is damage no repair command can undo: the
        generator refuses the document and names a row it will never rewrite.
        That is not hypothetical -- a driver that poisoned every ``object_id``
        in the file left a ledger ``write`` would not repair.

        Counted from the parsed JSON rather than from the driver's own notion
        of which rows are which, so an expectation cannot shrink along with a
        bug.
        """

        self.assertTrue(os.access(self.driver, os.X_OK), f"{self.driver} is not executable")
        original = provenance.load_handoff(self.handoff)
        with tempfile.TemporaryDirectory() as directory:
            merged = pathlib.Path(directory) / "ours"
            self.poison_with_driver(self.handoff, merged)
            # Still a document the validator can read, or the rejection the
            # design relies on would be a parse error wearing its name.
            poisoned = json.loads(merged.read_text(encoding="utf-8"))

        pulp_rows = vellum_rows = 0
        for before, after in zip(original["entries"], poisoned["entries"]):
            for row_before, row_after in zip(before["pulp_paths"], after["pulp_paths"]):
                pulp_rows += 1
                # Both fields `write` rewrites per re-pin. Poisoning only one of
                # them would leave the other differing between the two sides, so
                # every row would come back as a conflict and the driver would
                # buy nothing.
                for field in ("revision", "object_id"):
                    self.assertEqual(row_after[field], self.SENTINEL)
                regenerable = {"revision", "object_id"}
                self.assertEqual(
                    {k: v for k, v in row_before.items() if k not in regenerable},
                    {k: v for k, v in row_after.items() if k not in regenerable},
                    "the driver changed a field other than the identity",
                )
            for row_before, row_after in zip(before["vellum_paths"], after["vellum_paths"]):
                vellum_rows += 1
                self.assertEqual(row_before, row_after, "a Vellum constant was poisoned")

        self.assertEqual(pulp_rows, len(provenance.canonical_inventory(original)))
        self.assertGreater(vellum_rows, 0, "no Vellum rows present to protect")
        for key in ("entries",):
            self.assertEqual(len(original[key]), len(poisoned[key]))
        self.assertEqual(
            {k: v for k, v in original.items() if k != "entries"},
            {k: v for k, v in poisoned.items() if k != "entries"},
        )

    def test_the_driver_poisons_the_receipt_digest(self) -> None:
        """The receipt carries no object id, so its binding digest is the lie.

        ``handoff_sha256`` is what ties the receipt to the exact ledger bytes.
        Left to merge cleanly it would resolve to the other side's digest --
        stale, plausible, and silent -- so the driver invalidates it for the
        same reason it invalidates a pin.
        """

        with tempfile.TemporaryDirectory() as directory:
            merged = pathlib.Path(directory) / "ours"
            self.poison_with_driver(provenance.DEFAULT_RECEIPT, merged)
            receipt = json.loads(merged.read_text(encoding="utf-8"))
        self.assertEqual(receipt["handoff_sha256"], self.SENTINEL)
        # The commit the receipt names is regenerated with the digest, and
        # differs on both sides of every re-pin, so it is normalized too.
        self.assertEqual(receipt["source_commit"], self.SENTINEL)
        # The path list is NOT poisoned: it moves only when a row set moves, so
        # it is one-sided exactly when a row addition is one-sided.
        self.assertEqual(
            receipt["canonical_paths"],
            json.loads(
                provenance.DEFAULT_RECEIPT.read_text(encoding="utf-8")
            )["canonical_paths"],
        )
        self.assertNotEqual(
            receipt["handoff_sha256"],
            hashlib.sha256(self.handoff.read_bytes()).hexdigest(),
        )

    def three_way(self, directory, base, ours, theirs):
        """Run the real driver over three encoded documents, as Git would."""

        scratch = pathlib.Path(directory)
        written = {}
        for name, document in (("base", base), ("ours", ours), ("theirs", theirs)):
            written[name] = scratch / name
            written[name].write_text(
                provenance.serialize_handoff(document), encoding="utf-8"
            )
        completed = subprocess.run(
            [str(self.driver), str(written["base"]), str(written["ours"]),
             str(written["theirs"])],
            check=False, stdin=subprocess.DEVNULL, timeout=30,
            capture_output=True, text=True,
        )
        return completed, written["ours"].read_text(encoding="utf-8")

    @staticmethod
    def repinned(document, revision):
        """The ledger as a later commit re-pins it: identities move, rows do not."""

        moved = json.loads(json.dumps(document))
        for entry in moved["entries"]:
            for row in entry["pulp_paths"]:
                row["revision"] = revision
                row["object_id"] = revision
        return moved

    def test_a_row_one_side_added_survives_a_re_pin_on_the_other(self) -> None:
        """The collision the driver exists for, and the one it must not eat.

        A branch adds a pinned path and re-pins; main only re-pins. Resolving
        the file to either side would be silent data loss: the regeneration that
        follows only rewrites the identity fields of rows that already exist, so
        a dropped row never comes back, and the always-on provenance tier only
        checks that the rows still present route correctly -- a ledger missing a
        row validates clean. Row additions are not hypothetical; main carries
        them regularly.
        """

        base = provenance.load_handoff(self.handoff)
        ours = self.repinned(base, "a" * 40)
        added = json.loads(json.dumps(ours["entries"][0]["pulp_paths"][0]))
        added["path"] = "core/gpu/added_by_the_branch.hpp"
        ours["entries"][0]["pulp_paths"].append(added)
        theirs = self.repinned(base, "b" * 40)

        with tempfile.TemporaryDirectory() as directory:
            completed, merged = self.three_way(directory, base, ours, theirs)

        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertNotIn("<<<<<<<", merged, "churn was reported as a disagreement")
        document = json.loads(merged)
        paths = [
            row["path"] for entry in document["entries"] for row in entry["pulp_paths"]
        ]
        self.assertIn("core/gpu/added_by_the_branch.hpp", paths)
        self.assertEqual(
            len(paths), len(provenance.canonical_inventory(base)) + 1,
            "the merge changed the row set by something other than the addition",
        )
        # Still sentinelled, or the row survived at the cost of the guarantee.
        self.assertEqual(
            {
                row["object_id"]
                for entry in document["entries"] for row in entry["pulp_paths"]
            },
            {self.SENTINEL},
        )
        self.assertEqual(
            [entry["vellum_paths"] for entry in document["entries"]],
            [entry["vellum_paths"] for entry in base["entries"]],
            "a Vellum constant moved",
        )

    def test_two_authors_editing_one_row_still_conflicts_loudly(self) -> None:
        """Normalizing the churn must not normalize away a real disagreement.

        The driver silences the identity fields precisely because nobody edits
        them by hand. Everything else is content, and two sides changing the
        same content differently is the case that has to come back as markers
        and a nonzero exit -- today's behaviour, which the fix has to keep.
        """

        base = provenance.load_handoff(self.handoff)
        ours = json.loads(json.dumps(base))
        theirs = json.loads(json.dumps(base))
        ours["entries"][0]["pulp_paths"][0]["path"] = "core/gpu/ours.hpp"
        theirs["entries"][0]["pulp_paths"][0]["path"] = "core/gpu/theirs.hpp"

        with tempfile.TemporaryDirectory() as directory:
            completed, merged = self.three_way(directory, base, ours, theirs)

        self.assertNotEqual(completed.returncode, 0, "a real collision merged silently")
        for marker in ("<<<<<<<", ">>>>>>>"):
            self.assertIn(marker, merged)
        for side in ("core/gpu/ours.hpp", "core/gpu/theirs.hpp"):
            self.assertIn(side, merged, "a side was dropped from the conflict")

    def test_both_generated_paths_are_routed_to_the_driver(self) -> None:
        """A driver nothing routes to is a driver that never runs."""

        attributes = (self.root / ".gitattributes").read_text(encoding="utf-8")
        for path in (
            self.handoff.relative_to(self.root),
            provenance.DEFAULT_RECEIPT.relative_to(self.root),
        ):
            self.assertIn(f"{path} merge=pulp-gpu-ledger", attributes)

    def test_the_bootstrap_registers_the_driver_git_resolves(self) -> None:
        """``merge=<name>`` with no local driver behind it silently text-merges.

        That failure is invisible: the attribute is still there, the conflict
        just comes back anyway. So this runs the real installer against a
        scratch repository and reads the configured value back out of Git,
        rather than grepping the script -- the installer assembles the value
        from a shell variable, so the string it ends up writing appears nowhere
        in its own source.
        """

        installer = self.root / "tools/scripts/install-githooks.sh"
        with tempfile.TemporaryDirectory() as directory:
            scratch = pathlib.Path(directory)
            git(scratch, "init", "--quiet", "--initial-branch=main")
            (scratch / ".githooks").mkdir()
            (scratch / ".githooks/pre-push").write_text("#!/bin/sh\n")
            target = scratch / "tools/scripts"
            target.mkdir(parents=True)
            for source in (installer, self.driver):
                copied = target / source.name
                shutil.copyfile(source, copied)
                copied.chmod(0o755)
            subprocess.run(
                [str(target / installer.name)],
                cwd=scratch, check=True, stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL, timeout=60,
            )
            configured = git(
                scratch, "config", "--get", "merge.pulp-gpu-ledger.driver"
            )
            self.assertTrue(
                git(scratch, "config", "--get", "merge.pulp-gpu-ledger.name")
            )
            self.assertEqual(git(scratch, "config", "--get", "core.hooksPath"), ".githooks")
            # Re-running must not double-register or start failing: setup.sh
            # calls this on every bootstrap of an already-configured checkout.
            subprocess.run(
                [str(target / installer.name)],
                cwd=scratch, check=True, stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL, timeout=60,
            )
            self.assertEqual(
                git(scratch, "config", "--get", "merge.pulp-gpu-ledger.driver"),
                configured,
            )

        command = shlex.split(configured)
        self.assertEqual(
            command[0], str(self.driver.relative_to(self.root))
        )
        # %O is base, %A is ours and the output file, %B is theirs. Dropping
        # %O leaves the driver unable to tell a re-pin from a row somebody
        # added, which is how a two-sided driver loses content in silence.
        self.assertEqual(command[1:], ["%O", "%A", "%B"])

    def test_the_sentinel_check_reads_a_merged_ledger_and_refuses_it(self) -> None:
        """The local half of the design, end to end on real driver output.

        The pin-freshness guard cannot cover this: it fires when a pinned path
        changes and the ledger does not, and a sentinel merge changes the
        ledger, so it reads the sentinel as the refresh it was waiting for. The
        conflict-marker guard cannot either -- the driver's whole purpose is
        that there are no markers.
        """

        # Spelled out rather than taken from sentinel_check.LEDGERS: naming the
        # files the guard is supposed to cover is the entire assertion, and
        # deriving them from the guard's own list would shrink the expectation
        # in lockstep with the bug. Dropping the receipt from that list left
        # this case green until it was written this way.
        expected = [
            pathlib.Path("docs/status/gpu-vellum-handoff.yaml"),
            pathlib.Path("docs/validation/gpu-handoff-provenance/receipt.json"),
        ]
        with tempfile.TemporaryDirectory() as directory:
            fake = pathlib.Path(directory)
            for relative in expected:
                target = fake / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                self.poison_with_driver(self.root / relative, target)
            # Control: the pristine tree this was copied from must be silent,
            # or a guard that fires on everything would look like one that
            # detected something.
            self.assertEqual(sentinel_check.sentinel_files(self.root), [])
            self.assertEqual(sentinel_check.sentinel_files(fake), expected)
            with contextlib.redirect_stderr(io.StringIO()) as reported:
                self.assertEqual(sentinel_check.main(["--root", str(fake)]), 1)
            for relative in expected:
                self.assertIn(str(relative), reported.getvalue())
            self.assertIn(sentinel_check.REPAIR, reported.getvalue())
            with contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(
                    sentinel_check.main(["--root", str(fake), "--mode", "hint"]), 0
                )

    def test_the_guard_reads_the_commit_being_pushed_not_the_working_tree(self) -> None:
        """A clean tree can sit over a sentinel tip, and the tip is what ships.

        Regenerate-but-forget-to-commit leaves exactly that state: `write` has
        repaired the files on disk, so a working-tree scan is silent, while the
        commit the push carries still reads `regenerate-me` and CI rejects it
        twenty minutes later. The push gate has to read the object, not the file.
        """

        expected = [
            pathlib.Path("docs/status/gpu-vellum-handoff.yaml"),
            pathlib.Path("docs/validation/gpu-handoff-provenance/receipt.json"),
        ]
        with tempfile.TemporaryDirectory() as directory:
            scratch = pathlib.Path(directory)
            git(scratch, "init", "--quiet", "--initial-branch=main")
            git(scratch, "config", "user.email", "test@example.com")
            git(scratch, "config", "user.name", "test")
            for relative in expected:
                target = scratch / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(
                    json.dumps({"object_id": self.SENTINEL}, indent=2) + "\n",
                    encoding="utf-8",
                )
            git(scratch, "add", "--all")
            git(scratch, "commit", "--quiet", "-m", "merged, not regenerated")
            # `write` ran but nothing was committed: disk repaired, tip is not.
            for relative in expected:
                (scratch / relative).write_text(
                    json.dumps({"object_id": "a" * 40}, indent=2) + "\n",
                    encoding="utf-8",
                )

            self.assertEqual(
                sentinel_check.sentinel_files(scratch), [],
                "the working tree is clean here -- that is the whole trap",
            )
            self.assertEqual(sentinel_check.sentinel_files(scratch, "HEAD"), expected)
            with contextlib.redirect_stderr(io.StringIO()) as reported:
                self.assertEqual(
                    sentinel_check.main(
                        ["--root", str(scratch), "--rev", "HEAD", "--mode", "report"]
                    ),
                    1,
                )
            self.assertIn("being pushed", reported.getvalue())
            # Control: once the repair is committed the same scan goes quiet, so
            # the reading above is the sentinel and not the scan firing always.
            git(scratch, "add", "--all")
            git(scratch, "commit", "--quiet", "-m", "regenerated")
            self.assertEqual(sentinel_check.sentinel_files(scratch, "HEAD"), [])

    def test_the_push_gate_reads_the_tip(self) -> None:
        """The hook must pass --rev, or the case above is untested in production."""

        hook = (self.root / ".githooks/pre-push").read_text(encoding="utf-8")
        invocation = [
            line for line in hook.splitlines()
            if not line.lstrip().startswith("#")
            and '"$GPU_LEDGER_SENTINEL"' in line
            and "--mode" in line
        ]
        self.assertEqual(len(invocation), 1, "expected exactly one live invocation")
        self.assertIn("--rev HEAD", invocation[0])

    def test_the_driver_and_the_check_agree_on_the_sentinel(self) -> None:
        """Git runs the driver as a bare shell command, so it cannot import the
        checker; the two literals are only in step because this says so. A
        silent disagreement would leave the driver writing something no guard
        looks for, which is the failure this whole design exists to prevent.
        """

        self.assertEqual(sentinel_check.SENTINEL, self.SENTINEL)
        self.assertIn(
            f'"{self.SENTINEL}"', self.driver.read_text(encoding="utf-8")
        )

    def test_both_push_paths_run_the_sentinel_check(self) -> None:
        """gates.sh is run by convention; the hook is run by git.

        Wiring only gates.sh would leave the sentinel reaching CI for anyone
        who did not think to run it -- the twenty-minute roundtrip the driver
        exists to remove.

        Asserting the script *name* appears in each file cannot prove that:
        both surfaces hold the path in a shell variable, so the name stays
        present when the line that runs it is deleted or commented out. This
        finds the assignment, then requires a live command that actually
        executes that variable.
        """

        script = "gpu_ledger_sentinel_check.py"
        for surface in ("tools/scripts/gates.sh", ".githooks/pre-push"):
            text = (self.root / surface).read_text(encoding="utf-8")
            assigned = re.findall(
                rf'^\s*(\w+)="[^"]*{re.escape(script)}"', text, re.MULTILINE
            )
            self.assertEqual(
                len(assigned), 1,
                f"{surface} does not name {script} exactly once",
            )
            variable = assigned[0]
            invocations = [
                line for line in text.splitlines()
                if not line.lstrip().startswith("#")
                and f'"${variable}"' in line
                and "--mode" in line
            ]
            self.assertTrue(
                invocations,
                f"{surface} never runs ${variable}: the gate is registered but dead",
            )


def catalog_module():
    """The handoff validator, loaded the way the generator loads it."""

    provenance.validate_with_catalog(
        provenance.load_handoff(provenance.DEFAULT_HANDOFF), provenance.ROOT
    )
    return provenance._CATALOG_MODULE


class MergeResolution(unittest.TestCase):
    """Prove `resolve` finishes the mechanical repair and declines the rest.

    The sequence it replaces was run by hand on five consecutive sweeps, and the
    branch in it is the part that cannot be defaulted: regeneration always
    rewrites the receipt's self-referential source_commit, so a diff is never by
    itself evidence that an identity moved.
    """

    root = pathlib.Path(__file__).resolve().parents[2]

    def scratch_repository(self) -> pathlib.Path:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        scratch = pathlib.Path(directory.name)
        git(scratch, "init", "--quiet", "--initial-branch=main")
        git(scratch, "config", "user.email", "resolve@example.invalid")
        git(scratch, "config", "user.name", "Resolve Fixture")
        return scratch

    def test_binding_proof_discriminates(self) -> None:
        """A True that cannot be False is not evidence.

        The binding is the claim the identity tiers structurally cannot make:
        every pinned identity can match while the receipt names another ledger
        entirely. So the proof is asserted together with the control that must
        return False on the same inputs.
        """

        ledger = '{"entries": []}\n'
        digest = hashlib.sha256(ledger.encode("utf-8")).hexdigest()
        receipt = json.dumps({"handoff_sha256": digest})

        binds, named, actual = provenance.binding_proof(ledger, receipt)
        self.assertTrue(binds)
        self.assertEqual(named, actual)

        # Negative control: one byte of ledger movement must flip it.
        mutated, _, _ = provenance.binding_proof(ledger + "\n", receipt)
        self.assertFalse(mutated, "the binding check cannot see a changed ledger")
        unreadable, _, _ = provenance.binding_proof(ledger, "not json")
        self.assertFalse(unreadable)

    def test_the_discriminator_separates_churn_from_a_real_move(self) -> None:
        """source_commit alone is churn; anything else is not this tool's call."""

        base = {"schema": "s", "source_commit": "a" * 40, "handoff_sha256": "b" * 64}
        churn = dict(base, source_commit="c" * 40)
        self.assertEqual(
            provenance.changed_receipt_fields(json.dumps(base), json.dumps(churn)),
            {"source_commit"},
        )
        self.assertLessEqual(
            provenance.changed_receipt_fields(json.dumps(base), json.dumps(churn)),
            provenance.RECEIPT_CHURN_FIELDS,
        )

        moved = dict(base, source_commit="c" * 40, handoff_sha256="d" * 64)
        self.assertFalse(
            provenance.changed_receipt_fields(json.dumps(base), json.dumps(moved))
            <= provenance.RECEIPT_CHURN_FIELDS
        )
        self.assertIsNone(provenance.changed_receipt_fields(None, json.dumps(base)))

    def test_the_sentinel_literal_matches_every_copy_of_it(self) -> None:
        """Three modules carry the literal because Git runs the driver bare."""

        driver = (
            self.root / "tools/scripts/gpu_ledger_merge_driver.py"
        ).read_text(encoding="utf-8")
        self.assertIn(f'SENTINEL = "{provenance.LEDGER_SENTINEL}"', driver)
        self.assertEqual(sentinel_check.SENTINEL, provenance.LEDGER_SENTINEL)

    def test_resolve_refuses_while_the_merge_is_uncommitted(self) -> None:
        """Pinning to the commit the write is about to create cannot converge."""

        scratch = self.scratch_repository()
        (scratch / "leaf.txt").write_text("base\n", encoding="utf-8")
        git(scratch, "add", "leaf.txt")
        git(scratch, "commit", "--quiet", "-m", "base")
        self.assertIsNone(provenance.unresolved_merge(scratch))

        git(scratch, "checkout", "--quiet", "-b", "side")
        (scratch / "leaf.txt").write_text("side\n", encoding="utf-8")
        git(scratch, "commit", "--quiet", "-a", "-m", "side")
        git(scratch, "checkout", "--quiet", "main")
        (scratch / "leaf.txt").write_text("main\n", encoding="utf-8")
        git(scratch, "commit", "--quiet", "-a", "-m", "main")
        subprocess.run(
            ["git", "merge", "--no-edit", "side"],
            cwd=scratch, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            check=False, timeout=60,
        )
        blocked = provenance.unresolved_merge(scratch)
        self.assertIsNotNone(blocked, "an in-flight merge was reported as resolved")

    def test_the_discriminator_is_head_not_main(self) -> None:
        """A branch that already re-pinned is not moving; main is the wrong baseline.

        Measured on the real branch: against `origin/main` the ledger differs
        because this branch re-pinned it, so a merge that moved nothing read as
        MOVED and rewrote source_commit for nothing -- a commit claiming a re-pin
        that did not happen, which re-collides on the next sweep. The question
        is "did regeneration change what HEAD committed", and only HEAD answers
        it.
        """

        source = (
            self.root / "tools/scripts/gpu_handoff_provenance.py"
        ).read_text(encoding="utf-8")
        self.assertNotIn("--baseline", source, "resolve still takes a baseline ref")
        self.assertIn('read_revision_text(args.root, "HEAD", ledger_name)', source)
        self.assertNotIn("args.baseline", source)

    def test_the_independent_probe_is_read_before_resolve_writes(self) -> None:
        """A cross-check taken after `resolve` measures resolve's own output.

        `resolve` writes the repaired ledger and receipt into the same two
        files `check` reads. Run afterwards, `check` reports the repair and
        never the state that produced the verdict, so it agrees with any
        verdict at all. On a tree that edits one pinned path without
        regenerating it exits 1 naming two drifts beforehand and 0 naming none
        afterwards -- which is how a correct `moved` came to be asserted
        against `churn` on the required gate. The cross-check is only a
        cross-check while it runs first.
        """

        source = inspect.getsource(self.test_resolve_leaves_the_checked_in_pair_bound)
        probe = source.index('"check", "--json"')
        resolves = source.index('"resolve", "--json"')
        self.assertLess(
            probe,
            resolves,
            "the check probe runs after resolve, so it reads the files resolve "
            "just wrote rather than the state resolve judged",
        )

    def test_resolve_leaves_the_checked_in_pair_bound(self) -> None:
        """End to end on the real ledger: whatever it decides, the pair binds.

        `resolve` writes, so the files are restored unconditionally. The claim
        asserted here is the one a green identity check cannot make.
        """

        handoff = provenance.DEFAULT_HANDOFF
        receipt = provenance.DEFAULT_RECEIPT
        if not handoff.is_file() or not receipt.is_file():
            self.skipTest("the checked-in ledger pair is not present")
        names = [str(path.relative_to(self.root)) for path in (handoff, receipt)]
        # Restore from Git, not from bytes read at test start: a run that
        # inherits an already-dirty pair would otherwise "restore" the dirt and
        # leave a modified checkout behind. Skipping on dirt keeps the restore
        # from discarding somebody's real edit.
        if git(self.root, "status", "--porcelain", "--", *names):
            self.skipTest("the checked-in ledger pair is modified locally")
        self.addCleanup(lambda: git(self.root, "checkout", "--", *names))

        # `check` is the independent answer, so it has to be read BEFORE
        # `resolve` runs. `resolve` WRITES its repair into the same two files
        # `check` reads, so a probe taken afterwards reports the repair and
        # never the state that produced the verdict. Measured on a tree that
        # edits one pinned path without regenerating: check exits 1 naming two
        # drifts before resolve and 0 naming none after, so the later probe
        # called a stale ledger current and demanded `churn` from a correct
        # `moved`.
        probe = io.StringIO()
        with contextlib.redirect_stdout(probe), contextlib.redirect_stderr(probe):
            current = provenance.main(["--root", str(self.root), "check", "--json"])
        # A branch that edited a pinned path leaves the ledger stale, which is
        # an ordinary state and not this suite's to reject. If an assertion
        # below still fires, the reader needs the repair rather than the
        # verdict vocabulary, so carry it into every message.
        remedy = stale_pin_remedy(probe.getvalue()) or None

        buffer = io.StringIO()
        with contextlib.redirect_stdout(buffer), contextlib.redirect_stderr(buffer):
            status = provenance.main(["--root", str(self.root), "resolve", "--json"])
        if status != 0:
            self.skipTest(f"resolve declined on this checkout: {buffer.getvalue()}")

        summary = json.loads(buffer.getvalue())

        # Pin the verdict, do not merely accept one. `check` answers,
        # independently, whether every identity is current AND the receipt binds
        # -- which is exactly the state in which regeneration can change nothing.
        # So when it exits 0 the only admissible verdict is churn, and resolve
        # owes no commit. Accepting a set here lets the discriminator invert
        # without a test noticing.
        if current == 0:
            self.assertEqual(
                summary["verdict"],
                "churn",
                "a ledger check reports current and bound, so nothing moved",
            )
            self.assertFalse(summary["pending"], "churn left a commit pending")
        else:
            self.assertIn(summary["verdict"], {"moved", "rebind"}, remedy)
        self.assertTrue(summary["binds"], "resolve reported success on an unbound pair")
        self.assertFalse(
            summary["binding_control_binds"],
            "the binding control passed, so the proof does not discriminate",
        )
        binds, _, _ = provenance.binding_proof(
            handoff.read_text(encoding="utf-8"), receipt.read_text(encoding="utf-8")
        )
        self.assertTrue(binds, "the files resolve left on disk do not bind")
        if summary["verdict"] == "churn":
            # The whole point of the churn verdict: no commit is owed, because
            # rewriting source_commit alone claims a re-pin that did not happen.
            self.assertEqual(
                git(self.root, "status", "--porcelain", "--", *names),
                "",
                "a churn resolution dirtied the checkout",
            )


class DriverRegistration(unittest.TestCase):
    """`.gitattributes` names a driver; only local config can answer for it.

    Git does not error on a `merge=<name>` it cannot resolve -- it falls back to
    the ordinary text merge, in silence. So the repo can ship working merge
    automation that is inert in a checkout bootstrapped before it landed, and
    nothing says so. Measured on this machine: the attribute present, the driver
    absent, the collision back on every sweep.
    """

    root = pathlib.Path(__file__).resolve().parents[2]

    def scratch_repository(self) -> pathlib.Path:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        scratch = pathlib.Path(directory.name)
        git(scratch, "init", "--quiet", "--initial-branch=main")
        shutil.copyfile(self.root / ".gitattributes", scratch / ".gitattributes")
        return scratch

    def test_the_routed_paths_are_read_from_gitattributes(self) -> None:
        """Hardcoding them would keep passing after the routing was removed."""

        routed = sentinel_check.declared_driver_paths(self.root)
        self.assertEqual(
            sorted(routed),
            sorted(
                [
                    str(provenance.DEFAULT_HANDOFF.relative_to(self.root)),
                    str(provenance.DEFAULT_RECEIPT.relative_to(self.root)),
                ]
            ),
        )
        empty = self.scratch_repository()
        (empty / ".gitattributes").write_text("*.txt text\n", encoding="utf-8")
        self.assertEqual(sentinel_check.declared_driver_paths(empty), [])

    def test_an_unregistered_driver_fails_the_gate_and_says_how_to_fix_it(self) -> None:
        """The condition, and the control that must not fire on the repaired state."""

        scratch = self.scratch_repository()
        self.assertFalse(sentinel_check.driver_is_registered(scratch))

        buffer = io.StringIO()
        with contextlib.redirect_stderr(buffer):
            status = sentinel_check.main(["--root", str(scratch), "--mode=report"])
        self.assertEqual(status, 1)
        message = buffer.getvalue()
        self.assertIn("no driver registered", message)
        self.assertIn(sentinel_check.INSTALLER, message)

        # Positive control: register it and the same call must go quiet.
        git(
            scratch, "config", "merge.pulp-gpu-ledger.driver",
            "tools/scripts/gpu_ledger_merge_driver.py %O %A %B",
        )
        self.assertTrue(sentinel_check.driver_is_registered(scratch))
        quiet = io.StringIO()
        with contextlib.redirect_stderr(quiet):
            self.assertEqual(
                sentinel_check.main(["--root", str(scratch), "--mode=report"]), 0
            )
        self.assertEqual(quiet.getvalue(), "")

    def test_hint_mode_and_the_opt_out_never_block(self) -> None:
        """Advisory callers must stay advisory, and the scan stay reachable alone."""

        scratch = self.scratch_repository()
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(
                sentinel_check.main(["--root", str(scratch), "--mode=hint"]), 0
            )
            self.assertEqual(
                sentinel_check.main(
                    ["--root", str(scratch), "--mode=report", "--skip-registration"]
                ),
                0,
            )


if __name__ == "__main__":
    unittest.main()

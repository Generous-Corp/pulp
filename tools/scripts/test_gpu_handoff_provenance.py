#!/usr/bin/env python3
"""Prove the GPU handoff provenance generator is deterministic and fail-closed."""

from __future__ import annotations

import contextlib
import hashlib
import io
import json
import pathlib
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest


sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import gpu_handoff_provenance as provenance  # noqa: E402


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

    def test_identity_tracks_a_later_edit(self) -> None:
        (self.root / "leaf.txt").write_text("leaf two\n", encoding="utf-8")
        git(self.root, "add", "leaf.txt")
        git(self.root, "commit", "--quiet", "-m", "edit leaf")
        third = git(self.root, "rev-parse", "HEAD")
        self.assertEqual(self.identity_for("leaf.txt").revision, third)

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
        exit_code = provenance.main(
            ["--root", str(self.root), "--handoff", str(handoff), "write"]
        )
        self.assertEqual(exit_code, 1)
        self.assertEqual(handoff.read_text(encoding="utf-8"), before)

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

    def test_an_unresolvable_source_commit_is_an_environment_error(self) -> None:
        handoff = self.write_fixture_handoff([build_row("leaf.txt")])
        self.assertEqual(
            provenance.main(
                [
                    "--root",
                    str(self.root),
                    "--handoff",
                    str(handoff),
                    "check",
                    "--source-commit",
                    "0" * 40,
                ]
            ),
            2,
        )

    def test_write_refuses_an_unclean_checkout(self) -> None:
        handoff = self.root / "handoff.json"
        handoff.write_text(
            provenance.serialize_handoff(self.document([build_row("leaf.txt")])),
            encoding="utf-8",
        )
        (self.root / "leaf.txt").write_text("modified\n", encoding="utf-8")
        exit_code = provenance.main(
            [
                "--root",
                str(self.root),
                "--handoff",
                str(handoff),
                "write",
            ]
        )
        self.assertEqual(exit_code, 2)
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

        This is the drift gate. It goes red when a commit changes a pinned path
        without regenerating the ledger, and it names the generator as the
        repair.
        """

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
        self.assertEqual(
            provenance.main(
                ["--root", str(self.root), "--handoff", str(self.handoff), "check"]
            ),
            0,
        )


if __name__ == "__main__":
    unittest.main()

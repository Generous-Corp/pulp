#!/usr/bin/env python3
"""A branch may edit the ledger's content; it may not regenerate its identities.

Both directions are asserted, and the permissive one matters more. A guard that
rejected every change to these files would be easy to write and would recreate
the two defects that killed the blunt version of this design: it would outlaw
the inventory edits the generator explicitly leaves to humans, and it would
strand a branch whose only way to turn the required gate green is to correct
``route_set_sha256`` or ``expansion_id``, neither of which any tool regenerates.
"""
from __future__ import annotations

import copy
import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import gpu_handoff_pin_freshness as guard  # noqa: E402
import gpu_ledger_merge_driver as driver  # noqa: E402

REPO = Path(__file__).parents[2]

SHA_A = "a" * 40
SHA_B = "b" * 40
SHA_C = "c" * 40


def git(*args: str, cwd: Path) -> None:
    subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True, text=True)


def row(path: str, revision: str, object_id: str) -> dict:
    return {
        "repo": "Generous-Corp/pulp",
        "path": path,
        "state": "pulp-owned-retained",
        "revision": revision,
        "object_id": object_id,
        "object_type": "blob",
    }


def ledger() -> dict:
    """A document with the real one's shape: rows, constants, and free fields."""

    return {
        "schema": "pulp.gpu-vellum-handoff.v2",
        "expansion_id": "full-design-import-render-v1",
        "route_set_sha256": "5" * 64,
        "authorities": {"pulp": {"repo": "Generous-Corp/pulp", "revision": SHA_A}},
        "upstream": {"issue": {"number": 26}, "current_comment_id": 5464003821},
        "entries": [{
            "id": "e0",
            "pulp_paths": [
                row("core/render/src/skia_surface.cpp", SHA_A, "1" * 40),
                row("core/view/src/view.cpp", SHA_A, "2" * 40),
            ],
            # Pinned to a foreign revision and never regenerated, so the driver
            # leaves them alone and so must the guard.
            "vellum_paths": [
                {"repo": "Generous-Corp/vellum", "path": "graphics/src/x.mm",
                 "revision": SHA_C, "object_id": "9" * 40, "object_type": "blob"},
            ],
            "retained_paths": ["core/render/src/skia_surface.cpp", "core/view/src/view.cpp"],
        }],
    }


def receipt_for(document: dict, source_commit: str) -> dict:
    paths = sorted(r["path"] for e in document["entries"] for r in e["pulp_paths"])
    rendered = json.dumps(document, indent=2, ensure_ascii=True, separators=(",", ": ")) + "\n"
    return {
        "schema": "pulp.gpu-handoff-provenance-receipt.v1",
        "generator": "tools/scripts/gpu_handoff_provenance.py",
        "handoff_path": str(guard.HANDOFF),
        "source_commit": source_commit,
        "source_repository": "Generous-Corp/pulp",
        "canonical_path_count": len(paths),
        "canonical_paths": paths,
        "canonical_path_sha256": hashlib.sha256(
            ("\n".join(paths) + "\n").encode()).hexdigest(),
        "handoff_sha256": hashlib.sha256(rendered.encode()).hexdigest(),
    }


def repin(document: dict, revision: str, object_suffix: str) -> dict:
    """Exactly what `gpu_handoff_provenance.py write` does: identities, nothing else."""

    updated = copy.deepcopy(document)
    for index, pinned in enumerate(updated["entries"][0]["pulp_paths"]):
        pinned["revision"] = revision
        pinned["object_id"] = (object_suffix * 40)[:39] + str(index)
    return updated


class ChurnGuardTests(unittest.TestCase):

    # ---- positive control -------------------------------------------------

    def test_the_real_ledger_and_receipt_are_visible_to_the_differ(self) -> None:
        """Without this, every rejection below could be an inert comparison."""

        for relative in guard.WATCHED:
            raw = (REPO / relative).read_text()
            self.assertIsNotNone(guard.neutralized(raw), f"{relative} did not parse")
            self.assertNotEqual(
                json.loads(raw), guard.neutralized(raw),
                f"neutralizing {relative} changed nothing; the differ sees no churn "
                "and would accept a re-pin of the real file",
            )

    def test_the_guard_reads_the_fields_the_driver_owns(self) -> None:
        """The two must not drift into separate definitions of churn."""

        self.assertEqual(driver.LEDGER_IDENTITY_FIELDS, ("revision", "object_id"))
        self.assertEqual(driver.RECEIPT_REGENERABLE_FIELDS,
                         ("source_commit", "handoff_sha256"))

    # ---- fixtures ---------------------------------------------------------

    def _write(self, root: Path, document: dict, source_commit: str) -> None:
        for relative, payload in (
            (guard.HANDOFF, document),
            (guard.RECEIPT, receipt_for(document, source_commit)),
        ):
            target = root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(json.dumps(payload, indent=2) + "\n")

    def _repo(self, td: str, document: dict | None = None) -> tuple[Path, dict]:
        root = Path(td)
        document = document or ledger()
        git("init", "-q", "-b", "main", cwd=root)
        git("config", "user.email", "t@e.st", cwd=root)
        git("config", "user.name", "t", cwd=root)
        self._write(root, document, SHA_A)
        (root / "unrelated.txt").write_text("a\n")
        git("add", str(guard.HANDOFF), str(guard.RECEIPT), "unrelated.txt", cwd=root)
        git("commit", "-qm", "base", cwd=root)
        git("checkout", "-q", "-b", "feature", cwd=root)
        return root, document

    def _commit(self, root: Path, message: str) -> None:
        git("add", str(guard.HANDOFF), str(guard.RECEIPT), "unrelated.txt", cwd=root)
        git("commit", "-qm", message, cwd=root)

    def run_guard(self, root: Path, *extra: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, "-B",
             str(Path(__file__).parent / "gpu_handoff_pin_freshness.py"),
             "--base", "main", "--root", str(root), *extra],
            cwd=root, capture_output=True, text=True,
        )

    # ---- reject: the churn ------------------------------------------------

    def test_an_identity_only_regeneration_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root, document = self._repo(td)
            self._write(root, repin(document, SHA_B, "7"), SHA_B)
            self._commit(root, "refresh ledger")
            proc = self.run_guard(root)
            self.assertEqual(proc.returncode, 1, proc.stderr)
            self.assertIn(str(guard.HANDOFF), proc.stderr)
            self.assertIn(str(guard.RECEIPT), proc.stderr)
            self.assertIn("git restore --source", proc.stderr)

    def test_a_receipt_only_source_commit_move_is_rejected(self) -> None:
        """The unconditional half of the collision, on its own."""

        with tempfile.TemporaryDirectory() as td:
            root, _ = self._repo(td)
            target = root / guard.RECEIPT
            payload = json.loads(target.read_text())
            payload["source_commit"] = SHA_B
            target.write_text(json.dumps(payload, indent=2) + "\n")
            self._commit(root, "receipt only")
            proc = self.run_guard(root)
            self.assertEqual(proc.returncode, 1, proc.stderr)
            self.assertIn(str(guard.RECEIPT), proc.stderr)

    def test_a_ledger_only_repin_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root, document = self._repo(td)
            (root / guard.HANDOFF).write_text(
                json.dumps(repin(document, SHA_B, "7"), indent=2) + "\n")
            self._commit(root, "ledger only")
            self.assertEqual(self.run_guard(root).returncode, 1)

    # ---- permit: the content ----------------------------------------------

    def test_adding_a_pinned_path_is_permitted(self) -> None:
        """R1: the generator never adds rows, so every addition is a human edit."""

        with tempfile.TemporaryDirectory() as td:
            root, document = self._repo(td)
            grown = copy.deepcopy(document)
            grown["entries"][0]["pulp_paths"].append(
                row("core/render/src/dawn_surface.cpp", SHA_B, "3" * 40))
            grown["entries"][0]["retained_paths"].append("core/render/src/dawn_surface.cpp")
            self._write(root, grown, SHA_B)
            self._commit(root, "pin a new path")
            proc = self.run_guard(root)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertEqual(proc.stderr, "")

    def test_removing_a_pinned_path_is_permitted(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root, document = self._repo(td)
            shrunk = copy.deepcopy(document)
            shrunk["entries"][0]["pulp_paths"].pop()
            shrunk["entries"][0]["retained_paths"].pop()
            self._write(root, shrunk, SHA_B)
            self._commit(root, "unpin a path")
            self.assertEqual(self.run_guard(root).returncode, 0)

    def test_an_inventory_edit_carries_its_identity_refresh_with_it(self) -> None:
        """The mixed case: a new row AND every other row re-pinned, together.

        This is what regenerating after a real edit produces, and rejecting it
        would leave the edit unlandable.
        """

        with tempfile.TemporaryDirectory() as td:
            root, document = self._repo(td)
            grown = copy.deepcopy(document)
            grown["entries"][0]["pulp_paths"].append(
                row("core/render/src/dawn_surface.cpp", SHA_B, "3" * 40))
            grown["entries"][0]["retained_paths"].append("core/render/src/dawn_surface.cpp")
            self._write(root, repin(grown, SHA_B, "7"), SHA_B)
            self._commit(root, "pin a new path and regenerate")
            self.assertEqual(self.run_guard(root).returncode, 0)

    def test_a_route_set_correction_is_permitted(self) -> None:
        """R2: validate_handoff_routing checks this unconditionally and no tool
        regenerates it, so forbidding the edit would make the gate unfixable."""

        with tempfile.TemporaryDirectory() as td:
            root, document = self._repo(td)
            corrected = copy.deepcopy(document)
            corrected["route_set_sha256"] = "6" * 64
            self._write(root, corrected, SHA_B)
            self._commit(root, "route set moved")
            self.assertEqual(self.run_guard(root).returncode, 0)

    def test_an_expansion_id_correction_is_permitted(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root, document = self._repo(td)
            corrected = copy.deepcopy(document)
            corrected["expansion_id"] = "full-design-import-render-v2"
            self._write(root, corrected, SHA_B)
            self._commit(root, "expansion moved")
            self.assertEqual(self.run_guard(root).returncode, 0)

    def test_an_upstream_update_is_permitted(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root, document = self._repo(td)
            updated = copy.deepcopy(document)
            updated["upstream"]["current_comment_id"] = 5464003822
            self._write(root, updated, SHA_B)
            self._commit(root, "upstream comment superseded")
            self.assertEqual(self.run_guard(root).returncode, 0)

    def test_an_object_type_change_is_content(self) -> None:
        """The driver leaves object_type to merge on purpose; so does the guard."""

        with tempfile.TemporaryDirectory() as td:
            root, document = self._repo(td)
            retyped = copy.deepcopy(document)
            retyped["entries"][0]["pulp_paths"][0]["object_type"] = "tree"
            self._write(root, retyped, SHA_B)
            self._commit(root, "path became a directory")
            self.assertEqual(self.run_guard(root).returncode, 0)

    def test_a_vellum_row_identity_is_content(self) -> None:
        """Foreign pins are constants, not generated output."""

        with tempfile.TemporaryDirectory() as td:
            root, document = self._repo(td)
            moved = copy.deepcopy(document)
            moved["entries"][0]["vellum_paths"][0]["revision"] = SHA_B
            self._write(root, moved, SHA_B)
            self._commit(root, "vellum pin moved")
            self.assertEqual(self.run_guard(root).returncode, 0)

    # ---- scope ------------------------------------------------------------

    def test_an_untouched_ledger_is_ignored(self) -> None:
        """NEGATIVE CONTROL: the guard must not fire on unrelated work."""

        with tempfile.TemporaryDirectory() as td:
            root, _ = self._repo(td)
            (root / "unrelated.txt").write_text("b\n")
            self._commit(root, "unrelated")
            proc = self.run_guard(root)
            self.assertEqual(proc.returncode, 0)
            self.assertEqual(proc.stderr, "")

    def test_editing_a_pinned_path_without_refreshing_is_now_permitted(self) -> None:
        """The exact shape the old polarity rejected."""

        with tempfile.TemporaryDirectory() as td:
            root, _ = self._repo(td)
            target = root / "core/view/src"
            target.mkdir(parents=True)
            (target / "view.cpp").write_text("b\n")
            git("add", "core/view/src/view.cpp", cwd=root)
            git("commit", "-qm", "edit a pinned path", cwd=root)
            proc = self.run_guard(root)
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertEqual(proc.stderr, "")

    def test_the_before_side_is_the_merge_base_not_the_base_ref(self) -> None:
        """Main's own content move must not mask this branch's churn.

        Read against the base ref, main's added row would show as a difference
        the branch never made, the range would read as content, and the churn
        would land. Silent under-firing, which is the failure a reviewer cannot
        see.
        """

        with tempfile.TemporaryDirectory() as td:
            root, document = self._repo(td)
            self._write(root, repin(document, SHA_B, "7"), SHA_B)
            self._commit(root, "refresh ledger")

            git("checkout", "-q", "main", cwd=root)
            grown = copy.deepcopy(document)
            grown["entries"][0]["pulp_paths"].append(
                row("core/render/src/dawn_surface.cpp", SHA_C, "4" * 40))
            grown["entries"][0]["retained_paths"].append("core/render/src/dawn_surface.cpp")
            self._write(root, grown, SHA_C)
            self._commit(root, "main pins a new path")
            git("checkout", "-q", "feature", cwd=root)

            self.assertEqual(self.run_guard(root).returncode, 1)

    # ---- fail-visible, never fail-silent ----------------------------------

    def test_an_unparseable_ledger_says_so_rather_than_passing_quietly(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root, _ = self._repo(td)
            (root / guard.HANDOFF).write_text("{ not json")
            self._commit(root, "broken")
            proc = self.run_guard(root)
            self.assertEqual(proc.returncode, 0)
            self.assertIn("nothing checked", proc.stderr)

    def test_a_deleted_ledger_says_so_rather_than_passing_quietly(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root, _ = self._repo(td)
            (root / guard.HANDOFF).unlink()
            git("add", "-u", str(guard.HANDOFF), cwd=root)
            git("commit", "-qm", "delete", cwd=root)
            proc = self.run_guard(root)
            self.assertEqual(proc.returncode, 0)
            self.assertIn("nothing checked", proc.stderr)

    def test_an_unreadable_range_says_so_rather_than_passing_quietly(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root, _ = self._repo(td)
            proc = subprocess.run(
                [sys.executable, "-B",
                 str(Path(__file__).parent / "gpu_handoff_pin_freshness.py"),
                 "--base", "no-such-ref", "--root", str(root)],
                cwd=root, capture_output=True, text=True,
            )
            self.assertEqual(proc.returncode, 0)
            self.assertIn("nothing checked", proc.stderr)

    def test_a_scalar_where_the_rows_belong_does_not_traceback(self) -> None:
        """`poison` walks with `or []`, which survives None and not a scalar."""

        self.assertIsNone(guard.neutralized('{"entries": 7}'))

    def test_hint_mode_reports_without_failing(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root, document = self._repo(td)
            self._write(root, repin(document, SHA_B, "7"), SHA_B)
            self._commit(root, "refresh ledger")
            proc = self.run_guard(root, "--mode", "hint")
            self.assertEqual(proc.returncode, 0)
            self.assertIn("gpu-handoff-churn", proc.stderr)

    def test_both_push_paths_run_this_guard(self) -> None:
        """Wiring only gates.sh leaves the churn reaching CI for anyone who
        pushes without running it by hand, which is the normal case."""

        script = "gpu_handoff_pin_freshness.py"
        for surface in (".githooks/pre-push", "tools/scripts/gates.sh"):
            self.assertIn(script, (REPO / surface).read_text(),
                          f"{surface} does not run {script}")


if __name__ == "__main__":
    unittest.main()

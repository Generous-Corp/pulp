#!/usr/bin/env python3
"""Counter-derivation contract tests.

The rule these pin is asymmetric and easy to get backwards: a counter must
advance when its own material moved, and must NOT advance when it did not.
Over-bumping is not the safe direction — it fails the opposite evolution rule,
`... changed without a manifest change`.
"""
from __future__ import annotations

import pathlib
import subprocess
import sys

import agent_capability_rederive as rederive


ROOT = pathlib.Path(__file__).resolve().parents[2]

BASE_MANIFEST = {"manifest_revision": 28, "capabilities": [{"key": "a"}]}
BASE_SURFACE = {"inventory_version": 45, "headers": [{"include": "x.hpp"}]}


def _manifest(revision: int, capabilities: list[dict[str, str]] | None = None) -> dict:
    return {
        "manifest_revision": revision,
        "capabilities": capabilities if capabilities is not None else [{"key": "a"}],
    }


def _surface(version: int, headers: list[dict[str, str]] | None = None) -> dict:
    return {
        "inventory_version": version,
        "headers": headers if headers is not None else [{"include": "x.hpp"}],
    }


def exercise_no_change() -> int:
    """Identical material must hold both counters still."""
    revision, inventory = rederive.derive(
        _manifest(28), _surface(45), BASE_MANIFEST, BASE_SURFACE
    )
    assert (revision, inventory) == (28, 45), (revision, inventory)

    # A tree that already carries a HIGHER counter with identical material is
    # still derived back down: the counter it holds is not evidence, the
    # material is. This is the stale-reservation case the tool exists for.
    revision, inventory = rederive.derive(
        _manifest(29), _surface(46), BASE_MANIFEST, BASE_SURFACE
    )
    assert (revision, inventory) == (28, 45), (revision, inventory)
    return 2


def exercise_manifest_only() -> int:
    """Manifest material moved; the surface did not."""
    revision, inventory = rederive.derive(
        _manifest(28, [{"key": "a"}, {"key": "b"}]),
        _surface(45),
        BASE_MANIFEST,
        BASE_SURFACE,
    )
    assert revision == 29, revision
    assert inventory == 45, f"surface must not move when its material is identical: {inventory}"
    return 1


def exercise_surface_only() -> int:
    """Surface material moved; the manifest did not."""
    revision, inventory = rederive.derive(
        _manifest(28),
        _surface(45, [{"include": "x.hpp"}, {"include": "y.hpp"}]),
        BASE_MANIFEST,
        BASE_SURFACE,
    )
    assert inventory == 46, inventory
    assert revision == 28, f"manifest must not move when its material is identical: {revision}"
    return 1


def exercise_both() -> int:
    revision, inventory = rederive.derive(
        _manifest(28, [{"key": "a"}, {"key": "b"}]),
        _surface(45, [{"include": "x.hpp"}, {"include": "y.hpp"}]),
        BASE_MANIFEST,
        BASE_SURFACE,
    )
    assert (revision, inventory) == (29, 46), (revision, inventory)
    return 1


def exercise_counter_is_not_material() -> int:
    """The counter itself must be excluded from the comparison.

    Without the pop, a tree holding 29 against a base of 28 would look like a
    material change and bump forever on every re-run.
    """
    first = rederive.derive(_manifest(28), _surface(45), BASE_MANIFEST, BASE_SURFACE)
    second = rederive.derive(
        _manifest(first[0]), _surface(first[1]), BASE_MANIFEST, BASE_SURFACE
    )
    assert first == second == (28, 45), (first, second)
    return 1


def exercise_rejects_underivable_base() -> int:
    """A base we cannot read must raise, never guess a number."""
    checks = 0
    for base_manifest, base_surface, needle in (
        (None, BASE_SURFACE, "initial bootstrap"),
        (BASE_MANIFEST, None, "initial bootstrap"),
        ({"manifest_revision": "28"}, BASE_SURFACE, "manifest_revision is not an integer"),
        (BASE_MANIFEST, {"inventory_version": None}, "inventory_version is not an integer"),
        # bool is an int subclass in Python; True must not read as revision 1.
        ({"manifest_revision": True}, BASE_SURFACE, "manifest_revision is not an integer"),
    ):
        try:
            rederive.derive(_manifest(28), _surface(45), base_manifest, base_surface)
        except rederive.DeriveError as error:
            assert needle in str(error), f"expected {needle!r} in {error!r}"
            checks += 1
        else:  # pragma: no cover - a miss is a test failure, not a branch
            raise AssertionError(f"underivable base unexpectedly accepted: {needle}")
    return checks


def exercise_rewrite_round_trip() -> int:
    """Rewriting must move exactly the two constants and stay idempotent."""
    path = ROOT / rederive.SCRIPT
    original = path.read_text(encoding="utf-8")
    try:
        moved = rederive.rewrite_constants(ROOT, 1234, 5678)
        assert moved is True
        text = path.read_text(encoding="utf-8")
        assert "MANIFEST_REVISION = 1234" in text
        assert "SURFACE_INVENTORY_VERSION = 5678" in text
        # Same values again must report "nothing moved" rather than rewrite.
        assert rederive.rewrite_constants(ROOT, 1234, 5678) is False
        # Everything except those two lines must be byte-identical.
        before = [
            line for line in original.splitlines()
            if not line.startswith(("MANIFEST_REVISION =", "SURFACE_INVENTORY_VERSION ="))
        ]
        after = [
            line for line in text.splitlines()
            if not line.startswith(("MANIFEST_REVISION =", "SURFACE_INVENTORY_VERSION ="))
        ]
        assert before == after, "rewrite touched lines other than the two constants"
        return 4
    finally:
        path.write_text(original, encoding="utf-8")


def exercise_refuses_mid_merge() -> int:
    """An uncommitted merge must refuse, not derive from the merge base.

    This is the case the tool is most often reached for and the one where the
    resolver quietly answers a different question: until the merge commits, the
    incoming tip is not an ancestor of HEAD, so resolution steps back to the
    merge base and the derived counter is stale but plausible.
    """
    git_dir = subprocess.run(
        ["git", "rev-parse", "--git-dir"],
        cwd=ROOT, capture_output=True, text=True,
    ).stdout.strip()
    if not git_dir:  # pragma: no cover - not a work tree
        return 0
    path = pathlib.Path(git_dir)
    if not path.is_absolute():
        path = ROOT / path
    merge_head = path / "MERGE_HEAD"
    if merge_head.exists():  # pragma: no cover - never clobber a real merge
        raise AssertionError(
            "a merge is genuinely in progress; refusing to run this test"
        )
    try:
        merge_head.write_text("0" * 39 + "1\n", encoding="utf-8")
        completed = subprocess.run(
            [sys.executable, str(ROOT / "tools/scripts/agent_capability_rederive.py"),
             "--print"],
            cwd=ROOT, capture_output=True, text=True,
        )
        assert completed.returncode != 0, completed.stdout
        assert "a merge is in progress" in completed.stderr, completed.stderr
        # The message must say what to DO, or it is just a different stall.
        assert "Commit the merge first" in completed.stderr, completed.stderr
    finally:
        merge_head.unlink(missing_ok=True)
    assert not merge_head.exists(), "test left a stray MERGE_HEAD behind"
    return 3


def main() -> int:
    checks = 0
    checks += exercise_refuses_mid_merge()
    checks += exercise_no_change()
    checks += exercise_manifest_only()
    checks += exercise_surface_only()
    checks += exercise_both()
    checks += exercise_counter_is_not_material()
    checks += exercise_rejects_underivable_base()
    checks += exercise_rewrite_round_trip()
    print(f"agent-capability-rederive: {checks} derivation contract checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

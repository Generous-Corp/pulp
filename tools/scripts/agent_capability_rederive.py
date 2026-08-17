#!/usr/bin/env python3
"""Re-derive the capability counters from the current protected base.

`MANIFEST_REVISION` and `SURFACE_INVENTORY_VERSION` are reserved when a
transaction is authored, but the protected base keeps moving: another
capability transaction can land while yours waits in the merge queue, take the
numbers you reserved, and leave your branch failing `manifest changed without a
manifest_revision increase` with a conflict on exactly those two constant
lines.

The recorded fix is to re-read both counters from the current base and
regenerate rather than resolving that conflict by hand. This performs that,
deterministically:

  1. resolve the protected tip (the same resolution `--check` uses),
  2. read its `manifest_revision` / `inventory_version`,
  3. compare this tree's generated material against the base's, counter
     excluded, so each counter moves only when its own material actually
     changed — bumping one whose material is identical fails the opposite
     rule, `... changed without a manifest change`,
  4. rewrite the two constants and hand off to `--write`.

Only the counters are decided here. Every contract, digest, and evolution rule
is still enforced by `--write` and `--check`, which this deliberately does not
reimplement.
"""

from __future__ import annotations

import argparse
import copy
import pathlib
import re
import subprocess
import sys
from typing import Any

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

import agent_capability_history as history  # noqa: E402
import agent_capability_manifest as manifest  # noqa: E402
import agent_capability_surface as surface  # noqa: E402

MANIFEST_CONSTANT = "MANIFEST_REVISION"
SURFACE_CONSTANT = "SURFACE_INVENTORY_VERSION"
SCRIPT = pathlib.Path("tools/scripts/agent_capability_manifest.py")


class DeriveError(RuntimeError):
    """A counter could not be derived; the caller must not guess one."""


def _material(document: Any, counter_key: str) -> Any:
    """The document minus its counter — what the evolution rules compare."""
    if not isinstance(document, dict):
        return None
    stripped = copy.deepcopy(document)
    stripped.pop(counter_key, None)
    return stripped


def derive(
    current_manifest: dict[str, Any],
    current_surface: dict[str, Any],
    base_manifest: Any,
    base_surface: Any,
) -> tuple[int, int]:
    """Return the counters this tree should carry on top of its base.

    A counter advances only when its own material moved. Advancing one whose
    material is unchanged is not a harmless over-bump: it trips
    `... changed without a manifest change`.
    """
    if not isinstance(base_manifest, dict) or not isinstance(base_surface, dict):
        raise DeriveError(
            "protected base has no capability artifacts to derive from; "
            "this is the initial bootstrap and must be authored explicitly"
        )

    base_revision = base_manifest.get("manifest_revision")
    base_inventory = base_surface.get("inventory_version")
    if not isinstance(base_revision, int) or isinstance(base_revision, bool):
        raise DeriveError("protected base manifest_revision is not an integer")
    if not isinstance(base_inventory, int) or isinstance(base_inventory, bool):
        raise DeriveError("protected base inventory_version is not an integer")

    manifest_moved = _material(current_manifest, "manifest_revision") != _material(
        base_manifest, "manifest_revision"
    )
    surface_moved = _material(current_surface, "inventory_version") != _material(
        base_surface, "inventory_version"
    )

    revision = base_revision + 1 if manifest_moved else base_revision
    inventory = base_inventory + 1 if surface_moved else base_inventory
    return revision, inventory


def rewrite_constants(root: pathlib.Path, revision: int, inventory: int) -> bool:
    """Point the two constants at the derived values. True when bytes moved."""
    path = root / SCRIPT
    original = path.read_text(encoding="utf-8")
    updated = original
    for name, value in ((MANIFEST_CONSTANT, revision), (SURFACE_CONSTANT, inventory)):
        pattern = re.compile(rf"^{name} = \d+$", re.MULTILINE)
        if not pattern.search(updated):
            raise DeriveError(f"could not locate `{name} = <int>` in {SCRIPT}")
        updated = pattern.sub(f"{name} = {value}", updated)
    if updated == original:
        return False
    path.write_text(updated, encoding="utf-8")
    return True


def _resolve_base(root: pathlib.Path) -> tuple[Any, Any]:
    """Read the protected tip's two snapshots, or explain why we cannot."""
    inside = history._git_output(root, ["rev-parse", "--is-inside-work-tree"])
    if inside != "true":
        raise DeriveError(
            "not inside a git work tree; a counter cannot be derived without "
            "an addressable protected base"
        )
    base_ref = history.os.environ.get("PULP_AGENT_CAPABILITY_BASE_REF", "origin/main")
    tip = history._resolve_protected_tip(root, base_ref)
    if tip is None:
        raise DeriveError(
            f"could not resolve the protected capability base {base_ref!r}; "
            "set PULP_AGENT_CAPABILITY_BASE_REF to the CI base ref"
        )
    return (
        history._git_json(root, tip, history.SNAPSHOT),
        history._git_json(root, tip, surface.SURFACE_SNAPSHOT),
    )


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--print",
        action="store_true",
        help="report the derived counters without editing anything",
    )
    parser.add_argument(
        "--no-write",
        action="store_true",
        help="rewrite the constants but skip the --write regeneration",
    )
    args = parser.parse_args(argv)

    root = pathlib.Path(__file__).resolve().parents[2]
    try:
        base_manifest, base_surface = _resolve_base(root)
        current_surface, surface_problems = manifest.build_surface(root)
        if surface_problems:
            raise DeriveError(
                "the surface has unresolved problems, so there is nothing "
                "stable to derive a counter from. Counters are decided LAST: "
                "fix these first (a changed header needs its fingerprint "
                "refreshed in the owning catalog), then re-run.\n  - "
                + "\n  - ".join(surface_problems)
            )
        try:
            current_manifest = manifest.document(root)
        except RuntimeError as error:
            raise DeriveError(
                f"the manifest does not build, so no counter can be derived "
                f"from it: {error}"
            ) from error
        revision, inventory = derive(
            current_manifest, current_surface, base_manifest, base_surface
        )
    except DeriveError as error:
        print(f"agent-capability-rederive: {error}", file=sys.stderr)
        return 1

    held_revision = current_manifest.get("manifest_revision")
    held_inventory = current_surface.get("inventory_version")
    print(f"  protected base   revision {base_manifest['manifest_revision']}"
          f"  inventory {base_surface['inventory_version']}")
    print(f"  this tree holds  revision {held_revision}  inventory {held_inventory}")
    print(f"  derived          revision {revision}  inventory {inventory}")

    if args.print:
        return 0

    changed = rewrite_constants(root, revision, inventory)
    if not changed:
        print("  constants already correct for this base")
    if args.no_write:
        return 0

    completed = subprocess.run(
        [sys.executable, str(root / SCRIPT), "--write"], cwd=root
    )
    return completed.returncode


if __name__ == "__main__":  # pragma: no cover - CLI entry point
    raise SystemExit(main(sys.argv[1:]))

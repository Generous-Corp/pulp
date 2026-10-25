#!/usr/bin/env python3
"""Refresh the release-derived files on the SDK release PR's branch.

release-please writes the version numbers into its release PR. Three other
files used to ride in the version-at-land bump commit, and release-please has
no way to produce them:

* `CHANGELOG.md`, rendered by `shipyard changelog regenerate` from the tag
  graph. Shipyard stays its only writer: both release-please configurations set
  `skip-changelog`, so this step is the one place the file is written.
* the GPU/Vellum handoff ledger and its receipt
  (`gpu_handoff_provenance.py write --receipt`), which ordinary PRs must not
  re-pin (gpu_handoff_pin_freshness.py).
* `docs/migrations/unreleased/*.md`, folded into `docs/migrations/v<sdk>.md`
  for the SDK version the release PR assigns, which `pulp upgrade --notes`
  reads.

This reuses the bump's own regenerators (`version_at_land._refresh_derived`
and `_fold_migration_notes`) against the version release-please wrote, so it
does no version arithmetic. It leaves out the plugin disposition, which the
plugin release PR updates through release-please `extra-files`. It edits the
working tree only and prints the changed paths, one per line; the workflow
commits them with the `Release-Derived-Refresh:` marker. Like the bump, every
step is best-effort: a failure leaves its file stale and names the cause.

    python3 tools/scripts/release_derived_refresh.py --repo .
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

from version_at_land import (  # noqa: E402
    MIGRATION_NOTES_SURFACE,
    Assignment,
    _fold_migration_notes,
    _refresh_derived,
)
from version_bump_surfaces import load_config, read_version  # noqa: E402

# Owned by the plugin release PR's `extra-files`, not by this refresh.
PLUGIN_DISPOSITION = "docs/status/pulp-tooling-disposition.json"


def refresh(repo: Path, config_path: Path) -> list[str]:
    config = load_config(config_path)
    surface = next(s for s in config.surfaces if s.name == MIGRATION_NOTES_SURFACE)
    version = read_version(repo, surface.version_files[0])
    if not version:
        print(
            f"release-derived-refresh: cannot read the {surface.name} version "
            f"from {surface.version_files[0].path}; nothing refreshed",
            file=sys.stderr,
        )
        return []
    plan = [Assignment(surface=surface.name, level="none", current=version, assigned=version)]
    edited = _fold_migration_notes(repo, plan)
    edited.extend(_refresh_derived(repo, skip=(PLUGIN_DISPOSITION,)))
    return edited


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--repo", type=Path, default=Path("."))
    parser.add_argument("--config", type=Path, default=None)
    args = parser.parse_args(argv)
    repo = args.repo.resolve()
    config_path = args.config or repo / "tools" / "scripts" / "versioning.json"
    for path in refresh(repo, config_path):
        print(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())

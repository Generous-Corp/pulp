#!/usr/bin/env python3
"""Prove the negative-contract driver says WHY its control case failed.

`consumption_census_contract.py` opens with a control that feeds unmodified
inputs and requires exit 0. So any real drift in the committed census takes the
whole fourteen-case driver down with it, and the raw failure — "case
valid-current: expected exit 0, got 1" — reads as a broken contract rather than
as the drift gate's own cause showing up twice.

This stages a repo root whose census has drifted by one public header, runs the
driver against the real build tree, and asserts two things: the driver still
FAILS (the note is a note, never a reprieve), and it names the drift gate as
where to read the cause.

Exits 77 when this build tree is not a recorded profile, so the case skips
rather than passing on a measurement it never made.

Run:
    python3 tools/scripts/test_consumption_census_contract_note.py \\
        --build-dir <build-dir> --repo-root <repo-root>
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path

CENSUS_RELPATH = Path("docs/status/consumption-profiles.json")
SCHEMA_RELPATH = Path("docs/status/consumption-profiles.schema.json")
CONTROL_NOTE = "consumption_census_contract_control_failed=true"
DRIFT_TEST_NAME = "consumption-census-drift"


def drift_one_header(document: dict) -> dict:
    """Exactly what a new header under an already-exported root does."""
    for profile in document["profiles"].values():
        name = sorted(profile["targets"])[0]
        profile["targets"][name]["public_headers"]["count"] += 1
        for row in profile["summary"]["ranked_by_closure"]:
            if row["exported_as"] == f"Pulp::{name}":
                row["public_header_count"] += 1
    return document


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--repo-root", type=Path, required=True)
    args = parser.parse_args(argv)

    repo_root = args.repo_root.resolve()
    driver = repo_root / "tools" / "scripts" / "consumption_census_contract.py"

    with tempfile.TemporaryDirectory() as tmp:
        staged_root = Path(tmp) / "repo"
        (staged_root / "docs" / "status").mkdir(parents=True)
        # The scripts stay the real ones: only the census is drifted, so the
        # driver under test is the shipped file rather than a copy of it.
        (staged_root / "tools").mkdir()
        (staged_root / "tools" / "scripts").symlink_to(repo_root / "tools" / "scripts")
        (staged_root / SCHEMA_RELPATH).write_text((repo_root / SCHEMA_RELPATH).read_text())
        drifted = drift_one_header(json.loads((repo_root / CENSUS_RELPATH).read_text()))
        (staged_root / CENSUS_RELPATH).write_text(json.dumps(drifted, indent=2) + "\n")

        result = subprocess.run(
            [
                sys.executable,
                str(driver),
                "--build-dir",
                str(args.build_dir.resolve()),
                "--repo-root",
                str(staged_root),
            ],
            capture_output=True,
            text=True,
        )

    if result.returncode == 77:
        print(
            "consumption_census_contract_note_skipped reason=profile "
            "detail=this build tree is not a recorded profile",
            file=sys.stderr,
        )
        return 77

    if result.returncode == 0:
        print(
            "consumption_census_contract_note: the driver PASSED against a drifted "
            "census; its control case is not fail-closed",
            file=sys.stderr,
        )
        return 1

    if CONTROL_NOTE not in result.stderr:
        print(
            "consumption_census_contract_note: the control failed without saying why\n"
            f"stderr: {result.stderr}",
            file=sys.stderr,
        )
        return 1

    if DRIFT_TEST_NAME not in result.stderr:
        print(
            f"consumption_census_contract_note: the note does not name {DRIFT_TEST_NAME} "
            f"as where to read the cause\nstderr: {result.stderr}",
            file=sys.stderr,
        )
        return 1

    print("consumption_census_contract_note_verified=true")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

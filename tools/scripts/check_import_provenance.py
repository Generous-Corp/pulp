#!/usr/bin/env python3
"""Mainline-PR provenance check for migrated/emitted projects.

`pulp import emit` materialises a Pulp migration scaffold and stamps it
with a `.pulp-import-provenance.json` marker (written by the SDK, not the
importer). This script is the audit that a migrated project landing in a
PR was produced clean-room: it verifies the marker is present and
well-formed, that its provenance values are valid, and that the
clean-room contract is honoured — no framework-source markers leak into
any file the marker labels `generated`.

Neutral by design: this script names NO framework and NO vendor. The
framework-source markers it scans for are loaded as DATA from the
known-frameworks discovery index (the one place real markers live),
defaulting to `tools/import/known-frameworks.json` in the Pulp tree, or
`$PULP_KNOWN_FRAMEWORKS`. With no index resolvable, the structural checks
still run and the content scan is reported as skipped.

Usage:
    check_import_provenance.py <project-dir> [<project-dir> ...]
    check_import_provenance.py --index <known-frameworks.json> <dir>

Exit codes:
    0 — every project carries a well-formed marker honouring the contract
    1 — at least one project failed a check (details printed)
    2 — usage / internal error
"""

from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

MARKER_NAME = ".pulp-import-provenance.json"
EXPECTED_SCHEMA = "pulp.import.provenance.v0"
REQUIRED_FIELDS = ("schema", "importer_id", "framework", "spi_version",
                   "emitted_at", "source_dir_hash", "files")
# Per-file provenance values the SDK writes (see import_run.cpp
# provenance_name + import_emit.hpp). `generated` / `stub` are SDK-authored
# output and are scanned; `copied-user-file` is the user's own code, exempt.
VALID_PROVENANCE = {"generated", "stub", "copied-user-file"}
SCANNED_PROVENANCE = {"generated", "stub"}


def find_default_index() -> Path | None:
    """Resolve the known-frameworks index for the content denylist.

    Order: $PULP_KNOWN_FRAMEWORKS, then walk up from this script for
    tools/import/known-frameworks.json.
    """
    env = os.environ.get("PULP_KNOWN_FRAMEWORKS")
    if env and Path(env).is_file():
        return Path(env)
    here = Path(__file__).resolve()
    for parent in here.parents:
        cand = parent / "tools" / "import" / "known-frameworks.json"
        if cand.is_file():
            return cand
    return None


def load_denylist(index_path: Path | None) -> list[str]:
    """Build the content denylist from the index's content_match markers.

    Mirrors the SDK's denylist_from_known_frameworks (import_emit_scan):
    only `content_match` markers become tokens (path globs are not output
    content). Tokens are lowercased for case-insensitive matching.
    """
    if index_path is None:
        return []
    try:
        data = json.loads(index_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return []
    tokens: list[str] = []
    for fw in data.get("frameworks", []):
        for marker in fw.get("detection", []):
            if marker.get("type") == "content_match":
                pat = marker.get("pattern", "")
                if pat:
                    tokens.append(pat.lower())
    return tokens


def check_project(project_dir: Path, denylist: list[str],
                  scan_enabled: bool) -> list[str]:
    """Return a list of failure messages for one project ([] == pass)."""
    failures: list[str] = []
    marker_path = project_dir / MARKER_NAME

    if not marker_path.is_file():
        return [f"{project_dir}: missing provenance marker ({MARKER_NAME}). "
                f"An emitted/migrated project must carry one."]

    try:
        marker = json.loads(marker_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return [f"{marker_path}: marker is not valid JSON: {exc}"]

    if not isinstance(marker, dict):
        return [f"{marker_path}: marker is not a JSON object."]

    # Required fields present.
    for field in REQUIRED_FIELDS:
        if field not in marker:
            failures.append(f"{marker_path}: missing required field '{field}'.")

    # Schema id correct (misroute guard, mirrors the SDK manifest schema check).
    schema = marker.get("schema")
    if schema is not None and schema != EXPECTED_SCHEMA:
        failures.append(
            f"{marker_path}: schema '{schema}' != expected '{EXPECTED_SCHEMA}'.")

    # Field-shape checks.
    if "spi_version" in marker and not isinstance(marker["spi_version"], int):
        failures.append(f"{marker_path}: spi_version must be an integer.")
    for str_field in ("importer_id", "framework", "emitted_at", "source_dir_hash"):
        val = marker.get(str_field)
        if str_field in marker and (not isinstance(val, str) or not val.strip()):
            failures.append(
                f"{marker_path}: '{str_field}' must be a non-empty string.")

    files = marker.get("files")
    if not isinstance(files, list):
        if "files" in marker:
            failures.append(f"{marker_path}: 'files' must be an array.")
        # No file list — nothing more to validate.
        return failures

    # Per-file provenance values valid; collect the scanned (generated/stub) set.
    scanned_rel: list[str] = []
    for i, entry in enumerate(files):
        if not isinstance(entry, dict):
            failures.append(f"{marker_path}: files[{i}] is not an object.")
            continue
        rel = entry.get("path")
        prov = entry.get("provenance")
        if not isinstance(rel, str) or not rel:
            failures.append(f"{marker_path}: files[{i}] missing 'path'.")
        if prov not in VALID_PROVENANCE:
            failures.append(
                f"{marker_path}: files[{i}] ('{rel}') has invalid provenance "
                f"'{prov}' (valid: {sorted(VALID_PROVENANCE)}).")
        elif prov in SCANNED_PROVENANCE and isinstance(rel, str):
            scanned_rel.append(rel)

    # Clean-room contract: no framework-source marker may appear in any file the
    # marker labels generated/stub. copied-user-file is exempt (the user's own
    # code). The denylist is DATA; with no index the scan is skipped (reported).
    if scan_enabled and denylist:
        for rel in scanned_rel:
            fpath = project_dir / rel
            if not fpath.is_file():
                continue
            try:
                content = fpath.read_text(encoding="utf-8", errors="ignore").lower()
            except OSError:
                continue
            for token in denylist:
                if token in content:
                    failures.append(
                        f"{fpath}: generated file contains framework-source "
                        f"marker '{token}' — violates the clean-room contract.")

    return failures


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("project_dirs", nargs="+", help="emitted/migrated project dirs")
    ap.add_argument("--index", help="known-frameworks.json for the content denylist")
    ap.add_argument("--no-scan", action="store_true",
                    help="skip the clean-room content scan (structural checks only)")
    args = ap.parse_args(argv)

    index_path = Path(args.index) if args.index else find_default_index()
    denylist = [] if args.no_scan else load_denylist(index_path)
    scan_enabled = not args.no_scan

    if scan_enabled and not denylist:
        print("note: no framework-source denylist resolved "
              "(set --index or $PULP_KNOWN_FRAMEWORKS); content scan skipped, "
              "structural checks still run.", file=sys.stderr)

    all_failures: list[str] = []
    for raw in args.project_dirs:
        project_dir = Path(raw)
        if not project_dir.is_dir():
            all_failures.append(f"{project_dir}: not a directory.")
            continue
        all_failures.extend(check_project(project_dir, denylist, scan_enabled))

    if all_failures:
        print("provenance check: FAILED", file=sys.stderr)
        for msg in all_failures:
            print(f"  - {msg}", file=sys.stderr)
        return 1

    print(f"provenance check: ok ({len(args.project_dirs)} project(s))",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

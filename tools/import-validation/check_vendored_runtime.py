#!/usr/bin/env python3
"""Warn when an app's vendored @pulp/react runtime predates the SDK's fixes.

A materialized or JSX import compiles the whole @pulp/react runtime into one
bundle (usually `runtime.js`) that the app checks in and embeds. Nothing
rebuilds it when the SDK improves, so an app can build against a current SDK
while running a runtime weeks older: one shipped editor paid ~44 ms per React
commit for a scoped re-apply fix its vendored bundle never received.

    python3 check_vendored_runtime.py <runtime.js | app-dir>... \
        [--fingerprint packages/pulp-react/runtime-fingerprint.json] [--strict]

For each bundle it reports the fixes the bundle lacks, and the command to
refresh it. A fix counts as present when the bundle's
`/* @pulp/react runtime revision N */` banner is at or past the fix's revision,
or when the fix's signature identifier appears in the bundle -- the second rule
is what lets it judge bundles emitted before the banner existed.

Exit codes: 0 up to date (or stale without --strict: this is a warning, never a
build break by default), 1 stale with --strict, 2 usage error or no @pulp/react
bundle found where one was named.

What it cannot see: a fix whose manifest entry has no signature can only be
judged by the banner, so a banner-less bundle is reported stale for it; a
bundle minified after emission can lose a signature and read as stale; and it
does not know whether the app's code path exercises the fix at all.
"""
from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_FINGERPRINT = REPO / "packages" / "pulp-react" / "runtime-fingerprint.json"
BANNER = re.compile(r"@pulp/react runtime revision (\d+)")
# Present in every @pulp/react bundle old enough to matter, banner or not.
BUNDLE_MARKERS = ("__pulpReactDomRegistry__", "markMaterializedTreeDirty")
MAX_BYTES = 64 * 1024 * 1024
REFRESH = ("re-run the import transform that produced it "
           "(tools/import-design/jsx-runtime/materialized-runtime-transform.mjs "
           "or jsx-transform.mjs, or `pulp import-design`) against this SDK "
           "and commit the regenerated bundle")


def load_fingerprint(path: pathlib.Path) -> dict:
    manifest = json.loads(path.read_text())
    if not isinstance(manifest.get("revision"), int) or manifest["revision"] < 1:
        raise ValueError(f"{path}: revision must be a positive integer")
    return manifest


def is_pulp_react_bundle(text: str) -> bool:
    return bool(BANNER.search(text)) or any(m in text for m in BUNDLE_MARKERS)


def bundle_revision(text: str) -> int | None:
    match = BANNER.search(text)
    return int(match.group(1)) if match else None


def missing_fixes(text: str, manifest: dict) -> list[dict]:
    """Fixes the SDK runtime has that this bundle does not."""
    revision = bundle_revision(text) or 0
    missing = []
    for fix in manifest.get("fixes", []):
        if revision >= fix["revision"]:
            continue
        signature = fix.get("signature")
        if signature and signature in text:
            continue
        missing.append(fix)
    return missing


def find_bundles(target: pathlib.Path) -> list[pathlib.Path]:
    if target.is_file():
        return [target]
    found = []
    for path in sorted(target.rglob("*.js")):
        if "node_modules" in path.parts or not path.is_file():
            continue
        if path.stat().st_size > MAX_BYTES:
            continue
        try:
            text = path.read_text(errors="ignore")
        except OSError:
            continue
        if is_pulp_react_bundle(text):
            found.append(path)
    return found


def report(path: pathlib.Path, text: str, manifest: dict) -> bool:
    """Print one bundle's verdict; return True when it is stale."""
    missing = missing_fixes(text, manifest)
    revision = bundle_revision(text)
    stamp = f"revision {revision}" if revision is not None else "no revision banner"
    if not missing:
        print(f"OK     {path}: @pulp/react runtime current "
              f"({stamp}; SDK revision {manifest['revision']})")
        return False
    print(f"STALE  {path}: vendored @pulp/react runtime ({stamp}) predates "
          f"SDK revision {manifest['revision']}; missing:")
    for fix in missing:
        print(f"         - [{fix['id']}] {fix['summary']}")
    print(f"       refresh: {REFRESH}")
    return True


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("targets", type=pathlib.Path, nargs="+",
                    help="A vendored runtime bundle, or an app directory to scan.")
    ap.add_argument("--fingerprint", type=pathlib.Path, default=DEFAULT_FINGERPRINT,
                    help="The SDK's runtime-fingerprint.json.")
    ap.add_argument("--strict", action="store_true",
                    help="Exit 1 when any bundle is stale (default: warn only).")
    args = ap.parse_args(argv)

    try:
        manifest = load_fingerprint(args.fingerprint)
    except (OSError, ValueError) as exc:
        print(f"check_vendored_runtime: {exc}", file=sys.stderr)
        return 2

    stale = False
    for target in args.targets:
        if not target.exists():
            print(f"check_vendored_runtime: no such path: {target}", file=sys.stderr)
            return 2
        bundles = find_bundles(target)
        if target.is_file() and not is_pulp_react_bundle(target.read_text(errors="ignore")):
            print(f"check_vendored_runtime: {target} is not an @pulp/react bundle",
                  file=sys.stderr)
            return 2
        if not bundles:
            # Said out loud: an app with no bundle found is not "up to date".
            print(f"NONE   {target}: no vendored @pulp/react bundle found")
            continue
        for bundle in bundles:
            stale = report(bundle, bundle.read_text(errors="ignore"), manifest) or stale

    return 1 if (stale and args.strict) else 0


if __name__ == "__main__":
    sys.exit(main())

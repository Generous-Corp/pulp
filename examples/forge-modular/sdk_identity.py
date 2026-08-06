#!/usr/bin/env python3
"""Validate one official Pulp SDK prefix and emit its exact content identity."""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import sys


REPO = Path(__file__).resolve().parents[2]
SCRIPTS = REPO / "tools" / "scripts"
PROVENANCE_PATH = SCRIPTS / "sdk_provenance.py"
# sdk_provenance owns the verification contract and may import sibling helpers
# installed with it. Loading it by path must preserve that package-local import
# surface, including on a combined branch where the helper set just expanded.
sys.path.insert(0, str(SCRIPTS))
SPEC = importlib.util.spec_from_file_location("sdk_provenance", PROVENANCE_PATH)
PROVENANCE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(PROVENANCE)


def directory_sha256(root: Path) -> str:
    digest = hashlib.sha256()
    for path in sorted(item for item in root.rglob("*") if item.is_file()):
        relative = path.relative_to(root).as_posix().encode()
        digest.update(len(relative).to_bytes(8, "big"))
        digest.update(relative)
        with path.open("rb") as handle:
            for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                digest.update(chunk)
    return digest.hexdigest()


def identify(prefix: Path, platform: str, source_sha: str, version: str,
             expected_content_sha256: str) -> dict[str, str]:
    marker = PROVENANCE.verify_release_marker(
        prefix, expected_platform=platform, expected_source_sha=source_sha
    )
    if marker["sdk_version"] != version:
        raise PROVENANCE.ProvenanceError(
            f"SDK version is {marker['sdk_version']!r}, expected {version!r}"
        )
    content_sha256 = directory_sha256(prefix)
    if content_sha256 != expected_content_sha256:
        raise PROVENANCE.ProvenanceError(
            f"SDK content SHA-256 is {content_sha256}, expected "
            f"{expected_content_sha256}"
        )
    return {
        "prefix": str(prefix.resolve()),
        "version": version,
        "source_sha": source_sha,
        "content_sha256": content_sha256,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--platform", required=True)
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--content-sha256", required=True)
    args = parser.parse_args()
    print(json.dumps(identify(args.prefix, args.platform, args.source_sha, args.version,
                              args.content_sha256),
                     sort_keys=True, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

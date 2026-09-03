#!/usr/bin/env python3
"""Lockstep check: the Yoga web-compat oracle must match the Yoga pin.

``tools/harness/oracles/yoga/yoga-supported.json`` is a hand-maintained table
of the CSS properties Yoga understands, transcribed by NAME from a specific
Yoga release. ``tools/harness/adapters/yoga.py`` treats any property absent
from that table as ``Status.OOS`` — out of scope, not a gap.

The Yoga version the table describes is stamped in its ``version`` field; the
Yoga version Pulp actually builds against is pinned in
``tools/cmake/PulpDependencies.cmake``. Nothing linked the two, so bumping the
pin without re-transcribing the table silently reclassified every property and
enum value the new Yoga gained as out-of-scope. Compat coverage then improves
on paper precisely because the measurement stopped being able to see the gap.

This script reads both ends and exits non-zero when they disagree, so the pin
bump and the table refresh have to move together.

Usage::

    python3 tools/scripts/check_yoga_oracle_pin.py

Exit codes:
    0 — the oracle's stamped version matches the Yoga pin.
    1 — drift detected (mismatch printed to stderr).
    2 — an input could not be parsed, or a version stamp is missing entirely
        (a missing stamp would otherwise pass vacuously).
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
PIN_FILE = REPO_ROOT / "tools" / "cmake" / "PulpDependencies.cmake"
ORACLE_FILE = REPO_ROOT / "tools" / "harness" / "oracles" / "yoga" / "yoga-supported.json"

# The oracle's free-text `source` field cites the upstream tree the property
# table was transcribed from, e.g. "facebook/yoga@v3.2.1 YGEnums.h". It is the
# only record of which YGEnums.h a reviewer should diff against, so it drifts
# just as harmfully as the `version` field.
_SOURCE_REF_RE = re.compile(r"facebook/yoga@(\S+)")

# `[^)]` already spans newlines, so a multi-line registration needs no flags.
_REGISTER_RE = re.compile(
    r"pulp_register_fetchcontent_source\s*\(\s*yoga\b[^)]*?\bREF\s+(\S+?)\s*\)"
)
_DECLARE_RE = re.compile(r"FetchContent_Declare\s*\(\s*yoga\b")
_GIT_TAG_RE = re.compile(r"\bGIT_TAG\s+(\S+)")


class CheckError(Exception):
    """Raised when an input cannot be parsed (exit code 2)."""


def _declare_block(text: str, path: Path) -> str:
    """Return the body of the ``FetchContent_Declare(yoga ...)`` call."""
    match = _DECLARE_RE.search(text)
    if not match:
        raise CheckError(f"{path} has no FetchContent_Declare(yoga ...) call")
    open_paren = text.index("(", match.start())
    depth = 0
    for i in range(open_paren, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return text[open_paren + 1:i]
    raise CheckError(
        f"{path} FetchContent_Declare(yoga ...) has no closing paren"
    )


def read_pin(path: Path = PIN_FILE) -> str:
    """Return the Yoga ref Pulp builds against.

    Yoga is pinned twice in the same file — once for the shared source cache
    (``pulp_register_fetchcontent_source ... REF``) and once for the fetch
    itself (``GIT_TAG``). They must agree, or "the pin" has no single value to
    check the oracle against.
    """
    if not path.is_file():
        raise CheckError(f"CMake dependency file not found: {path}")
    text = path.read_text(encoding="utf-8")

    register = _REGISTER_RE.search(text)
    if not register:
        raise CheckError(
            f"{path} has no pulp_register_fetchcontent_source(yoga REF ...) call"
        )
    register_ref = register.group(1)

    git_tag = _GIT_TAG_RE.search(_declare_block(text, path))
    if not git_tag:
        raise CheckError(
            f"{path} FetchContent_Declare(yoga ...) declares no GIT_TAG"
        )
    declare_ref = git_tag.group(1)

    if register_ref != declare_ref:
        raise CheckError(
            f"{path} pins Yoga twice with different refs: "
            f"pulp_register_fetchcontent_source REF={register_ref} vs "
            f"FetchContent_Declare GIT_TAG={declare_ref}"
        )
    return register_ref


def read_oracle(path: Path = ORACLE_FILE) -> dict[str, str]:
    """Return the Yoga refs the oracle claims to describe."""
    if not path.is_file():
        raise CheckError(f"Yoga oracle not found: {path}")
    try:
        oracle = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise CheckError(f"{path} is not valid JSON: {exc}") from exc

    version = oracle.get("version")
    if not isinstance(version, str) or not version.strip():
        raise CheckError(f"{path} has no non-empty 'version' string")

    source = oracle.get("source")
    if not isinstance(source, str) or not source.strip():
        raise CheckError(f"{path} has no non-empty 'source' string")
    source_ref = _SOURCE_REF_RE.search(source)
    if not source_ref:
        raise CheckError(
            f"{path} 'source' does not cite an upstream tree as "
            f"'facebook/yoga@<ref>': {source!r}"
        )

    return {"version": version.strip(), "source_ref": source_ref.group(1)}


def compare(pin: str, oracle: dict[str, str]) -> list[str]:
    """Return human-readable drift messages (empty == in lockstep)."""
    drift: list[str] = []
    if oracle["version"] != pin:
        drift.append(
            f"  oracle 'version' stamp:\n"
            f"    oracle (tools/harness/oracles/yoga/yoga-supported.json): {oracle['version']}\n"
            f"    pin    (tools/cmake/PulpDependencies.cmake):             {pin}"
        )
    if oracle["source_ref"] != pin:
        drift.append(
            f"  oracle 'source' upstream citation:\n"
            f"    oracle (tools/harness/oracles/yoga/yoga-supported.json): facebook/yoga@{oracle['source_ref']}\n"
            f"    pin    (tools/cmake/PulpDependencies.cmake):             {pin}"
        )
    return drift


def main(argv: list[str] | None = None) -> int:
    try:
        pin = read_pin()
        oracle = read_oracle()
    except CheckError as exc:
        print(f"check_yoga_oracle_pin: ERROR: {exc}", file=sys.stderr)
        return 2

    drift = compare(pin, oracle)
    if drift:
        print(
            "check_yoga_oracle_pin: the Yoga web-compat oracle no longer "
            "describes the pinned Yoga:",
            file=sys.stderr,
        )
        for entry in drift:
            print(entry, file=sys.stderr)
        print(
            "\nThe adapter (tools/harness/adapters/yoga.py) reports any property\n"
            "missing from the oracle as out-of-scope, so a stale table hides real\n"
            "gaps instead of reporting them. Re-transcribe the property table from\n"
            f"facebook/yoga@{pin} YGEnums.h and restamp 'version' and 'source', or\n"
            "restore the pin.",
            file=sys.stderr,
        )
        return 1

    print(
        "check_yoga_oracle_pin: OK — yoga-supported.json describes the pinned "
        f"Yoga ({pin})."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

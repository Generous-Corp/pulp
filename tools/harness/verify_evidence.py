#!/usr/bin/env python3
"""Web-compat catalog hygiene validator — evidence check (pulp #1657 / #1737).

Extracted from ``verifier.py`` (roadmap P9-3) as a focused validator
module. Post-processes PASS results: an entry that claims ``supported``
in ``compat.json`` must back the claim with at least one runtime test
reference that (a) names a file that exists AND (b) lists ``[tag]``s
that appear as ``TEST_CASE`` declarations in that file.

This is a pure mechanical move — the code is byte-identical to the
``verifier.py`` original. ``verifier.py`` re-imports every public symbol
defined here so existing ``from tools.harness.verifier import ...``
call sites keep working unchanged.
"""

from __future__ import annotations

import logging
import re
from pathlib import Path
from typing import Optional

from tools.harness.adapters.base import Result
from tools.harness.status import Status

logger = logging.getLogger("pulp.harness.verifier")


# ─────────────────────────────────────────────────────────────────────────────
# Evidence check (#1657 control #1) — requires runtime tests for supported claims
# ─────────────────────────────────────────────────────────────────────────────


def _get_grace_until(compat: dict) -> Optional[str]:
    audit = compat.get("_audit") or {}
    return audit.get("_evidence_grace_until")  # e.g. "2026-05-22"


def _grace_active(grace_until: Optional[str]) -> bool:
    if not grace_until:
        return False
    from datetime import date

    try:
        deadline = date.fromisoformat(grace_until)
        return date.today() <= deadline
    except (ValueError, TypeError):
        return False


# ── pulp #1737 — test-tag validation cache ─────────────────────────────────
# Reading + scanning a test file is cheap, but we do it once per file across
# all catalog entries that reference it. Cache the extracted TEST_CASE tag
# sets + name sets so the verifier doesn't re-parse the same file 50× during
# a full run.
_TEST_CASE_INDEX_CACHE: dict[Path, tuple[set[str], set[str]]] = {}

# Matches `TEST_CASE("test name", "[tag1][tag2]")`. Tolerates Catch2's
# multi-line literal-concat form (`"part1 " "part2"` inside the first arg)
# by matching the SECOND quoted string in the call as the tag block — the
# fallback regex below handles concatenated names by capturing the full
# arg list and reparsing.
_TEST_CASE_FULL_PATTERN = re.compile(
    r'TEST_CASE\s*\(\s*((?:"(?:[^"\\]|\\.)*"\s*)+)\s*,\s*"([^"]*)"',
    re.MULTILINE | re.DOTALL,
)
# Catch2 also allows the single-arg `TEST_CASE("name")` form (no tags).
_TEST_CASE_NAME_ONLY_PATTERN = re.compile(
    r'TEST_CASE\s*\(\s*((?:"(?:[^"\\]|\\.)*"\s*)+)\s*\)',
    re.MULTILINE | re.DOTALL,
)


def _join_concat_string_literals(raw: str) -> str:
    """Catch2's TEST_CASE name argument is often broken across multiple
    "literal " "literal" string concatenations. Re-join into one string."""
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', raw)
    return "".join(parts)


def _extract_test_case_index(file_path: Path) -> tuple[set[str], set[str]]:
    """Return (tags, names) — the union of all ``[tag]`` substrings AND the
    set of TEST_CASE names declared in the file. Empty sets if the file
    doesn't exist or doesn't define any TEST_CASEs."""
    if file_path in _TEST_CASE_INDEX_CACHE:
        return _TEST_CASE_INDEX_CACHE[file_path]
    tags: set[str] = set()
    names: set[str] = set()
    if file_path.exists():
        try:
            text = file_path.read_text(errors="replace")
        except OSError:
            text = ""
        for name_raw, tag_string in _TEST_CASE_FULL_PATTERN.findall(text):
            names.add(_join_concat_string_literals(name_raw))
            for tok in re.findall(r"\[([^\]]+)\]", tag_string):
                tags.add(tok.strip())
        # Single-arg TEST_CASE(name) — names only.
        for name_raw in _TEST_CASE_NAME_ONLY_PATTERN.findall(text):
            joined = _join_concat_string_literals(name_raw)
            if joined:
                names.add(joined)
    _TEST_CASE_INDEX_CACHE[file_path] = (tags, names)
    return tags, names


def _extract_test_case_tags(file_path: Path) -> set[str]:
    """Backward-compat wrapper — returns just the tag set."""
    return _extract_test_case_index(file_path)[0]


# Typed test-reference prefixes per the compat.json convention (see the
# top-level _comment field). These routes name validation in other
# subsystems (visual baselines, semantic fixtures, design fixtures) and
# don't have a TEST_CASE-tag contract, so we accept them on file-existence
# alone (or always, for `cannot-validate:`).
_TYPED_REF_PREFIXES = (
    "unit:", "semantic:", "visual:", "dom:", "behavior:", "cannot-validate:",
)


def _validate_test_ref(repo_root: Path, ref: str) -> tuple[bool, str]:
    """Parse a catalog test reference (e.g. `test/foo.cpp [issue-1737][tag2]`)
    and verify (a) the file exists, AND (b) every listed [tag] appears in
    some TEST_CASE in that file. Returns (ok, reason). When ok is True,
    reason is empty; when False, reason explains the gap.

    Typed prefixes (`unit:` / `semantic:` / `visual:` / `dom:` / `behavior:`
    / `cannot-validate:`) are accepted as-is — they name fixtures in other
    subsystems with their own validation contracts; we don't grep TEST_CASE
    tags out of them. `cannot-validate:` is always accepted (it documents
    why the entry is intentionally unverifiable).
    """
    stripped = ref.strip()
    # Typed prefixes — pulp #1737 followup (Codex P2 on #1768): the
    # part-after-prefix is validated when it looks like a file path
    # (contains `/`) so typo'd / stale typed refs don't auto-pass.
    # `cannot-validate:` always passes — it documents intentional
    # un-verifiability. The other prefixes try a file-existence check;
    # if the body doesn't look like a path, fall back to accept-on-prefix
    # (e.g. `semantic:yoga/aspect-ratio` is a fixture id, not a path).
    for prefix in _TYPED_REF_PREFIXES:
        if stripped.startswith(prefix):
            if prefix == "cannot-validate:":
                return True, ""
            body = stripped[len(prefix):].strip()
            # Heuristic: bodies with a `/` AND a recognised test-source
            # extension are paths (test/foo.cpp, packages/x/foo.test.ts).
            # Anything else is a fixture id we can't validate from here.
            looks_like_path = "/" in body and any(
                body.endswith(ext) for ext in (
                    ".cpp", ".hpp", ".h", ".cc", ".mm",
                    ".test.ts", ".test.tsx", ".spec.ts", ".spec.tsx",
                    ".test.js", ".spec.js",
                )
            )
            if looks_like_path:
                if not (repo_root / body).exists():
                    return False, (
                        f"typed ref `{prefix}{body}` points at `{body}` "
                        "which does not exist in the repo"
                    )
            return True, ""
    # Prefer the bracket-tag form when `[` appears before `::`. This
    # avoids tripping on `::` that appears inside free-form description
    # text after the bracket tags (e.g. `path [tag] (View::method...)`).
    bracket_first = stripped.find("[")
    name_split = stripped.find('::')
    if name_split > 0 and (bracket_first < 0 or name_split < bracket_first):
        path_part = stripped[:name_split].strip()
        name_part = stripped[name_split + 2:].strip()
        # Strip optional surrounding quotes.
        if (name_part.startswith('"') and name_part.endswith('"')
                and len(name_part) >= 2):
            name_part = name_part[1:-1]
        # Non-.cpp files (e.g. .ts jest tests) — file-existence check
        # only; we don't grep for TEST_CASE in .ts files.
        if not path_part.endswith('.cpp'):
            if not (repo_root / path_part).exists():
                return False, f"file `{path_part}` not found"
            return True, ""
        if not (repo_root / path_part).exists():
            return False, f"file `{path_part}` not found"
        _, file_names = _extract_test_case_index(repo_root / path_part)
        if name_part not in file_names:
            return False, (
                f"TEST_CASE name {name_part!r} not found in `{path_part}` — "
                "either the test was renamed / deleted or the catalog "
                "reference is stale"
            )
        return True, ""
    # `path [tag1][tag2]` form — match against TEST_CASE second-arg tags.
    bracket = stripped.find("[")
    if bracket < 0:
        # No tags — file-existence check is the contract.
        if not stripped:
            return False, "empty test reference"
        if not (repo_root / stripped).exists():
            return False, f"file `{stripped}` not found"
        return True, ""
    path_part = stripped[:bracket].strip()
    tag_part = stripped[bracket:]
    if not path_part or not (repo_root / path_part).exists():
        return False, f"file `{path_part}` not found"
    listed_tags = re.findall(r"\[([^\]]+)\]", tag_part)
    if not listed_tags:
        return True, ""
    file_tags, _ = _extract_test_case_index(repo_root / path_part)
    if not file_tags:
        return False, (
            f"file `{path_part}` exists but no TEST_CASE was found — "
            "either the file is empty or the tag is wrong"
        )
    missing = [t for t in listed_tags if t.strip() not in file_tags]
    if missing:
        return False, (
            f"tag(s) {missing} not found in any TEST_CASE in `{path_part}` — "
            "either the test was renamed/deleted or the catalog reference "
            "is stale"
        )
    return True, ""


def check_evidence(repo_root: Path, results: list[Result], compat: dict) -> list[Result]:
    """Post-process PASS results: demote to SUPPORTED_NO_EVIDENCE when the
    catalog entry claims ``supported`` but has no ``tests`` paths that exist in
    the repo. During a grace period the result stays PASS with a warning;
    after the grace period it becomes SUPPORTED_NO_EVIDENCE.

    pulp #1737 — also verifies that any ``[tag]`` substrings inside each test
    reference actually appear in some ``TEST_CASE`` declaration in the named
    file. Without this, a renamed / deleted TEST_CASE leaves a dangling
    reference in the catalog (the em-dash glitch fixed in PR #1752 was an
    instance of exactly this — file existed, tag didn't match because the
    em-dash made the path string unparseable). Tag-validation gaps are
    treated the same as file-existence gaps for grace-period demotion.
    """
    grace_until = _get_grace_until(compat)
    in_grace = _grace_active(grace_until)
    updated: list[Result] = []
    for r in results:
        if r.status is not Status.PASS:
            updated.append(r)
            continue
        if r.entry.status != "supported":
            updated.append(r)
            continue
        # Check for evidence: at least one test reference must (a) point at a
        # file that exists AND (b) name [tag]s that appear as TEST_CASE
        # declarations in that file. pulp #1737 — the second check is new;
        # without it a renamed/deleted TEST_CASE leaves a dangling catalog
        # reference that the verifier didn't notice (PR #1752 hand-fixed
        # 6 such cases that the file-existence-only check let through).
        valid_tests: list[str] = []
        invalid_reasons: list[str] = []
        for t in r.entry.tests:
            ok, reason = _validate_test_ref(repo_root, t)
            if ok:
                valid_tests.append(t)
            else:
                invalid_reasons.append(f"  - {t!r}: {reason}")
        if valid_tests:
            # Even when SOME tests are valid, surface dangling refs as
            # warnings so contributors can clean up the catalog without
            # the entry losing its PASS status.
            for reason in invalid_reasons:
                logger.warning(
                    "evidence: `%s` has a dangling test reference:\n%s",
                    r.entry.name,
                    reason,
                )
            updated.append(r)
            continue
        # No evidence (or all references were dangling).
        if in_grace:
            grace_msg = (
                "evidence: `%s` claims supported but has no validatable "
                "tests — grace period until %s, will fail after"
            )
            if invalid_reasons:
                grace_msg += "\n  reasons:\n" + "\n".join(invalid_reasons)
            logger.warning(grace_msg, r.entry.name, grace_until)
            updated.append(r)
        else:
            detail = "catalog claims supported but evidence.tests is empty"
            if invalid_reasons:
                detail = (
                    "catalog claims supported but every test reference is "
                    "dangling: " + "; ".join(
                        line.lstrip("  - ").rstrip() for line in invalid_reasons
                    )
                )
            updated.append(
                Result(
                    entry=r.entry,
                    status=Status.SUPPORTED_NO_EVIDENCE,
                    detail=detail,
                )
            )
    return updated

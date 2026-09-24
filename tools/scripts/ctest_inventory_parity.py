#!/usr/bin/env python3
"""Prove a test-registration refactor changed nothing CTest can see.

Consolidating several Catch2 executables into one, moving a suite between
manifests, or re-routing a discovery call are all changes whose only
acceptable effect on `ctest` is the executable behind each test. Every
registered name must still be registered, and every name must keep exactly
the properties it had: LABELS decide which lane runs it, TIMEOUT decides
whether it passes there, RESOURCE_LOCK / RUN_SERIAL / ENVIRONMENT decide
whether it can run at all. A case that silently loses its `slow` label lands on
the required gate; one that silently gains it vanishes from the gate. Neither
fails a build.

This compares two `ctest --show-only=json-v1` snapshots as multisets of
(name, properties). The executable is deliberately not part of the key, since
changing it is the point of the refactor. Names that were intentionally renamed
(Catch2 refuses two cases with one name inside a single binary, so a merge can
force a rename) are declared in a rename map that names the OLD executable, so
a rename can never be satisfied by a different test that happened to share the
name.

    ctest_inventory_parity.py snapshot --build-dir build --out before.json
    ... refactor, reconfigure, rebuild the affected targets ...
    ctest_inventory_parity.py snapshot --build-dir build --out after.json
    ctest_inventory_parity.py compare before.json after.json \
        --rename-map renames.json --min-tests 100

A snapshot lists every registered test, so the comparison is whole-inventory:
an accidental change to an unrelated manifest is a finding, not noise. Tests
that were not built at snapshot time register as `<target>_NOT_BUILT-<hash>`
placeholders, which compare like any other name; build the refactored targets
before snapshotting or the placeholders will differ.

`--min-tests` is the control: a snapshot taken from the wrong directory, or from
a tree whose `TEST_INCLUDE_FILES` never loaded, compares equal to itself and
reports parity over nothing. Two near-empty snapshots are refused.
"""

from __future__ import annotations

import argparse
import collections
import json
import os
import pathlib
import sys
from typing import Any

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from changed_surface_inventory import load_ctest_json  # noqa: E402


def _property_value(name: str, value: Any) -> Any:
    # When two registrations share one test name, CTest applies the second
    # registration's set_tests_properties() to the first test and APPENDS its
    # ENVIRONMENT, so a colliding name carries the same variable twice. Two
    # identical assignments are one assignment; comparing them as such lets a
    # rename that resolves the collision read as parity rather than drift.
    if name == "ENVIRONMENT" and isinstance(value, list):
        return list(dict.fromkeys(value))
    return value


def normalize(test: dict[str, Any]) -> dict[str, Any]:
    props = test.get("properties") or []
    command = test.get("command") or []
    return {
        "name": test["name"],
        "executable": os.path.basename(command[0]) if command else "",
        "properties": sorted(
            (p["name"], json.dumps(_property_value(p["name"], p["value"]), sort_keys=True))
            for p in props
        ),
    }


def renormalize(entries: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Re-apply value normalization to a stored snapshot.

    Snapshots persist property values as JSON strings, so a snapshot written
    before a normalization rule existed is brought up to date on load rather
    than having to be retaken.
    """
    for entry in entries:
        entry["properties"] = sorted(
            (name, json.dumps(_property_value(name, json.loads(value)), sort_keys=True))
            for name, value in entry["properties"]
        )
    return entries


def snapshot(build_dir: pathlib.Path) -> list[dict[str, Any]]:
    return [normalize(t) for t in load_ctest_json(build_dir)]


def _key(entry: dict[str, Any]) -> tuple[str, tuple[tuple[str, str], ...]]:
    return entry["name"], tuple(tuple(p) for p in entry["properties"])


def apply_renames(
    before: list[dict[str, Any]], renames: list[dict[str, str]]
) -> list[dict[str, Any]]:
    """Return a copy of `before` with each rename applied to exactly one entry."""
    result = [dict(entry) for entry in before]
    for rename in renames:
        matches = [
            i
            for i, entry in enumerate(result)
            if entry["name"] == rename["from"]
            and entry["executable"] == rename["executable"]
        ]
        if len(matches) != 1:
            raise SystemExit(
                f"rename {rename['from']!r} (executable {rename['executable']}) "
                f"matched {len(matches)} tests in the before-snapshot; "
                f"a rename must identify exactly one"
            )
        result[matches[0]]["name"] = rename["to"]
    return result


def compare(
    before: list[dict[str, Any]],
    after: list[dict[str, Any]],
    renames: list[dict[str, str]],
    expect_new: set[str] | None = None,
) -> list[str]:
    """Return human-readable findings; empty means parity.

    `expect_new` names tests the refactor deliberately adds (a new check
    registered in the same change); each must appear after and not before.
    """
    expect_new = set(expect_new or ())
    before = apply_renames(before, renames)
    after_names = {e["name"] for e in after}
    before_names = {e["name"] for e in before}
    findings: list[str] = []
    for name in sorted(expect_new):
        if name in before_names:
            findings.append(f"expected-new    {name!r} already existed before")
        if name not in after_names:
            findings.append(f"expected-new    {name!r} is not registered after")
    after = [e for e in after if e["name"] not in expect_new]
    before_counts = collections.Counter(_key(e) for e in before)
    after_counts = collections.Counter(_key(e) for e in after)
    missing = before_counts - after_counts
    extra = after_counts - before_counts
    if not missing and not extra:
        return findings

    missing_by_name: dict[str, list[tuple]] = collections.defaultdict(list)
    extra_by_name: dict[str, list[tuple]] = collections.defaultdict(list)
    for (name, props), n in missing.items():
        missing_by_name[name].extend([props] * n)
    for (name, props), n in extra.items():
        extra_by_name[name].extend([props] * n)

    for name in sorted(set(missing_by_name) | set(extra_by_name)):
        lost = missing_by_name.get(name, [])
        gained = extra_by_name.get(name, [])
        if lost and gained:
            # Same name on both sides: the properties drifted.
            for old, new in zip(lost, gained):
                old_d, new_d = dict(old), dict(new)
                for prop in sorted(set(old_d) | set(new_d)):
                    if old_d.get(prop) != new_d.get(prop):
                        findings.append(
                            f"property drift  {name!r}: {prop} "
                            f"{old_d.get(prop, '<absent>')} -> {new_d.get(prop, '<absent>')}"
                        )
            if len(lost) != len(gained):
                findings.append(
                    f"multiplicity     {name!r}: {len(lost)} before, {len(gained)} after"
                )
        elif lost:
            findings.append(f"missing after    {name!r} (x{len(lost)})")
        else:
            findings.append(f"unexpected after {name!r} (x{len(gained)})")
    return findings


def self_check() -> None:
    """The comparator must be able to fail before its silence means anything."""
    before = [
        {"name": "a", "executable": "one", "properties": [["LABELS", '["slow"]']]},
        {"name": "b", "executable": "one", "properties": [["TIMEOUT", "30.0"]]},
        {"name": "dup", "executable": "one", "properties": []},
        {"name": "dup", "executable": "two", "properties": []},
    ]
    merged = [
        {"name": "a", "executable": "group", "properties": [["LABELS", '["slow"]']]},
        {"name": "b", "executable": "group", "properties": [["TIMEOUT", "30.0"]]},
        {"name": "dup", "executable": "group", "properties": []},
        {"name": "dup (two)", "executable": "group", "properties": []},
    ]
    renames = [{"from": "dup", "to": "dup (two)", "executable": "two"}]
    if compare(before, merged, renames):
        raise SystemExit("self-check: a faithful merge with a declared rename must pass")

    # A rename that names the wrong executable must be refused, not matched to
    # the other test of that name.
    try:
        compare(before, merged, [{"from": "dup", "to": "dup (two)", "executable": "nope"}])
    except SystemExit:
        pass
    else:
        raise SystemExit("self-check: a rename naming the wrong executable was accepted")

    drifted = json.loads(json.dumps(merged))
    drifted[0]["properties"] = []
    findings = compare(before, drifted, renames)
    if not any(f.startswith("property drift") and "'a'" in f for f in findings):
        raise SystemExit(f"self-check: dropped LABELS on 'a' not reported: {findings}")

    lost = merged[:-1]
    findings = compare(before, lost, renames)
    if not any(f.startswith("missing after") and "dup (two)" in f for f in findings):
        raise SystemExit(f"self-check: a vanished test not reported: {findings}")

    grown = merged + [{"name": "c", "executable": "group", "properties": []}]
    findings = compare(before, grown, renames)
    if not any(f.startswith("unexpected after") and "'c'" in f for f in findings):
        raise SystemExit(f"self-check: an extra test not reported: {findings}")
    if compare(before, grown, renames, expect_new={"c"}):
        raise SystemExit("self-check: a declared new test must not be a finding")
    if not compare(before, merged, renames, expect_new={"c"}):
        raise SystemExit("self-check: a declared new test that never appeared must be a finding")

    doubled = json.loads(json.dumps(before))
    doubled[0]["properties"] = [["ENVIRONMENT", '["X=1", "X=1"]']]
    single = json.loads(json.dumps(merged))
    single[0]["properties"] = [["ENVIRONMENT", '["X=1"]']]
    raw_doubled = [{"name": "a", "command": ["one"], "properties": [{"name": "ENVIRONMENT", "value": ["X=1", "X=1"]}]}]
    raw_single = [{"name": "a", "command": ["group"], "properties": [{"name": "ENVIRONMENT", "value": ["X=1"]}]}]
    if compare([normalize(t) for t in raw_doubled], [normalize(t) for t in raw_single], []):
        raise SystemExit("self-check: a doubled ENVIRONMENT entry must compare equal to a single one")
    raw_other = [{"name": "a", "command": ["group"], "properties": [{"name": "ENVIRONMENT", "value": ["X=2"]}]}]
    if not compare([normalize(t) for t in raw_doubled], [normalize(t) for t in raw_other], []):
        raise SystemExit("self-check: a changed ENVIRONMENT value must still be reported")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)

    snap = sub.add_parser("snapshot", help="write the normalized ctest inventory")
    snap.add_argument("--build-dir", required=True, type=pathlib.Path)
    snap.add_argument("--out", required=True, type=pathlib.Path)

    cmp_ = sub.add_parser("compare", help="compare two snapshots")
    cmp_.add_argument("before", type=pathlib.Path)
    cmp_.add_argument("after", type=pathlib.Path)
    cmp_.add_argument(
        "--rename-map",
        type=pathlib.Path,
        help='JSON list of {"from", "to", "executable"} for deliberate renames',
    )
    cmp_.add_argument(
        "--expect-new",
        action="append",
        default=[],
        metavar="NAME",
        help="a test the change deliberately adds; must be absent before and present after",
    )
    cmp_.add_argument(
        "--min-tests",
        type=int,
        default=100,
        help="refuse to report parity when either snapshot has fewer tests",
    )

    sub.add_parser("self-check", help="prove the comparator can fail")

    args = parser.parse_args(argv)
    self_check()

    if args.command == "self-check":
        print("ctest inventory parity self-check passed")
        return 0

    if args.command == "snapshot":
        tests = snapshot(args.build_dir)
        args.out.write_text(json.dumps(tests, indent=1, sort_keys=True) + "\n")
        executables = {t["executable"] for t in tests if t["executable"]}
        print(f"{len(tests)} tests across {len(executables)} executables -> {args.out}")
        return 0

    before = renormalize(json.loads(args.before.read_text()))
    after = renormalize(json.loads(args.after.read_text()))
    renames = json.loads(args.rename_map.read_text()) if args.rename_map else []
    for side, tests in (("before", before), ("after", after)):
        if len(tests) < args.min_tests:
            print(
                f"{side} snapshot has {len(tests)} tests, below the {args.min_tests} "
                f"floor; refusing to call that parity",
                file=sys.stderr,
            )
            return 1

    findings = compare(before, after, renames, expect_new=set(args.expect_new))
    exes_before = {t["executable"] for t in before if t["executable"]}
    exes_after = {t["executable"] for t in after if t["executable"]}
    print(
        f"{len(before)} tests / {len(exes_before)} executables before; "
        f"{len(after)} tests / {len(exes_after)} executables after; "
        f"{len(renames)} declared renames, {len(args.expect_new)} declared new tests"
    )
    if findings:
        print(f"{len(findings)} finding(s):", file=sys.stderr)
        for finding in findings:
            print(f"  {finding}", file=sys.stderr)
        return 1
    print("parity: every test name and property set is preserved")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Guard launcher bulk construction and persistent undo accounting."""

from __future__ import annotations

import argparse
from pathlib import Path


def bulk_build_violations(source: str) -> list[str]:
    begin = source.find("build_launcher(")
    end = source.find("validate_scene_for_insert", begin)
    if begin < 0 or end < 0:
        return ["missing build_launcher boundary"]
    body = source[begin:end]
    return ["build_launcher uses reference_add"] if "reference_add(" in body else []


def retained_size_violations(source: str) -> list[str]:
    violations = []
    if "launcher_slot_list_owned_storage(value.scene.slots)" not in source:
        violations.append("InsertScene omits SlotList owned storage")
    if "saturated_multiply(value.scene.slots.size(), sizeof(Slot))" in source:
        violations.append("InsertScene charges only logical Slot payload")
    return violations


def self_test() -> None:
    bulk = "build_launcher() { build_references(edges); }\nvalidate_scene_for_insert"
    assert not bulk_build_violations(bulk)
    mutated_bulk = "build_launcher() { reference_add(root, target, source); }\nvalidate_scene_for_insert"
    assert bulk_build_violations(mutated_bulk) == ["build_launcher uses reference_add"]

    retained = "launcher_slot_list_owned_storage(value.scene.slots)"
    assert not retained_size_violations(retained)
    mutated_retained = "saturated_multiply(value.scene.slots.size(), sizeof(Slot))"
    assert retained_size_violations(mutated_retained) == [
        "InsertScene omits SlotList owned storage",
        "InsertScene charges only logical Slot payload",
    ]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--launcher-source", type=Path)
    parser.add_argument("--command-source", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if bool(args.launcher_source) == bool(args.command_source):
        parser.error("provide exactly one source")
    if args.launcher_source:
        found = bulk_build_violations(args.launcher_source.read_text(encoding="utf-8"))
        source = args.launcher_source
    else:
        found = retained_size_violations(args.command_source.read_text(encoding="utf-8"))
        source = args.command_source
    if found:
        print(f"{source}: {'; '.join(found)}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

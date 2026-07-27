#!/usr/bin/env python3
"""Guard launcher transaction reductions against authored-order scans."""

from __future__ import annotations

import argparse
from pathlib import Path


FORBIDDEN = (
    "sequence->scenes()",
    "scene->slots",
)


def violations(source: str) -> list[str]:
    return [token for token in FORBIDDEN if token in source]


def self_test() -> None:
    persistent = """
auto erased = SequenceEditAccess::erase_scene(*sequence, remove.scene_id);
auto slot = SequenceEditAccess::erase_slot(*sequence, remove.scene_id, remove.slot_id);
"""
    assert not violations(persistent)
    for mutation in FORBIDDEN:
        assert violations(f"auto scan = {mutation};") == [mutation]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", nargs="?", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if args.source is None:
        parser.error("source is required unless --self-test is used")
    found = violations(args.source.read_text(encoding="utf-8"))
    if found:
        print(f"{args.source}: authored-order launcher scan(s): {', '.join(found)}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

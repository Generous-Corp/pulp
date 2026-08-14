#!/usr/bin/env python3
"""Focused offline proof for tag retrieval and exclusive-maker validation."""

from __future__ import annotations

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import intent_context as I  # noqa: E402


INVENTORY = {
    "CVfunk": {"modules": {
        "Step": {"name": "Step", "tags": ["Sequencer"],
                 "description": "Clocked pattern generator"},
        "Kick": {"name": "Kick", "tags": ["Drum"],
                 "description": "Bass drum voice"},
    }},
    "Else": {"modules": {
        "Sampler": {"name": "Sampler", "tags": ["Sampler"],
                    "description": "Sample player"},
    }},
    "Core": {"modules": {
        "AudioInterface2": {"name": "Audio", "tags": []},
    }},
}

INDEX = {
    "CVfunk": {"modules": {
        "Step": {"tags": ["Sequencer"]},
        "Kick": {"tags": ["Drum"]},
    }},
    "Else": {"modules": {
        "Sampler": {"tags": ["Sampler"]},
    }},
}


def check_tag_context() -> tuple[int, int]:
    bad, ran = 0, 0
    refs = I.resolve_tag_references(
        "build a drum patch using #drum modules and #sequencers", INVENTORY, INDEX)
    ran += 1
    if {(reference.tag, reference.explicit) for reference in refs} != {
            ("Drum", True), ("Sequencer", True)}:
        bad += 1
        print(f"  WRONG  explicit singular/plural tags resolved as {refs}")
    else:
        print("  ok     #drum and #sequencers resolve to canonical VCV tags")

    refs = I.resolve_tag_references("build with a quantizer and sampler", INVENTORY, INDEX)
    ran += 1
    if {(reference.tag, reference.explicit) for reference in refs} != {("Sampler", False)}:
        bad += 1
        print(f"  WRONG  ordinary language was not a retrieval cue: {refs}")
    else:
        print("  ok     ordinary tag words stay cues instead of hard constraints")

    refs = I.resolve_tag_references("make a #sequencer patch", INVENTORY, INDEX)
    good = {"modules": [
        {"plugin": "CVfunk", "model": "Step"},
        {"plugin": "Core", "model": "AudioInterface2"},
    ]}
    bad_patch = {"modules": [{"plugin": "Else", "model": "Sampler"}]}
    ran += 1
    if I.required_tag_errors(good, INVENTORY, refs) or not I.required_tag_errors(
            bad_patch, INVENTORY, refs):
        bad += 1
        print("  WRONG  # tag validation did not distinguish a matching patch")
    else:
        print("  ok     explicit tags are verified against final patch modules")
    return bad, ran


def check_exclusive_maker() -> tuple[int, int]:
    mentions = {"CV funk": {"slugs": ["CVfunk"], "exclusive": True}}
    good = {"modules": [
        {"plugin": "CVfunk", "model": "Kick"},
        {"plugin": "Core", "model": "AudioInterface2"},
    ]}
    bad = {"modules": [
        {"plugin": "CVfunk", "model": "Kick"},
        {"plugin": "Else", "model": "Sampler"},
    ]}
    ran = 1
    if I.exclusive_maker_errors(good, mentions) or not I.exclusive_maker_errors(bad, mentions):
        print("  WRONG  only-maker validation did not reject the foreign module")
        return 1, ran
    print("  ok     only @maker permits Core I/O but rejects other makers")
    return 0, ran


def main() -> int:
    bad, ran = 0, 0
    for check in (check_tag_context, check_exclusive_maker):
        failures, checks = check()
        bad += failures
        ran += checks
    print(f"\n{ran - bad}/{ran} correct")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())

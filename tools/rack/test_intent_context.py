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
        "Compressor": {"name": "Compressor", "tags": ["Dynamics"],
                       "description": "Dynamics processor"},
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

    narrowed = {("Core", "AudioInterface2")}
    choices = I.add_required_candidates(
        refs, INVENTORY, narrowed, allowed={
            ("CVfunk", "Step"), ("Core", "AudioInterface2")})
    ran += 1
    if ("CVfunk", "Step") not in narrowed or \
            [row[:2] for row in choices["Sequencer"]] != [("CVfunk", "Step")]:
        bad += 1
        print("  WRONG  required tag choices fell out of a narrowed inventory")
    else:
        print("  ok     required tag choices survive structural inventory narrowing")

    ran += 1
    try:
        I.add_required_candidates(refs, INVENTORY, set(), allowed=set())
    except ValueError as error:
        if "#Sequencer" not in str(error):
            bad += 1
            print(f"  WRONG  unavailable tag refusal omitted its name: {error}")
        else:
            print("  ok     an impossible required tag fails before a provider call")
    else:
        bad += 1
        print("  WRONG  an impossible required tag reached model generation")

    dynamics_refs = I.resolve_tag_references(
        "Build a euclidean rhythm. Include at least one #Dynamics module.",
        INVENTORY, INDEX)
    selected = {("Core", "AudioInterface2"), ("CVfunk", "Step")}
    I.add_required_candidates(
        dynamics_refs, INVENTORY, selected,
        allowed={("Else", "Compressor"), ("Core", "AudioInterface2"),
                 ("CVfunk", "Step")})
    narrowed = {
        plugin: {"modules": {
            model: INVENTORY[plugin]["modules"][model]
            for chosen_plugin, model in selected
            if chosen_plugin == plugin}}
        for plugin in INVENTORY
        if any(chosen_plugin == plugin for chosen_plugin, _ in selected)}
    context = I.render_tag_context(dynamics_refs, narrowed)
    ran += 1
    if "`Else/Compressor`" not in context or \
            "No installed module currently carries this tag" in context:
        bad += 1
        print("  WRONG  exact Euclidean #Dynamics context remained contradictory")
    else:
        print("  ok     exact Euclidean #Dynamics context exposes a legal choice")

    crowded = {
        "Blocked": {"modules": {
            f"Dynamics{index}": {"name": f"Blocked {index}",
                                  "tags": ["Dynamics"]}
            for index in range(8)}},
        "Allowed": {"modules": {
            "Dynamics": {"name": "Allowed", "tags": ["Dynamics"]}}},
    }
    selected = set()
    choices = I.add_required_candidates(
        dynamics_refs, crowded, selected,
        allowed={("Allowed", "Dynamics")}, limit=6)
    ran += 1
    if [row[:2] for row in choices["Dynamics"]] != [("Allowed", "Dynamics")]:
        bad += 1
        print("  WRONG  candidate limit hid the only permitted tag module")
    else:
        print("  ok     policy filtering happens before the tag candidate limit")

    existing = {"modules": [
        {"plugin": "Else", "model": "Compressor"},
        {"plugin": "Core", "model": "AudioInterface2"},
    ]}
    remaining = I.unsatisfied_explicit_references(
        dynamics_refs, existing, INVENTORY)
    ran += 1
    if remaining:
        bad += 1
        print("  WRONG  refinement re-requested an already satisfied tag")
    else:
        print("  ok     an existing matching module satisfies refinement tags")
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

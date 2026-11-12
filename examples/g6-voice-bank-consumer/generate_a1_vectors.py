#!/usr/bin/env python3
"""Generate the installed C++ probe inputs from the frozen A1 JSON fixture."""

import argparse
import hashlib
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parent
SOURCE = ROOT / "a1_reference_vectors.json"
OUTPUT = ROOT / "a1_reference_vectors.hpp"


def literal(value):
    return f"{float(value)!r}f"


def row(values, size=8):
    return "{" + ", ".join(literal(v) for v in (list(values) + [0.0] * (size - len(values)))) + "}"


def matrix(values):
    return "{" + ",\n            ".join(row(v) for v in values) + "}"


def document(data, digest):
    voices = data["voice_vectors"]
    output = [
        "#pragma once",
        "#include <cstddef>",
        "namespace pulp::examples::g6::a1 {",
        f"// Generated from frozen A1 JSON SHA-256 {digest}. Do not edit by hand.",
        "enum class Kind { Sum, Release, Steal };",
        "struct Case {",
        "    const char* id;",
        "    Kind kind;",
        "    std::size_t frames;",
        "    std::size_t capture_start;",
        "    bool active[8];",
        "    float gain[8];",
        "    float left[8][8];",
        "    float right[8][8];",
        "    float new_left[8];",
        "    float new_right[8];",
        "    float expected_left[8];",
        "    float expected_right[8];",
        "    std::size_t note_off_sample;",
        "    std::size_t steal_sample;",
        "};",
        "// clang-format off",
        "inline constexpr Case kCases[] = {",
    ]
    for case in data["cases"]:
        kind = case["kind"]
        active = case.get("active_slots", [0])
        if kind == "sum":
            gain = [voice["gain"] for voice in voices]
            left = [voice["left"] for voice in voices]
            right = [voice["right"] for voice in voices]
        elif kind == "sum_custom":
            gain = [1.0] * 8
            left = [[value] for value in case["left"]]
            right = [[value] for value in case["right"]]
        elif kind == "release":
            gain = [1.0] * 8
            left = [case["source_left"]] + [[] for _ in range(7)]
            right = [case["source_right"]] + [[] for _ in range(7)]
        elif kind == "steal":
            gain = [1.0] * 8
            left = [case["old_left"]] + [[] for _ in range(7)]
            right = [case["old_right"]] + [[] for _ in range(7)]
        else:
            raise ValueError(f"unknown case kind: {kind}")
        output.extend([
            "    Case{",
            f'        .id = "{case["id"]}",',
            f'        .kind = Kind::{"Sum" if kind.startswith("sum") else kind.title()},',
            f'        .frames = {len(case["expected_left"])},',
            f'        .capture_start = {4 if kind == "sum" and len(active) < 8 else 0},',
            "        .active = {" + ", ".join("true" if i in active else "false" for i in range(8)) + "},",
            "        .gain = " + row(gain) + ",",
            "        .left = " + matrix(left) + ",",
            "        .right = " + matrix(right) + ",",
            "        .new_left = " + row(case.get("new_left", [])) + ",",
            "        .new_right = " + row(case.get("new_right", [])) + ",",
            "        .expected_left = " + row(case["expected_left"]) + ",",
            "        .expected_right = " + row(case["expected_right"]) + ",",
            f'        .note_off_sample = {case.get("note_off_sample", 0)},',
            f'        .steal_sample = {case.get("steal_sample", 0)},',
            "    },",
        ])
    output.extend(["};", "// clang-format on", "} // namespace pulp::examples::g6::a1", ""])
    return "\n".join(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    raw = SOURCE.read_bytes()
    data = json.loads(raw)
    if data["reference_profile"]["slot_count"] != 8:
        raise SystemExit("A1 fixture slot count changed")
    rendered = document(data, hashlib.sha256(raw).hexdigest())
    if args.check:
        if OUTPUT.read_text() != rendered:
            raise SystemExit("A1 generated header drift")
    else:
        OUTPUT.write_text(rendered)


if __name__ == "__main__":
    main()

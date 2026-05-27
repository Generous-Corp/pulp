#!/usr/bin/env python3
"""Pulp helper for the bounded JSFX support lane."""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import asdict, dataclass
from pathlib import Path

SUPPORTED_SECTIONS = ("init", "slider", "block", "sample")
# JSFX section directives can carry trailing arguments — `@gfx WIDTH HEIGHT`
# is the canonical example (REAPER documents `@gfx 200 200` as the standard
# preferred-size declaration). Anchoring the regex to end-of-line would
# silently ignore those forms and let `@gfx 200 200` slip past the bounded
# subset check. Capture only the section name; everything after the first
# whitespace is treated as the directive's arguments and ignored.
SECTION_PATTERN = re.compile(r"^@([A-Za-z0-9_]+)(?:\s.*)?$")
SLIDER_PATTERN = re.compile(r"^slider(\d+):")


class JsfxSubsetError(RuntimeError):
    pass


@dataclass
class SliderInfo:
    index: int
    line: str


@dataclass
class JsfxSummary:
    file: str
    desc: str
    slider_count: int
    sliders: list[SliderInfo]
    sections: list[str]
    unsupported_sections: list[str]

    @property
    def supported(self) -> bool:
        return not self.unsupported_sections


def parse_jsfx(path: Path) -> JsfxSummary:
    if not path.is_file():
        raise JsfxSubsetError(f"jsfx file not found: {path}")

    desc = ""
    sections: list[str] = []
    section_set: set[str] = set()
    unsupported_sections: list[str] = []
    sliders: list[SliderInfo] = []
    slider_indices: set[int] = set()

    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.strip()
        if not line or line.startswith("//") or line.startswith("/*") or line.startswith("*"):
            continue

        if line.startswith("desc:") and not desc:
            desc = line[len("desc:") :].strip()
            continue

        slider_match = SLIDER_PATTERN.match(line)
        if slider_match:
            index = int(slider_match.group(1))
            if not 1 <= index <= 64:
                raise JsfxSubsetError(f"slider index out of range in {path}: {line}")
            if index in slider_indices:
                raise JsfxSubsetError(f"duplicate slider{index} in {path}")
            slider_indices.add(index)
            sliders.append(SliderInfo(index=index, line=line))
            continue

        section_match = SECTION_PATTERN.match(line)
        if section_match:
            section = section_match.group(1)
            if section in section_set:
                raise JsfxSubsetError(f"duplicate @{section} section in {path}")
            section_set.add(section)
            sections.append(section)
            if section not in SUPPORTED_SECTIONS:
                unsupported_sections.append(section)

    if not desc:
        raise JsfxSubsetError(f"missing desc: header in {path}")

    if not sections:
        raise JsfxSubsetError(f"no JSFX sections found in {path}")

    return JsfxSummary(
        file=str(path),
        desc=desc,
        slider_count=len(sliders),
        sliders=sliders,
        sections=sections,
        unsupported_sections=unsupported_sections,
    )


def print_human(summary: JsfxSummary) -> None:
    print(f"file: {summary.file}")
    print(f"desc: {summary.desc}")
    print(f"slider_count: {summary.slider_count}")
    if summary.sliders:
        print("sliders:")
        for slider in summary.sliders:
            print(f"  - slider{slider.index}: {slider.line}")
    print(f"sections: {', '.join(summary.sections)}")
    if summary.unsupported_sections:
        print(f"unsupported_sections: {', '.join(summary.unsupported_sections)}")
    else:
        print("unsupported_sections: none")
    print(f"supported: {'yes' if summary.supported else 'no'}")


def cmd_doctor(args: argparse.Namespace) -> int:
    summary = parse_jsfx(Path(args.file).expanduser().resolve())

    if args.json:
        payload = asdict(summary)
        payload["sliders"] = [asdict(item) for item in summary.sliders]
        payload["supported"] = summary.supported
        print(json.dumps(payload, indent=2))
    else:
        print_human(summary)

    if summary.unsupported_sections:
        sections = ", ".join(f"@{name}" for name in summary.unsupported_sections)
        print(
            f"error: unsupported JSFX sections for the current Pulp subset: {sections}",
            file=sys.stderr,
        )
        return 2

    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Validate JSFX files against Pulp's bounded audio-focused subset: "
            "@init, @slider, @block, @sample, slider1..slider64."
        )
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    doctor = subparsers.add_parser(
        "doctor",
        help="Parse a .jsfx file and fail clearly if it uses unsupported sections.",
    )
    doctor.add_argument("--file", required=True, help="Path to a .jsfx file")
    doctor.add_argument("--json", action="store_true", help="Emit machine-readable JSON")
    doctor.set_defaults(func=cmd_doctor)
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except JsfxSubsetError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())

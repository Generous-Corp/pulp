#!/usr/bin/env python3
"""Fail when WAM/WebCLAP omit a production timebase/timeline/playback TU."""

from __future__ import annotations

import argparse
import pathlib
import re
import sys


SUBSYSTEMS = ("timebase", "timeline", "playback")
REQUIRED_CONTRACT_TOKENS = (
    "cxx_std_20",
    "-fno-exceptions",
    "-fno-rtti",
    "PULP_COMPILE_EXECUTOR_DISABLE_THREADS=1",
)
LANES = {
    "WAM": ("tools/cmake/PulpWam.cmake", "_PULP_WAM_CORE_SOURCES"),
    "WebCLAP": ("tools/cmake/PulpWclap.cmake", "_PULP_WCLAP_CORE_SOURCES"),
}


def source_block(text: str, variable: str) -> str:
    match = re.search(rf"set\({re.escape(variable)}\s+(.*?)\n\)", text, re.DOTALL)
    if not match:
        raise ValueError(f"missing set({variable} ...)")
    return match.group(1)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=pathlib.Path,
                        default=pathlib.Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    root = args.root.resolve()
    expected = {
        path.relative_to(root).as_posix()
        for subsystem in SUBSYSTEMS
        for path in (root / "core" / subsystem / "src").rglob("*.cpp")
    }

    failures: list[str] = []
    for lane, (relative_file, variable) in LANES.items():
        cmake = root / relative_file
        try:
            text = cmake.read_text(encoding="utf-8")
            block = source_block(text, variable)
        except (OSError, ValueError) as error:
            failures.append(f"{lane}: {error}")
            continue
        listed = {
            match.group(1)
            for match in re.finditer(
                r"\$\{_[A-Z_]+_ROOT\}/(core/(?:timebase|timeline|playback)/src/[^\s)]+\.cpp)",
                block,
            )
        }
        missing = sorted(expected - listed)
        if missing:
            failures.append(f"{lane}: missing " + ", ".join(missing))
        missing_tokens = [token for token in REQUIRED_CONTRACT_TOKENS if token not in text]
        if missing_tokens:
            failures.append(f"{lane}: missing contract tokens " + ", ".join(missing_tokens))

    if failures:
        for failure in failures:
            print(f"web-timeline-source-closure: {failure}", file=sys.stderr)
        return 1
    print(f"web-timeline-source-closure: OK ({len(expected)} production TUs in both lanes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

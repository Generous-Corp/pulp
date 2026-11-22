#!/usr/bin/env python3
"""Exercise the installed Pulp SDK with and without its optional Vellum component."""

from __future__ import annotations

import argparse
import subprocess
import tempfile
from pathlib import Path


CORE_PROJECT = """cmake_minimum_required(VERSION 3.24)
project(pulp_core_consumer LANGUAGES CXX)
find_package(Pulp CONFIG REQUIRED)
if(NOT TARGET Pulp::format)
  message(FATAL_ERROR "core Pulp target Pulp::format is missing")
endif()
if(TARGET Pulp::vellum OR TARGET Pulp::vellum_audio)
  message(FATAL_ERROR "Vellum targets leaked into the core consumer")
endif()
"""

VELLUM_PROJECT = """cmake_minimum_required(VERSION 3.24)
project(pulp_vellum_consumer LANGUAGES CXX)
find_package(Pulp CONFIG REQUIRED COMPONENTS vellum)
foreach(target IN ITEMS Pulp::vellum Pulp::vellum_audio)
  if(NOT TARGET ${target})
    message(FATAL_ERROR "${target} is missing from the Vellum component")
  endif()
endforeach()
add_library(vellum_probe OBJECT probe.cpp)
target_link_libraries(vellum_probe PRIVATE Pulp::vellum Pulp::vellum_audio)
"""


def configure(source: Path, build: Path, prefix: str) -> None:
    subprocess.run(
        ["cmake", "-S", str(source), "-B", str(build), "-DCMAKE_PREFIX_PATH=" + prefix],
        check=True,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pulp-prefix", type=Path, required=True)
    parser.add_argument("--vellum-prefix", type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="pulp-vellum-consumer-") as raw:
        root = Path(raw)
        core = root / "core"
        core.mkdir()
        (core / "CMakeLists.txt").write_text(CORE_PROJECT, encoding="utf-8")
        configure(core, root / "core-build", str(args.pulp_prefix))

        vellum = root / "vellum"
        vellum.mkdir()
        (vellum / "CMakeLists.txt").write_text(VELLUM_PROJECT, encoding="utf-8")
        (vellum / "probe.cpp").write_text(
            "#include <pulp/vellum/editor.hpp>\nint pulp_vellum_probe() { return 0; }\n",
            encoding="utf-8",
        )
        configure(
            vellum,
            root / "vellum-build",
            f"{args.pulp_prefix};{args.vellum_prefix}",
        )
        subprocess.run(
            ["cmake", "--build", str(root / "vellum-build"), "--target", "vellum_probe"],
            check=True,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

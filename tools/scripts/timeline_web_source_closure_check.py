#!/usr/bin/env python3
"""Keep the production WAM and WebCLAP timeline-engine closure exact."""

from __future__ import annotations

import argparse
import re
import tempfile
from pathlib import Path


MODULES = ("timebase", "timeline", "playback")
MANIFEST_RE = re.compile(
    r"\$\{ROOT\}/(core/(?:timebase|timeline|playback)/src/[A-Za-z0-9_./-]+\.cpp)"
)
CMAKE_SOURCE_RE = re.compile(r"(?<![A-Za-z0-9_./-])(src/[A-Za-z0-9_./-]+\.cpp)")


def expected_sources(root: Path) -> set[str]:
    result: set[str] = set()
    for module in MODULES:
        cmake = root / "core" / module / "CMakeLists.txt"
        if not cmake.is_file():
            continue
        for source in CMAKE_SOURCE_RE.findall(cmake.read_text(errors="replace")):
            result.add(f"core/{module}/{source}")
    return result


def verify(root: Path) -> list[str]:
    errors: list[str] = []
    manifest_path = root / "tools/cmake/PulpTimelineEngineWeb.cmake"
    if not manifest_path.is_file():
        return [f"missing shared web source manifest: {manifest_path}"]
    manifest_text = manifest_path.read_text(errors="replace")
    listed = MANIFEST_RE.findall(manifest_text)
    listed_set = set(listed)
    expected = expected_sources(root)
    for source in sorted(expected - listed_set):
        errors.append(f"shared web source manifest is missing {source}")
    for source in sorted(listed_set - expected):
        errors.append(f"shared web source manifest has stale source {source}")
    if len(listed) != len(listed_set):
        errors.append("shared web source manifest contains duplicate engine sources")

    for abi, variable in (("Wam", "_PULP_WAM_TIMELINE_SOURCES"),
                          ("Wclap", "_PULP_WCLAP_TIMELINE_SOURCES")):
        path = root / f"tools/cmake/Pulp{abi}.cmake"
        text = path.read_text(errors="replace") if path.is_file() else ""
        if "PulpTimelineEngineWeb.cmake" not in text:
            errors.append(f"Pulp{abi}.cmake does not include the shared engine manifest")
        if text.count("pulp_timeline_engine_web_sources(") != 1:
            errors.append(f"Pulp{abi}.cmake must resolve the shared engine manifest exactly once")
        if f"${{{variable}}}" not in text:
            errors.append(f"Pulp{abi}.cmake does not compile the shared engine sources")
        if "PULP_COMPILE_EXECUTOR_DISABLE_THREADS=1" not in text:
            errors.append(f"Pulp{abi}.cmake does not select the threadless compile executor")
    return errors


def selftest() -> int:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        sources = {
            "timebase": "compiled.cpp",
            "timeline": "model.cpp",
            "playback": "renderer.cpp",
        }
        for module, source in sources.items():
            path = root / "core" / module / "CMakeLists.txt"
            path.parent.mkdir(parents=True)
            path.write_text(f"add_library(pulp-{module} src/{source})\n")
        cmake = root / "tools/cmake"
        cmake.mkdir(parents=True)
        manifest_lines = "\n".join(
            f"        ${{ROOT}}/core/{module}/src/{source}"
            for module, source in sources.items()
        )
        (cmake / "PulpTimelineEngineWeb.cmake").write_text(
            "function(pulp_timeline_engine_web_sources ROOT OUT_SOURCES OUT_INCLUDES)\n"
            f"  set(_sources\n{manifest_lines}\n  )\nendfunction()\n"
        )
        for abi, variable in (("Wam", "_PULP_WAM_TIMELINE_SOURCES"),
                              ("Wclap", "_PULP_WCLAP_TIMELINE_SOURCES")):
            (cmake / f"Pulp{abi}.cmake").write_text(
                "include(PulpTimelineEngineWeb.cmake)\n"
                f"pulp_timeline_engine_web_sources(root {variable} includes)\n"
                f"set(core ${{{variable}}})\n"
                "target_compile_definitions(x PRIVATE "
                "PULP_COMPILE_EXECUTOR_DISABLE_THREADS=1)\n"
            )
        if verify(root):
            print("selftest valid fixture was rejected")
            return 1
        manifest = cmake / "PulpTimelineEngineWeb.cmake"
        manifest.write_text(manifest.read_text().replace(
            "        ${ROOT}/core/playback/src/renderer.cpp\n", ""))
        if not any("missing core/playback/src/renderer.cpp" in error
                   for error in verify(root)):
            print("selftest missed a source-closure omission")
            return 1
    print("timeline_web_source_closure_selftest=true")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path,
                        default=Path(__file__).resolve().parents[2])
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    if args.selftest:
        return selftest()
    errors = verify(args.repo_root.resolve())
    if errors:
        print("\n".join(errors))
        return 1
    print("timeline_web_source_closure_verified=true")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

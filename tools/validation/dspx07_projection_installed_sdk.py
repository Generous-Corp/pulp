#!/usr/bin/env python3
"""Exercise the DSPX-07 projection API from an installed Pulp SDK.

This is deliberately a downstream consumer: it compiles a temporary source
with only ``<pulp/format/projection_capability.hpp>`` from ``--sdk`` and runs
the same native/browser positive and typed-negative matrix as the in-tree gate.
It never adds the Pulp source checkout to the compiler search path.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def run(sdk: Path, output: Path) -> dict[str, object]:
    sdk = sdk.expanduser().resolve()
    include = sdk / "include"
    header = include / "pulp/format/projection_capability.hpp"
    config = sdk / "lib/cmake/Pulp/PulpConfig.cmake"
    require(header.is_file(), f"installed DSPX-07 header is missing: {header}")
    require(config.is_file(), f"installed Pulp config is missing: {config}")

    compiler = os.environ.get("CXX") or shutil.which("c++")
    require(compiler is not None, "no CXX compiler is available")
    with tempfile.TemporaryDirectory(prefix="pulp-dspx07-sdk-consumer-") as temp:
        root = Path(temp)
        source = root / "consumer.cpp"
        binary = root / "consumer"
        source.write_text(
            "#include <pulp/format/projection_capability.hpp>\n"
            "#include <array>\n"
            "#include <string_view>\n"
            "int main() {\n"
            "  using namespace pulp::format;\n"
            "  constexpr std::array supported = {ProjectionSurface::clap, ProjectionSurface::vst3,\n"
            "      ProjectionSurface::lv2, ProjectionSurface::wam, ProjectionSurface::wclap};\n"
            "  for (auto surface : supported) {\n"
            "    auto result = projection_capability(surface, true, true);\n"
            "    if (!result.supported() || !result.reason.empty()) return 10;\n"
            "  }\n"
            "  auto graph_only = projection_capability(ProjectionSurface::clap, false, true);\n"
            "  if (graph_only.supported() || graph_only.reason !=\n"
            "      std::string_view{\"graph-only descriptor has no baked Processor projection\"}) return 11;\n"
            "  auto unbounded = projection_capability(ProjectionSurface::wclap, true, false);\n"
            "  if (unbounded.supported() || unbounded.reason !=\n"
            "      std::string_view{\"descriptor bounds are missing or exceed adapter limits\"}) return 12;\n"
            "  auto au = projection_capability(ProjectionSurface::au, true, true);\n"
            "  if (au.supported()) return 13;\n"
            "  return 0;\n"
            "}\n",
            encoding="utf-8",
        )
        compile = subprocess.run(
            [compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(include),
             str(source), "-o", str(binary)],
            text=True,
            capture_output=True,
            encoding="utf-8",
            check=False,
        )
        require(
            compile.returncode == 0,
            f"installed SDK consumer compile failed:\n{compile.stdout}{compile.stderr}",
        )
        execute = subprocess.run([str(binary)], text=True, capture_output=True, encoding="utf-8", check=False)
        require(
            execute.returncode == 0,
            f"installed SDK consumer returned {execute.returncode}: {execute.stdout}{execute.stderr}",
        )

    provenance = sdk / "sdk-provenance.json"
    receipt: dict[str, object] = {
        "schema": "pulp.dspx07.installed-projection-receipt.v1",
        "status": "passed",
        "sdk_root": str(sdk),
        "header": "include/pulp/format/projection_capability.hpp",
        "header_sha256": sha256(header),
        "config": "lib/cmake/Pulp/PulpConfig.cmake",
        "compiler": compiler,
        "positive_surfaces": ["clap", "vst3", "lv2", "wam", "wclap"],
        "typed_negative_surfaces": ["graph_only", "unbounded", "au"],
    }
    if provenance.is_file():
        receipt["sdk_provenance_sha256"] = sha256(provenance)
        try:
            receipt["sdk_source_git_sha"] = json.loads(provenance.read_text(encoding="utf-8"))["source_git_sha"]
        except (KeyError, json.JSONDecodeError):
            raise RuntimeError("sdk-provenance.json is malformed or omits source_git_sha")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return receipt


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, required=True, help="installed Pulp SDK root")
    parser.add_argument("--output", type=Path, required=True, help="JSON receipt path")
    args = parser.parse_args(argv)
    try:
        receipt = run(args.sdk, args.output)
    except RuntimeError as error:
        print(f"dspx07 installed SDK consumer: FAIL: {error}", file=sys.stderr)
        return 1
    print(f"dspx07 installed SDK consumer: PASS ({receipt['header_sha256']})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

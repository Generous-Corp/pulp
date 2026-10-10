#!/usr/bin/env python3
"""Check the GPU device layer's link and include closure.

The GPU device layer is two targets below the 2D renderer:

  pulp-gpu-device-api  INTERFACE target over the Dawn-free device headers.
  pulp-gpu-device      the Dawn surface/compute implementation.

Their value is what they do NOT reach. pulp-gpu-audio links the device without
the 2D renderer, and canvas, the renderer and the view layer all sit above the
API. A link or include that crosses back up the stack still builds, because
every target here is a static archive and the renderer is usually in the final
link anyway, so nothing else in the tree fails when the layering breaks. This
check reads the configured link properties and the sources themselves.

Input is a properties file written at configure time by
test/cmake/render_gpu_device_closure_tests.cmake: one `name<TAB>value` record
per line, values as CMake stored them (`;`-separated, generator expressions
unevaluated). Reading the unevaluated property is deliberate: a PRIVATE link on
a static library is recorded as `$<LINK_ONLY:dep>`, and the check needs to see
that wrapper to tell PRIVATE from PUBLIC.

Exit codes: 0 every rule holds, 1 a rule is violated (each violation printed
with a stable code), 2 the input could not be read or is incomplete.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# The headers pulp-gpu-device-api owns, relative to core/render/include.
API_HEADERS = frozenset({
    "pulp/render/gpu_surface.hpp",
    "pulp/render/gpu_compute.hpp",
    "pulp/render/gpu_diagnostics.hpp",
    "pulp/render/gpu_startup_report.hpp",
    "pulp/render/gpu_render_time.hpp",
    "pulp/render/bench/perf_counters.hpp",
})

# Pulp modules the device implementation may link. Anything else named pulp::
# or pulp- is a module above or beside the device layer.
DEVICE_ALLOWED_PULP_LINKS = frozenset({"gpu-device-api", "runtime"})

# Pulp include roots a device implementation source may use besides the API.
DEVICE_ALLOWED_INCLUDE_ROOTS = frozenset({"runtime"})

REQUIRED_KEYS = (
    "api.type",
    "api.interface_link",
    "device.type",
    "device.link",
    "device.sources",
    "device.source_dir",
    "render.link",
    "render.interface_link",
    "gpu_audio.link",
    "gpu_audio.source_dir",
    "api_consumer.link",
    "device_consumer.link",
)

PULP_LINK_RE = re.compile(r"\bpulp(?:::|-)([A-Za-z0-9_-]+)")
INCLUDE_RE = re.compile(r'^\s*#\s*(?:include|import)\s*[<"]([^>"]+)[>"]', re.MULTILINE)
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".mm", ".m"}
GPU_PROVIDER_RE = re.compile(r"(?i)(skia|dawn|webgpu)")


class Violation:
    def __init__(self, code: str, detail: str) -> None:
        self.code = code
        self.detail = detail

    def __str__(self) -> str:
        return f"{self.code}: {self.detail}"


def parse_properties(text: str) -> dict[str, str]:
    properties: dict[str, str] = {}
    for line in text.splitlines():
        if not line.strip():
            continue
        name, _, value = line.partition("\t")
        value = value.strip()
        if value.endswith("-NOTFOUND"):
            value = ""
        properties[name.strip()] = value
    return properties


def split_list(value: str) -> list[str]:
    """Split a CMake list, keeping `;` inside generator expressions intact."""
    items: list[str] = []
    depth = 0
    current: list[str] = []
    index = 0
    while index < len(value):
        if value.startswith("$<", index):
            depth += 1
            current.append("$<")
            index += 2
            continue
        character = value[index]
        if character == ">" and depth > 0:
            depth -= 1
        if character == ";" and depth == 0:
            if current:
                items.append("".join(current))
            current = []
        else:
            current.append(character)
        index += 1
    if current:
        items.append("".join(current))
    return [item for item in items if item]


def pulp_modules(entries: list[str]) -> set[str]:
    modules: set[str] = set()
    for entry in entries:
        modules.update(PULP_LINK_RE.findall(entry))
    return modules


def is_link_only(entry: str, module: str) -> bool:
    return bool(re.fullmatch(rf"\$<LINK_ONLY:pulp(?:::|-){re.escape(module)}>", entry))


def plain_names(entries: list[str], module: str) -> bool:
    return any(re.fullmatch(rf"pulp(?:::|-){re.escape(module)}", entry) for entry in entries)


def check_links(props: dict[str, str]) -> list[Violation]:
    violations: list[Violation] = []

    if props["api.type"] != "INTERFACE_LIBRARY":
        violations.append(Violation(
            "api-not-interface",
            f"pulp-gpu-device-api must be an INTERFACE library, got {props['api.type']!r}"))
    api_links = split_list(props["api.interface_link"])
    if api_links:
        violations.append(Violation(
            "api-links-something",
            "pulp-gpu-device-api must name no link dependency (its headers are "
            f"Dawn-free and module-free), got {api_links}"))

    device_links = split_list(props["device.link"])
    if not plain_names(device_links, "gpu-device-api"):
        violations.append(Violation(
            "device-missing-api",
            f"pulp-gpu-device must link pulp::gpu-device-api, got {device_links}"))
    escaped = sorted(pulp_modules(device_links) - DEVICE_ALLOWED_PULP_LINKS)
    if escaped:
        violations.append(Violation(
            "device-reaches-up",
            f"pulp-gpu-device links Pulp modules outside {sorted(DEVICE_ALLOWED_PULP_LINKS)}: "
            f"{escaped}"))

    render_links = split_list(props["render.link"])
    for module in ("gpu-device-api", "gpu-device"):
        if not plain_names(render_links, module):
            violations.append(Violation(
                "render-missing-device",
                f"pulp-render must link pulp::{module}, got {render_links}"))
    render_interface = split_list(props["render.interface_link"])
    if not plain_names(render_interface, "gpu-device-api"):
        violations.append(Violation(
            "render-api-not-public",
            "pulp-render must expose pulp::gpu-device-api in its link interface"))
    if plain_names(render_interface, "gpu-device"):
        violations.append(Violation(
            "render-device-public",
            "pulp-render must link pulp::gpu-device PRIVATE (seen PUBLIC in its interface)"))
    elif not any(is_link_only(entry, "gpu-device") for entry in render_interface):
        violations.append(Violation(
            "render-device-unlinked",
            "pulp-render's interface carries no $<LINK_ONLY:pulp::gpu-device>; "
            "the device objects would be missing from a renderer consumer's link"))

    gpu_audio_links = split_list(props["gpu_audio.link"])
    gpu_audio_modules = pulp_modules(gpu_audio_links)
    if "render" in gpu_audio_modules:
        violations.append(Violation(
            "gpu-audio-reaches-render",
            "pulp-gpu-audio must link pulp::gpu-device, not the 2D renderer pulp::render"))
    if props.get("gpu_audio.expects_device") == "1" and "gpu-device" not in gpu_audio_modules:
        violations.append(Violation(
            "gpu-audio-missing-device",
            f"pulp-gpu-audio's GPU path must link pulp::gpu-device, got {gpu_audio_links}"))

    for key, module in (("api_consumer.link", "gpu-device-api"),
                        ("device_consumer.link", "gpu-device")):
        links = split_list(props[key])
        if pulp_modules(links) != {module}:
            violations.append(Violation(
                "consumer-not-isolated",
                f"{key.split('.')[0]} must link pulp::{module} and nothing else from Pulp; "
                f"anything more lets the link succeed for the wrong reason: {links}"))
    return violations


def pulp_includes(path: Path) -> list[str]:
    text = path.read_text(encoding="utf-8", errors="replace")
    return [include for include in INCLUDE_RE.findall(text) if include.startswith("pulp/")]


def check_includes(props: dict[str, str], render_include: Path) -> list[Violation]:
    violations: list[Violation] = []

    for header in sorted(API_HEADERS):
        path = render_include / header
        if not path.is_file():
            violations.append(Violation("api-header-missing", f"{path} does not exist"))
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for include in INCLUDE_RE.findall(text):
            if include.startswith("pulp/") and include not in API_HEADERS:
                violations.append(Violation(
                    "api-header-reaches-out",
                    f"{header} includes {include}, which is not a device API header"))
            elif GPU_PROVIDER_RE.search(include):
                violations.append(Violation(
                    "api-header-names-provider",
                    f"{header} includes {include}; device API headers stay Dawn-, "
                    "WebGPU- and Skia-free"))

    device_dir = Path(props["device.source_dir"])
    for source in split_list(props["device.sources"]):
        path = Path(source)
        if not path.is_absolute():
            path = device_dir / path
        if path.suffix not in SOURCE_SUFFIXES or not path.is_file():
            continue
        for include in pulp_includes(path):
            root = include.split("/")[1] if include.count("/") >= 1 else include
            if include in API_HEADERS or root in DEVICE_ALLOWED_INCLUDE_ROOTS:
                continue
            violations.append(Violation(
                "device-source-reaches-up",
                f"{path.name} includes {include}; the device implementation may use "
                "only the device API and pulp/runtime"))

    gpu_audio_dir = Path(props["gpu_audio.source_dir"])
    for path in sorted(gpu_audio_dir.rglob("*")):
        if path.suffix not in SOURCE_SUFFIXES or not path.is_file():
            continue
        for include in pulp_includes(path):
            if include.startswith("pulp/render/") and include not in API_HEADERS:
                violations.append(Violation(
                    "gpu-audio-includes-renderer",
                    f"{path.relative_to(gpu_audio_dir)} includes {include}; GPU audio may "
                    "use only the device API from pulp/render"))
    return violations


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--properties", required=True, type=Path,
                        help="configure-time properties file")
    parser.add_argument("--render-include", required=True, type=Path,
                        help="core/render/include directory")
    args = parser.parse_args(argv)

    try:
        props = parse_properties(args.properties.read_text(encoding="utf-8"))
    except OSError as error:
        print(f"gpu-device-link-closure: INCONCLUSIVE: {error}", file=sys.stderr)
        return 2
    missing = [key for key in REQUIRED_KEYS if key not in props]
    if missing:
        print(f"gpu-device-link-closure: INCONCLUSIVE: properties file lacks {missing}",
              file=sys.stderr)
        return 2
    if not args.render_include.is_dir():
        print(f"gpu-device-link-closure: INCONCLUSIVE: {args.render_include} is not a directory",
              file=sys.stderr)
        return 2

    violations = check_links(props) + check_includes(props, args.render_include)
    if violations:
        for violation in violations:
            print(f"gpu-device-link-closure: FAIL {violation}", file=sys.stderr)
        return 1
    device_sources = len(split_list(props["device.sources"]))
    print(f"gpu-device-link-closure: PASS ({len(API_HEADERS)} API headers, "
          f"{device_sources} device sources, gpu-audio links "
          f"{sorted(pulp_modules(split_list(props['gpu_audio.link'])))})")
    return 0


if __name__ == "__main__":
    sys.exit(main())

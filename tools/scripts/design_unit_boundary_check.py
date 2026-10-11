#!/usr/bin/env python3
"""Link-closure and include gate for the design units.

The design subsystem is split into three targets (core/view/cmake/PulpDesignUnits.cmake):

  pulp-design-ir                 header-only DesignIR value types, BELOW view-core
  pulp-design-import             importers and codegen, ABOVE view-core
  pulp-design-import-audio-pack  the Pulp-owned built-in audio-control recognition pack

The files did not move, so nothing about the directory layout separates the
units: the only record of the boundary is each target's file list and its link
edges. This gate reads both from the configured build, not from the CMake text,
because a PRIVATE link to a static library still reaches the consumer and only
the resolved target properties say so.

Configure writes the manifest (test/cmake/design_import_tests.cmake):

    <target>\t<field>\t<;-separated values>

with fields type, source_dir, sources, links, interface_links, headers.

The gate asserts:

  * design-ir links no Pulp target and its headers include only the standard
    library and other design-ir headers;
  * view-core links design-ir, never design-import or the audio pack, compiles
    none of their sources, and no view-core source or generic public header
    includes a design-import or audio-pack header;
  * design-import's direct links name no audio, DSP, host, MIDI, format or
    content target and not the audio pack, and none of its sources or headers
    includes an audio/signal/host/midi/format/content header or the audio pack.

The transitive closure of design-import still reaches signal and audio through
view-core's own PUBLIC links; that edge belongs to view-core and is checked by
the view-core audio-free gate, not here. This gate holds the edges the design
units themselves declare.

Exit status: 0 pass, 1 boundary violation, 2 unusable input.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

DESIGN_IR = "pulp-design-ir"
DESIGN_IMPORT = "pulp-design-import"
AUDIO_PACK = "pulp-design-import-audio-pack"
VIEW_CORE = "pulp-view-core"

# Product modules no extracted design unit may name, by target stem
# (`pulp-<stem>` / `pulp::<stem>`).
FORBIDDEN_LINK_STEMS = {
    "audio",
    "signal",
    "host",
    "midi",
    "format",
    "format-core",
    "format-view",
    "content",
    "sample-bank-manifest",
    "gpu-audio",
}
FORBIDDEN_INCLUDE = re.compile(r"^pulp/(audio|signal|host|midi|format|content)/")
INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]')
PUBLIC_HEADER_ROOT = "include"


@dataclass
class Target:
    name: str
    type: str = ""
    source_dir: str = ""
    sources: list[str] = field(default_factory=list)
    links: list[str] = field(default_factory=list)
    interface_links: list[str] = field(default_factory=list)
    headers: list[str] = field(default_factory=list)


def _split(value: str) -> list[str]:
    return [item for item in value.split(";") if item.strip()]


def parse_manifest(text: str) -> dict[str, Target]:
    targets: dict[str, Target] = {}
    for lineno, line in enumerate(text.splitlines(), 1):
        if not line.strip():
            continue
        parts = line.split("\t")
        if len(parts) != 3:
            raise ValueError(f"manifest line {lineno}: expected 3 tab-separated fields")
        name, key, value = parts
        target = targets.setdefault(name, Target(name))
        if key in ("type", "source_dir"):
            setattr(target, key, value)
        elif key in ("sources", "links", "interface_links", "headers"):
            setattr(target, key, _split(value))
        else:
            raise ValueError(f"manifest line {lineno}: unknown field {key!r}")
    return targets


TARGET_REF = re.compile(r"(?:pulp::|Pulp::|(?<![A-Za-z0-9_/.])pulp-)([A-Za-z0-9_][A-Za-z0-9_+.-]*)")


def link_stems(items: list[str]) -> set[str]:
    """Pulp target stems named by link items: `pulp::audio` / `pulp-audio` -> `audio`.

    Items are read raw, so a stem wrapped in a generator expression
    (`$<BUILD_INTERFACE:pulp::audio>`, `$<LINK_ONLY:pulp-audio>`) still counts.
    """
    stems: set[str] = set()
    for item in items:
        stems.update(TARGET_REF.findall(item))
    return stems


def resolve(target: Target, rel: str) -> Path:
    path = Path(rel)
    return path if path.is_absolute() else Path(target.source_dir) / path


def includes(path: Path) -> list[tuple[str, str]]:
    found = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = INCLUDE.match(line)
        if match:
            found.append((match.group(1), match.group(2)))
    return found


def with_private_headers(files: list[Path]) -> list[Path]:
    """`files` plus every quoted include they reach, transitively.

    A source's private headers (src/*.hpp) are not in SOURCES, but an include
    they carry is the source's include all the same.
    """
    seen: set[Path] = set()
    ordered: list[Path] = []
    pending = list(files)
    while pending:
        path = pending.pop()
        key = path.resolve() if path.exists() else path
        if key in seen:
            continue
        seen.add(key)
        ordered.append(path)
        if not path.is_file():
            continue
        for delim, inc in includes(path):
            if delim == '"':
                candidate = path.parent / inc
                if candidate.is_file():
                    pending.append(candidate)
    return ordered


def public_name(target: Target, rel: str) -> str:
    """include/pulp/view/x.hpp -> pulp/view/x.hpp"""
    parts = Path(rel).parts
    if parts and parts[0] == PUBLIC_HEADER_ROOT:
        return "/".join(parts[1:])
    return rel


def header_names(target: Target | None) -> set[str]:
    if target is None:
        return set()
    return {public_name(target, h) for h in target.headers}


def check(targets: dict[str, Target]) -> list[str]:
    errors: list[str] = []

    for required in (DESIGN_IR, VIEW_CORE):
        if required not in targets:
            raise ValueError(f"manifest has no {required} entry")
    ir = targets[DESIGN_IR]
    core = targets[VIEW_CORE]
    imp = targets.get(DESIGN_IMPORT)
    pack = targets.get(AUDIO_PACK)
    if (imp is None) != (pack is None):
        raise ValueError("manifest names one of design-import / audio-pack without the other")

    ir_headers = header_names(ir)
    imp_headers = header_names(imp)
    pack_headers = header_names(pack)
    if not ir.headers:
        raise ValueError(f"{DESIGN_IR} declares no PULP_UNIT_HEADERS")
    for unit in (u for u in (ir, imp, pack) if u is not None):
        for rel in unit.headers:
            if not resolve(unit, rel).is_file():
                errors.append(f"{unit.name}: manifest header {rel} does not exist")

    # design-ir: a leaf.
    for stem in sorted(link_stems(ir.interface_links + ir.links)):
        errors.append(f"{DESIGN_IR}: links pulp-{stem}; the value-type unit must link no Pulp target")
    for rel in ir.headers:
        path = resolve(ir, rel)
        if not path.is_file():
            continue
        for delim, inc in includes(path):
            if delim == '"':
                errors.append(f"{rel}: quoted include {inc!r}; design-ir headers include by public name only")
            elif inc.startswith("pulp/") and inc not in ir_headers:
                errors.append(f"{rel}: includes <{inc}>, which is outside {DESIGN_IR}")

    # view-core: links design-ir, never the importer or the pack.
    core_link_stems = link_stems(core.links + core.interface_links)
    if "design-ir" not in core_link_stems:
        errors.append(f"{VIEW_CORE}: does not link {DESIGN_IR}")
    for stem in ("design-import", "design-import-audio-pack"):
        if stem in core_link_stems:
            errors.append(f"{VIEW_CORE}: links pulp-{stem}; view-core sits below the importer")
    if imp is not None:
        core_sources = {resolve(core, s).resolve() for s in core.sources}
        for unit in (imp, pack):
            for s in unit.sources:
                if resolve(unit, s).resolve() in core_sources:
                    errors.append(f"{VIEW_CORE}: also compiles {unit.name} source {s}")

    upper_headers = imp_headers | pack_headers
    if upper_headers:
        core_files = [resolve(core, s) for s in core.sources]
        # Generic public headers: every view-core-owned header in the shared
        # include tree that is not in a design-unit manifest.
        include_root = Path(core.source_dir) / PUBLIC_HEADER_ROOT
        unit_headers = ir_headers | upper_headers
        if include_root.is_dir():
            for path in sorted(include_root.rglob("*.hpp")):
                name = path.relative_to(include_root).as_posix()
                if name not in unit_headers:
                    core_files.append(path)
        for path in with_private_headers(core_files):
            if path.suffix not in (".cpp", ".mm", ".hpp", ".h", ".c") or not path.is_file():
                continue
            for _, inc in includes(path):
                if inc in upper_headers:
                    errors.append(
                        f"{path}: includes <{inc}> from {DESIGN_IMPORT if inc in imp_headers else AUDIO_PACK}; "
                        f"generic view code may include design-ir headers only")

    # design-import: no product edges, no pack.
    if imp is not None:
        imp_link_stems = link_stems(imp.links + imp.interface_links)
        for stem in sorted(s for s in imp_link_stems if s in FORBIDDEN_LINK_STEMS):
            errors.append(f"{DESIGN_IMPORT}: links pulp-{stem}; the importer must not reach audio/DSP/host/MIDI/format/content")
        if "design-import-audio-pack" in imp_link_stems:
            errors.append(f"{DESIGN_IMPORT}: links the audio-control pack; the pack sits above the importer")
        for stem in ("design-ir", "view-core"):
            if stem not in imp_link_stems:
                errors.append(f"{DESIGN_IMPORT}: does not link pulp-{stem}")
        files = [resolve(imp, s) for s in imp.sources] + [resolve(imp, h) for h in imp.headers]
        for path in files:
            if not path.is_file():
                continue
            for _, inc in includes(path):
                if FORBIDDEN_INCLUDE.match(inc):
                    errors.append(f"{path}: includes <{inc}>; the importer must not reach a product module")
                if inc in pack_headers:
                    errors.append(f"{path}: includes <{inc}>; the importer must not depend on the audio-control pack")

    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--manifest", required=True, type=Path,
                        help="configure-generated design unit manifest")
    args = parser.parse_args(argv)
    try:
        targets = parse_manifest(args.manifest.read_text(encoding="utf-8"))
        errors = check(targets)
    except (OSError, ValueError) as exc:
        print(f"design-unit-boundary: INCONCLUSIVE: {exc}", file=sys.stderr)
        return 2
    if errors:
        for error in errors:
            print(f"design-unit-boundary: FAIL: {error}", file=sys.stderr)
        return 1
    units = [n for n in (DESIGN_IR, DESIGN_IMPORT, AUDIO_PACK) if n in targets]
    checked = sum(len(targets[n].sources) + len(targets[n].headers) for n in units)
    print(f"design-unit-boundary: PASS ({', '.join(units)}; {checked} unit files checked)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

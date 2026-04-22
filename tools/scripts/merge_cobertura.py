#!/usr/bin/env python3
"""Merge N Cobertura XML reports into a single XML.

Motivation
==========
Pulp's CI produces one Cobertura XML per OS (Linux / macOS / Windows)
because llvm-profdata merge is not architecture-portable. The
`coverage-diff-gate` workflow historically passed only the Linux XML
to diff-cover, which silently skipped platform-specific source files
(e.g. ``core/format/src/au_adapter.mm``, ``core/format/src/au_v2_*``,
any Apple-only ``.mm`` / Windows-only ``*_win.cpp``) because those
files never appear in the Linux build. PRs touching platform-specific
code therefore bypassed the 75% diff-cover gate entirely. See
pulp issue #635.

This script merges N Cobertura XMLs into one. For every ``(filename,
line_number)`` key that appears in any input XML, the merged output
reports ``max(hits)`` across the inputs. Files that only exist in one
XML (e.g. au_adapter.mm on macOS) carry through unchanged. Files that
exist in multiple XMLs with different hit counts (cross-platform code
measured on all three OSes) get the UNION — a line covered on any OS
counts as covered overall.

This is exactly the semantic diff-cover needs: "of the lines this PR
adds, how many are covered *somewhere*?" Cross-OS unions happen here,
once, before diff-cover runs; diff-cover itself remains a single-XML
tool and its output remains one comment per PR.

Usage
=====

    python3 merge_cobertura.py \\
        --out merged.cobertura.xml \\
        linux.cobertura.xml macos.cobertura.xml windows.cobertura.xml

Missing inputs are skipped with a warning on stderr (the merged XML
still writes; diff-cover needs *something* to compare against). If
every input is missing, the script exits non-zero.

The output XML is a minimal Cobertura document: one synthetic
``<package>``, all ``<class>`` entries flattened into it, each with a
``<lines>`` list containing the union of lines from every input. The
top-level ``lines-valid`` / ``lines-covered`` attributes are recomputed
from the merged line set; everything else (branch-rate per class,
methods) is passed through from the first input where a given file
appears — merging those fields across XMLs is not meaningful for
diff-cover, which reads only line-hit data.
"""

from __future__ import annotations

import argparse
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


def parse_xml(path: Path) -> dict[str, dict[int, int]]:
    """Return ``{filename: {line_number: hits}}`` from one Cobertura XML.

    Ignores branch data, methods, and timestamps — diff-cover's line
    coverage only reads ``<line number=... hits=...>``. Multiple
    ``<class>`` entries for the same filename (possible when a single
    source is compiled into multiple TUs) are accumulated via
    ``max()`` so we never regress a hit to 0.
    """
    if not path.is_file():
        return {}
    tree = ET.parse(str(path))
    root = tree.getroot()
    out: dict[str, dict[int, int]] = {}
    for cls in root.iter("class"):
        filename = cls.get("filename")
        if not filename:
            continue
        per_line = out.setdefault(filename, {})
        lines_el = cls.find("lines")
        if lines_el is None:
            continue
        for line in lines_el.findall("line"):
            n_raw = line.get("number")
            h_raw = line.get("hits")
            if n_raw is None or h_raw is None:
                continue
            try:
                n = int(n_raw)
                h = int(h_raw)
            except ValueError:
                continue
            prev = per_line.get(n, 0)
            if h > prev:
                per_line[n] = h
            elif n not in per_line:
                per_line[n] = h
    return out


def merge(inputs: list[dict[str, dict[int, int]]]) -> dict[str, dict[int, int]]:
    """Union the per-file, per-line maps, taking ``max`` on hit count."""
    out: dict[str, dict[int, int]] = {}
    for one in inputs:
        for filename, per_line in one.items():
            target = out.setdefault(filename, {})
            for n, h in per_line.items():
                prev = target.get(n)
                if prev is None or h > prev:
                    target[n] = h
    return out


def render(merged: dict[str, dict[int, int]]) -> ET.ElementTree:
    """Build a minimal Cobertura document from the merged line map."""
    total_lines = 0
    covered_lines = 0
    for per_line in merged.values():
        total_lines += len(per_line)
        covered_lines += sum(1 for h in per_line.values() if h > 0)
    line_rate = (covered_lines / total_lines) if total_lines else 0.0

    coverage = ET.Element(
        "coverage",
        attrib={
            "line-rate": f"{line_rate:.4f}",
            "branch-rate": "0",
            "lines-covered": str(covered_lines),
            "lines-valid": str(total_lines),
            "branches-covered": "0",
            "branches-valid": "0",
            "complexity": "0",
            "version": "pulp-merged",
            "timestamp": "0",
        },
    )
    ET.SubElement(coverage, "sources")  # empty; diff-cover tolerates it
    packages = ET.SubElement(coverage, "packages")
    pkg = ET.SubElement(
        packages,
        "package",
        attrib={
            "name": "merged",
            "line-rate": f"{line_rate:.4f}",
            "branch-rate": "0",
            "complexity": "0",
        },
    )
    classes = ET.SubElement(pkg, "classes")

    for filename in sorted(merged.keys()):
        per_line = merged[filename]
        file_total = len(per_line)
        file_covered = sum(1 for h in per_line.values() if h > 0)
        file_rate = (file_covered / file_total) if file_total else 0.0
        cls = ET.SubElement(
            classes,
            "class",
            attrib={
                "name": filename.replace("/", "."),
                "filename": filename,
                "line-rate": f"{file_rate:.4f}",
                "branch-rate": "0",
                "complexity": "0",
            },
        )
        ET.SubElement(cls, "methods")
        lines_el = ET.SubElement(cls, "lines")
        for n in sorted(per_line):
            ET.SubElement(
                lines_el,
                "line",
                attrib={"number": str(n), "hits": str(per_line[n]), "branch": "false"},
            )

    return ET.ElementTree(coverage)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0] if __doc__ else "")
    ap.add_argument("--out", required=True, help="Output merged XML path.")
    ap.add_argument("inputs", nargs="+", help="Input Cobertura XML paths.")
    args = ap.parse_args(argv)

    parsed: list[dict[str, dict[int, int]]] = []
    missing: list[str] = []
    for p in args.inputs:
        path = Path(p)
        data = parse_xml(path)
        if not data:
            missing.append(p)
            continue
        parsed.append(data)

    if missing:
        for p in missing:
            print(f"merge_cobertura: skipped (missing or empty): {p}", file=sys.stderr)

    if not parsed:
        print("merge_cobertura: no valid input XMLs — refusing to write empty merge.", file=sys.stderr)
        return 1

    merged = merge(parsed)
    tree = render(merged)
    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    # Use write() with encoding so the XML carries a declaration and
    # consumers that sniff it (diff-cover included) don't complain.
    tree.write(str(out_path), encoding="utf-8", xml_declaration=True)

    total_files = len(merged)
    total_lines = sum(len(pl) for pl in merged.values())
    covered = sum(sum(1 for h in pl.values() if h > 0) for pl in merged.values())
    print(
        f"merge_cobertura: merged {len(parsed)} XML(s) -> {out_path} "
        f"({total_files} files, {covered}/{total_lines} lines covered)",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())

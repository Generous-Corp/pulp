#!/usr/bin/env python3
"""Check durable API contracts for the installed Timeline public surface."""

from __future__ import annotations

import argparse
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


PUBLIC_COMPOUND_KINDS = {"class", "struct", "union"}
EXPECTED_PUBLIC_TYPES = {
    "pulp::timeline::DocumentSession",
    "pulp::timeline::ParsedJson",
    "pulp::timeline::Project",
    "pulp::timeline::Transaction",
}
def text(element: ET.Element | None) -> str:
    if element is None:
        return ""
    return " ".join("".join(element.itertext()).split())


def documented(element: ET.Element) -> bool:
    return bool(text(element.find("briefdescription")) or text(element.find("detaileddescription")))


def location(member: ET.Element) -> tuple[str, int]:
    node = member.find("location")
    if node is None:
        return ("<unknown>", 0)
    return (node.get("file", "<unknown>"), int(node.get("line", "0")))


def is_timeline_public_file(path: str) -> bool:
    normalized = path.replace("\\", "/")
    return "/core/timeline/include/pulp/timeline/" in normalized


def is_internal(name: str) -> bool:
    return "::detail::" in name or "::follow_detail::" in name


def is_exempt_function(member: ET.Element) -> bool:
    """Return true only when another durable contract fully describes the callable."""
    name = text(member.find("name"))
    qualified = text(member.find("qualifiedname"))
    args = text(member.find("argsstring"))
    return_type = text(member.find("type"))
    if is_internal(qualified):
        return True
    if "->" in args and not return_type:
        # Class-template argument deduction guides inherit the class contract.
        return True
    if "=delete" in args or "=default" in args:
        return True
    if name.startswith("~"):
        return True

    return False


def check(xml_dir: Path) -> list[tuple[str, int, str]]:
    findings: list[tuple[str, int, str]] = []
    seen: set[tuple[str, int, str]] = set()
    public_types: set[str] = set()

    for xml_path in sorted(xml_dir.glob("*.xml")):
        try:
            root = ET.parse(xml_path).getroot()
        except ET.ParseError as error:
            findings.append((str(xml_path), 0, f"invalid Doxygen XML: {error}"))
            continue

        compound = root.find("compounddef")
        if compound is None:
            continue
        compound_name = text(compound.find("compoundname"))
        compound_kind = compound.get("kind", "")
        compound_file, compound_line = location(compound)

        if (
            compound_kind in PUBLIC_COMPOUND_KINDS
            and compound.get("prot") == "public"
            and compound_name.startswith("pulp::timeline::")
            and not is_internal(compound_name)
            and is_timeline_public_file(compound_file)
        ):
            public_types.add(compound_name)
            if not documented(compound):
                key = (compound_file, compound_line, compound_name)
                if key not in seen:
                    findings.append(
                        (
                            compound_file,
                            compound_line,
                            f"public type lacks an API contract: {compound_name}",
                        )
                    )
                    seen.add(key)

        for member in compound.findall(".//memberdef"):
            if member.get("kind") != "function" or member.get("prot") != "public":
                continue
            member_file, member_line = location(member)
            if not is_timeline_public_file(member_file):
                continue
            qualified = text(member.find("qualifiedname"))
            if not qualified.startswith("pulp::timeline::") or is_exempt_function(member):
                continue
            if documented(member):
                continue
            key = (member_file, member_line, qualified)
            if key not in seen:
                findings.append(
                    (member_file, member_line, f"public callable lacks an API contract: {qualified}")
                )
                seen.add(key)

    for missing in sorted(EXPECTED_PUBLIC_TYPES - public_types):
        findings.append(
            (
                "<timeline-api-docs-check>",
                0,
                f"expected representative public type is absent from Doxygen XML: {missing}",
            )
        )

    return sorted(findings)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("xml_dir", type=Path, help="Doxygen XML output directory")
    args = parser.parse_args()

    if not args.xml_dir.is_dir():
        parser.error(f"not a Doxygen XML directory: {args.xml_dir}")

    findings = check(args.xml_dir)
    if findings:
        for file, line, message in findings:
            print(f"{file}:{line}: error: {message}")
        print(f"Timeline API documentation check failed with {len(findings)} issue(s).")
        return 1

    print("Timeline API documentation contracts are complete.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

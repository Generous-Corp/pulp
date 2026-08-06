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


PUBLIC_HEADER = "/src/core/timeline/include/pulp/timeline/model.hpp"


def _compound_xml(
    name: str,
    *,
    documented_type: bool = True,
    members: list[dict] | None = None,
    file: str = PUBLIC_HEADER,
    kind: str = "class",
    prot: str = "public",
) -> str:
    """Render one Doxygen compounddef. Shape mirrors real Doxygen output."""
    brief = "<para>A contract.</para>" if documented_type else ""
    body = []
    for member in members or []:
        member_brief = "<para>A contract.</para>" if member.get("documented") else ""
        body.append(
            f'<memberdef kind="{member.get("kind", "function")}" '
            f'prot="{member.get("prot", "public")}">'
            f'<type>{member.get("type", "bool")}</type>'
            f'<name>{member["name"]}</name>'
            f'<qualifiedname>{member["qualified"]}</qualifiedname>'
            f'<argsstring>{member.get("args", "() const noexcept")}</argsstring>'
            f"<briefdescription>{member_brief}</briefdescription>"
            f"<detaileddescription/>"
            f'<location file="{member.get("file", file)}" line="20"/>'
            f"</memberdef>"
        )
    return (
        '<?xml version="1.0"?><doxygen>'
        f'<compounddef kind="{kind}" prot="{prot}" id="x">'
        f"<compoundname>{name}</compoundname>"
        f"<briefdescription>{brief}</briefdescription><detaileddescription/>"
        f'<location file="{file}" line="10"/>'
        f"<sectiondef>{''.join(body)}</sectiondef>"
        "</compounddef></doxygen>"
    )


def _fixture(tmp: Path, extra: list[str]) -> Path:
    """A tree that is clean by construction, plus whatever a case adds.

    The baseline supplies every EXPECTED_PUBLIC_TYPES entry documented, so a
    case's findings are attributable to that case rather than to the
    representative-type guard firing on an otherwise empty directory.
    """
    tmp.mkdir(parents=True, exist_ok=True)
    for index, name in enumerate(sorted(EXPECTED_PUBLIC_TYPES)):
        (tmp / f"base{index}.xml").write_text(_compound_xml(name))
    for index, xml in enumerate(extra):
        (tmp / f"case{index}.xml").write_text(xml)
    return tmp


def self_test() -> int:
    """Prove the checker flags what it must and stays quiet where it must.

    Both directions matter. A checker that flags nothing passes every build
    silently, and every exemption here (internal namespaces, destructors,
    defaulted and deleted members, deduction guides, the path filter) is a
    chance to over-match and become exactly that.
    """
    import tempfile

    def member(**overrides) -> dict:
        return {"name": "valid", "qualified": "pulp::timeline::Widget::valid", **overrides}

    cases: list[tuple[str, list[str], int]] = [
        ("clean tree reports nothing", [], 0),
        (
            "undocumented public callable is flagged",
            [_compound_xml("pulp::timeline::Widget", members=[member(documented=False)])],
            1,
        ),
        (
            "documented public callable is not flagged",
            [_compound_xml("pulp::timeline::Widget", members=[member(documented=True)])],
            0,
        ),
        (
            "undocumented public type is flagged",
            [_compound_xml("pulp::timeline::Widget", documented_type=False)],
            1,
        ),
        (
            "detail namespace is exempt",
            [
                _compound_xml(
                    "pulp::timeline::detail::Widget",
                    documented_type=False,
                    members=[
                        {
                            "name": "valid",
                            "qualified": "pulp::timeline::detail::Widget::valid",
                            "documented": False,
                        }
                    ],
                )
            ],
            0,
        ),
        (
            "destructor is exempt",
            [
                _compound_xml(
                    "pulp::timeline::Widget",
                    members=[
                        member(name="~Widget", qualified="pulp::timeline::Widget::~Widget",
                               documented=False)
                    ],
                )
            ],
            0,
        ),
        (
            "defaulted and deleted members are exempt",
            [
                _compound_xml(
                    "pulp::timeline::Widget",
                    members=[
                        member(documented=False, args="()=default"),
                        member(documented=False, args="()=delete"),
                    ],
                )
            ],
            0,
        ),
        (
            "a header outside the timeline public surface is ignored",
            [
                _compound_xml(
                    "pulp::timeline::Widget",
                    documented_type=False,
                    file="/src/core/host/src/signal_graph.cpp",
                )
            ],
            0,
        ),
        (
            "private members are ignored",
            [
                _compound_xml(
                    "pulp::timeline::Widget",
                    members=[member(documented=False, prot="private")],
                )
            ],
            0,
        ),
        (
            "a missing representative public type is flagged",
            [],
            1,
        ),
    ]

    failures = 0
    with tempfile.TemporaryDirectory() as root:
        for index, (label, extra, expected) in enumerate(cases):
            directory = Path(root) / f"case{index}"
            if label.startswith("a missing representative"):
                # Drop one baseline type so the EXPECTED_PUBLIC_TYPES guard is
                # the thing under test, not the per-symbol scan.
                directory.mkdir(parents=True)
                for offset, name in enumerate(sorted(EXPECTED_PUBLIC_TYPES)[1:]):
                    (directory / f"base{offset}.xml").write_text(_compound_xml(name))
            else:
                _fixture(directory, extra)
            found = len(check(directory))
            status = "ok" if found == expected else "FAIL"
            if found != expected:
                failures += 1
            print(f"  [{status}] {label}: expected {expected} finding(s), got {found}")

    if failures:
        print(f"timeline API docs checker self-test FAILED ({failures} case(s))")
        return 1
    print(f"timeline API docs checker self-test passed ({len(cases)} cases)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "xml_dir",
        type=Path,
        nargs="?",
        help="Doxygen XML output directory",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="run the checker's own controls and exit (needs no Doxygen)",
    )
    args = parser.parse_args()

    if args.self_test:
        return self_test()

    if args.xml_dir is None:
        parser.error("xml_dir is required unless --self-test is given")

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

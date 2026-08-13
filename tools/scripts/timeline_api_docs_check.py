#!/usr/bin/env python3
"""Ratchet durable API contracts across the public sequencer headers."""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import json
import os
import re
import shutil
import sys
import tempfile
import time
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path


PUBLIC_COMPOUND_KINDS = {"class", "struct", "union"}
PUBLIC_DECLARATION_KINDS = {"enum", "enumvalue", "typedef", "variable"}
PUBLIC_MEMBER_KINDS = PUBLIC_DECLARATION_KINDS - {"enumvalue"}
ROOTS = {
    "timeline": ("core/timeline/include", "pulp::timeline::"),
    "music": ("core/music/include", "pulp::music::"),
    "timeline_editor": ("core/timeline_editor/include", "pulp::timeline_editor::"),
    "timeline_view": ("core/timeline_view/include", "pulp::timeline_view::"),
}
LEGACY_ROOTS = frozenset({"music", "timeline_editor", "timeline_view"})
EXPECTED_PUBLIC_TYPES = {
    "timeline": {
        "pulp::timeline::DocumentSession",
        "pulp::timeline::ParsedJson",
        "pulp::timeline::Project",
        "pulp::timeline::Transaction",
    },
    "music": {"pulp::music::Scale"},
    "timeline_editor": {"pulp::timeline_editor::SequencerUiHost"},
    "timeline_view": {
        "pulp::timeline_view::ArrangerView",
        "pulp::timeline_view::PianoRollView",
    },
}
BASELINE_VERSION = 1


@dataclass(frozen=True, order=True)
class Debt:
    kind: str
    name: str
    parameters: str
    file: str

    def json_value(self) -> dict[str, str]:
        return {
            "kind": self.kind,
            "name": self.name,
            "parameters": self.parameters,
            "file": self.file,
        }


def text(element: ET.Element | None) -> str:
    if element is None:
        return ""
    return " ".join("".join(element.itertext()).split())


def documented(element: ET.Element) -> bool:
    return bool(text(element.find("briefdescription")) or text(element.find("detaileddescription")))


def location(element: ET.Element) -> tuple[str, int]:
    node = element.find("location")
    if node is None:
        return ("<unknown>", 0)
    try:
        line = int(node.get("line", "0"))
    except ValueError:
        line = 0
    return (node.get("file", "<unknown>"), line)


def relative_file(path: str) -> str:
    """Return a checkout-independent path for an XML source location."""
    normalized = path.replace("\\", "/")
    parts: list[str] = []
    for part in normalized.split("/"):
        if not part or part == ".":
            continue
        if part == ".." and parts and parts[-1] != "..":
            parts.pop()
        else:
            parts.append(part)
    for index, part in enumerate(parts):
        if part == "core":
            candidate = "/".join(parts[index:])
            if any(candidate.startswith(directory + "/") for directory, _ in ROOTS.values()):
                return candidate
    return "/".join(parts)


def root_for(file: str, name: str) -> str | None:
    normalized = relative_file(file)
    for root, (directory, namespace) in ROOTS.items():
        if normalized.startswith(directory + "/") and name.startswith(namespace):
            return root
    return None


def baseline_eligible(debt: Debt) -> bool:
    debt_root = root_for(debt.file, debt.name)
    return debt_root in LEGACY_ROOTS or (
        debt_root == "timeline" and debt.kind in PUBLIC_DECLARATION_KINDS
    )


def is_internal(name: str) -> bool:
    parts = name.split("::")
    return any(part == "detail" or part.endswith("_detail") for part in parts)


def normalized_cpp_spelling(value: str) -> str:
    value = re.sub(r"\s+", " ", value).strip()
    return re.sub(r"\s*([(),=&*<>\[\]])\s*", r"\1", value)


def noexcept_spelling(suffix: str) -> str:
    match = re.search(r"\bnoexcept\b", suffix)
    if match is None:
        return ""
    cursor = match.end()
    while cursor < len(suffix) and suffix[cursor].isspace():
        cursor += 1
    if cursor >= len(suffix) or suffix[cursor] != "(":
        return "noexcept"
    depth = 0
    for index in range(cursor, len(suffix)):
        if suffix[index] == "(":
            depth += 1
        elif suffix[index] == ")":
            depth -= 1
            if depth == 0:
                return normalized_cpp_spelling(suffix[match.start() : index + 1])
    return normalized_cpp_spelling(suffix[match.start() :])


def structural_signature(member: ET.Element) -> str:
    """Build an overload key from Doxygen structure, not rendered args text."""
    template_parameters: list[str] = []
    for parameter in member.findall("templateparamlist/param"):
        spelling = normalized_cpp_spelling(text(parameter.find("type")))
        spelling += normalized_cpp_spelling(text(parameter.find("array")))
        constraint = normalized_cpp_spelling(text(parameter.find("typeconstraint")))
        if constraint:
            spelling = f"{constraint} {spelling}"
        template_parameters.append(spelling)
    parameters: list[str] = []
    for parameter in member.findall("param"):
        spelling = normalized_cpp_spelling(text(parameter.find("type")))
        spelling += normalized_cpp_spelling(text(parameter.find("array")))
        parameters.append(spelling)

    args = text(member.find("argsstring"))
    suffix = args
    depth = 0
    started = False
    for index, character in enumerate(args):
        if character == "(":
            started = True
            depth += 1
        elif character == ")" and started:
            depth -= 1
            if depth == 0:
                suffix = args[index + 1 :]
                break

    qualifiers: list[str] = []
    if member.get("const") == "yes" or re.search(r"\bconst\b", suffix):
        qualifiers.append("const")
    if member.get("volatile") == "yes" or re.search(r"\bvolatile\b", suffix):
        qualifiers.append("volatile")
    ref_qualifier = member.get("refqual", "")
    if ref_qualifier == "lvalue" or re.search(r"(?:^|\s)&(?:\s|$)", suffix):
        qualifiers.append("&")
    elif ref_qualifier == "rvalue" or re.search(r"(?:^|\s)&&(?:\s|$)", suffix):
        qualifiers.append("&&")
    exception_specification = noexcept_spelling(suffix)
    if exception_specification:
        qualifiers.append(exception_specification)
    elif member.get("noexcept") == "yes":
        qualifiers.append("noexcept")
    suffix = (" " + " ".join(qualifiers)) if qualifiers else ""
    constraint = normalized_cpp_spelling(text(member.find("requiresclause")))
    if constraint:
        suffix += " " + constraint
    template = f"template<{','.join(template_parameters)}>" if template_parameters else ""
    return f"{template}({','.join(parameters)}){suffix}"


def is_exempt_function(member: ET.Element) -> bool:
    """Return true only when another durable contract fully describes the callable."""
    name = text(member.find("name"))
    qualified = text(member.find("qualifiedname"))
    args = text(member.find("argsstring"))
    return_type = text(member.find("type"))
    if is_internal(qualified):
        return True
    if "->" in args and not return_type:
        return True
    if "=delete" in args or "=default" in args:
        return True
    return name.startswith("~")


def collect(xml_dir: Path) -> tuple[dict[Debt, tuple[str, int, str]], dict[str, set[str]]]:
    undocumented: dict[Debt, tuple[str, int, str]] = {}
    public_types = {root: set() for root in ROOTS}

    for xml_path in sorted(xml_dir.glob("*.xml")):
        try:
            root = ET.parse(xml_path).getroot()
        except ET.ParseError as error:
            debt = Debt("xml", str(xml_path), "", str(xml_path))
            undocumented[debt] = (str(xml_path), 0, f"invalid Doxygen XML: {error}")
            continue

        compound = root.find("compounddef")
        if compound is None:
            continue
        compound_name = text(compound.find("compoundname"))
        compound_file, compound_line = location(compound)
        compound_root = root_for(compound_file, compound_name)
        compound_kind = compound.get("kind", "")
        public_member_owner = (
            compound_kind not in PUBLIC_COMPOUND_KINDS or compound.get("prot") == "public"
        )

        if (
            compound_root is not None
            and compound_kind in PUBLIC_COMPOUND_KINDS
            and compound.get("prot") == "public"
            and not is_internal(compound_name)
        ):
            public_types[compound_root].add(compound_name)
            if not documented(compound):
                debt = Debt("type", compound_name, "", relative_file(compound_file))
                undocumented[debt] = (
                    compound_file,
                    compound_line,
                    f"public type lacks an API contract: {compound_name}",
                )

        if not public_member_owner:
            continue

        for member in compound.findall(".//memberdef"):
            member_kind = member.get("kind", "")
            if member_kind not in ({"function"} | PUBLIC_MEMBER_KINDS):
                continue
            if member.get("prot") != "public":
                continue
            member_file, member_line = location(member)
            qualified = text(member.find("qualifiedname"))
            member_root = root_for(member_file, qualified)
            if member_root is None or is_internal(qualified):
                continue
            if member_kind == "function" and is_exempt_function(member):
                continue
            if not documented(member):
                debt = Debt(
                    "callable" if member_kind == "function" else member_kind,
                    qualified,
                    structural_signature(member) if member_kind == "function" else "",
                    relative_file(member_file),
                )
                undocumented[debt] = (
                    member_file,
                    member_line,
                    f"public {debt.kind} lacks an API contract: {qualified}{debt.parameters}",
                )

            if member_kind == "enum":
                for value in member.findall("enumvalue"):
                    if value.get("prot") != "public" or documented(value):
                        continue
                    value_name = f"{qualified}::{text(value.find('name'))}"
                    debt = Debt("enumvalue", value_name, "", relative_file(member_file))
                    undocumented[debt] = (
                        member_file,
                        member_line,
                        f"public enumvalue lacks an API contract: {value_name}",
                    )

    return undocumented, public_types


def load_baseline(path: Path) -> tuple[set[Debt], list[tuple[str, int, str]]]:
    findings: list[tuple[str, int, str]] = []
    try:
        payload = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        return set(), [(str(path), 0, f"cannot read API contract baseline: {error}")]

    if not isinstance(payload, dict) or set(payload) != {"version", "entries"}:
        return set(), [(str(path), 0, "baseline must contain only version and entries")]
    if payload["version"] != BASELINE_VERSION or not isinstance(payload["entries"], list):
        return set(), [(str(path), 0, f"baseline must use version {BASELINE_VERSION} and an entries array")]

    debts: list[Debt] = []
    expected_keys = {"kind", "name", "parameters", "file"}
    for index, value in enumerate(payload["entries"]):
        if not isinstance(value, dict) or set(value) != expected_keys:
            findings.append((str(path), 0, f"baseline entry {index} has an invalid shape"))
            continue
        if not all(isinstance(value[key], str) for key in expected_keys):
            findings.append((str(path), 0, f"baseline entry {index} values must be strings"))
            continue
        debt = Debt(**value)
        if debt.kind not in ({"type", "callable"} | PUBLIC_DECLARATION_KINDS) or not baseline_eligible(debt):
            findings.append((str(path), 0, f"baseline entry {index} is outside the legacy sequencer roots"))
            continue
        debts.append(debt)

    if debts != sorted(debts):
        findings.append((str(path), 0, "baseline entries are not in canonical sorted order"))
    if len(set(debts)) != len(debts):
        findings.append((str(path), 0, "baseline contains duplicate entries"))
    return set(debts), findings


def check_baseline_growth(
    candidate_path: Path,
    trusted_path: Path | None,
    allow_missing_trusted: bool,
) -> list[tuple[str, int, str]]:
    """Reject newly waived debt when a trusted main-branch baseline exists."""
    if trusted_path is None:
        if allow_missing_trusted:
            return []
        return [
            (
                "<sequencer-api-docs-check>",
                0,
                "trusted baseline is required unless verified bootstrap mode is explicit",
            )
        ]
    if not trusted_path.is_file():
        return [
            (
                str(trusted_path),
                0,
                "configured trusted baseline is missing or unreadable",
            )
        ]
    if allow_missing_trusted:
        return [
            (
                "<sequencer-api-docs-check>",
                0,
                "trusted baseline and verified bootstrap mode are mutually exclusive",
            )
        ]
    candidate, candidate_findings = load_baseline(candidate_path)
    trusted, trusted_findings = load_baseline(trusted_path)
    findings = candidate_findings + trusted_findings
    if findings:
        return findings
    for added in sorted(candidate - trusted):
        findings.append(
            (
                str(candidate_path),
                0,
                "candidate baseline adds unreviewed API contract debt relative to trusted main: "
                f"{added.name}{added.parameters} [{added.file}]",
            )
        )
    return findings


def doxyfile_inputs(path: Path) -> set[str]:
    logical_lines: list[str] = []
    pending = ""
    for raw in path.read_text().splitlines():
        line = raw.split("#", 1)[0].rstrip()
        pending += (" " if pending else "") + line.removesuffix("\\").strip()
        if line.endswith("\\"):
            continue
        logical_lines.append(pending)
        pending = ""
    inputs: set[str] = set()
    for line in logical_lines:
        match = re.match(r"^INPUT\s*(?:\+?=)\s*(.*)$", line)
        if match:
            inputs.update(value.strip('"') for value in match.group(1).split())
    return inputs


def check_configs(strict_config: Path, html_config: Path) -> list[tuple[str, int, str]]:
    findings: list[tuple[str, int, str]] = []
    required = {f"../../{directory}" for directory, _ in ROOTS.values()}
    try:
        strict_inputs = doxyfile_inputs(strict_config)
        html_inputs = doxyfile_inputs(html_config)
    except OSError as error:
        return [("<sequencer-api-docs-check>", 0, f"cannot read Doxygen config: {error}")]
    for config, inputs in ((strict_config, strict_inputs), (html_config, html_inputs)):
        for missing in sorted(required - inputs):
            findings.append((str(config), 0, f"required public input root is absent: {missing}"))
    for path in sorted((strict_inputs & required) ^ (html_inputs & required)):
        findings.append(("<sequencer-api-docs-check>", 0, f"strict and HTML input roots disagree: {path}"))
    return findings


def check(
    xml_dir: Path,
    baseline_path: Path,
    strict_config: Path,
    html_config: Path,
    trusted_baseline_path: Path | None = None,
    allow_missing_trusted_baseline: bool = False,
) -> list[tuple[str, int, str]]:
    findings = check_configs(strict_config, html_config)
    findings.extend(
        check_baseline_growth(
            baseline_path,
            trusted_baseline_path,
            allow_missing_trusted_baseline,
        )
    )
    undocumented, public_types = collect(xml_dir)
    baseline, baseline_findings = load_baseline(baseline_path)
    findings.extend(baseline_findings)

    for root, expected in EXPECTED_PUBLIC_TYPES.items():
        for missing in sorted(expected - public_types[root]):
            findings.append(
                (
                    "<sequencer-api-docs-check>",
                    0,
                    f"expected representative public type is absent from {root} XML: {missing}",
                )
            )

    current_legacy: set[Debt] = set()
    for debt, finding in undocumented.items():
        if baseline_eligible(debt):
            current_legacy.add(debt)
            if debt not in baseline:
                findings.append(finding)
        else:
            findings.append(finding)

    for stale in sorted(baseline - current_legacy):
        findings.append(
            (
                str(baseline_path),
                0,
                f"stale API contract baseline entry: {stale.name}{stale.parameters} [{stale.file}]",
            )
        )
    return sorted(set(findings))


@contextmanager
def exclusive_file_lock(lock):
    if os.name == "nt":
        import msvcrt

        lock.seek(0, os.SEEK_END)
        if lock.tell() == 0:
            lock.write(b"\0")
            lock.flush()
        lock.seek(0)
        while True:
            try:
                msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
                break
            except OSError:
                time.sleep(0.1)
        try:
            yield
        finally:
            lock.seek(0)
            msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)
    else:
        import fcntl

        fcntl.flock(lock, fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(lock, fcntl.LOCK_UN)


def require_real_directory(path: Path, *, allow_absent: bool, label: str) -> None:
    if not os.path.lexists(path):
        if allow_absent:
            return
        raise RuntimeError(f"{label} is absent: {path}")
    if path.is_symlink() or not path.is_dir():
        raise RuntimeError(f"{label} must be a real directory: {path}")


def publish_api_docs(staging: Path, output: Path, lock_path: Path) -> None:
    previous = output.with_name(output.name + ".previous")
    lock_path.parent.mkdir(parents=True, exist_ok=True)
    with lock_path.open("a+b") as lock, exclusive_file_lock(lock):
        require_real_directory(staging, allow_absent=False, label="staged API documentation")
        require_real_directory(output, allow_absent=True, label="published API documentation")
        require_real_directory(previous, allow_absent=True, label="API documentation backup")

        if previous.exists():
            if output.exists():
                shutil.rmtree(previous)
            else:
                os.replace(previous, output)
        if output.exists():
            os.replace(output, previous)
        try:
            os.replace(staging, output)
        except BaseException:
            if previous.exists() and not output.exists():
                os.replace(previous, output)
            raise
        if previous.exists():
            shutil.rmtree(previous)


def publication_self_test() -> list[str]:
    failures: list[str] = []

    def make_tree(path: Path, marker: str) -> None:
        path.mkdir()
        (path / "marker.txt").write_text(marker)

    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        lock = root / "publish.lock"

        output = root / "normal"
        staging = root / "normal-stage"
        make_tree(output, "old")
        make_tree(staging, "new")
        publish_api_docs(staging, output, lock)
        if (output / "marker.txt").read_text() != "new" or staging.exists():
            failures.append("normal publication did not replace the tree")

        recovered = root / "recovered"
        previous = root / "recovered.previous"
        recovery_stage = root / "recovered-stage"
        make_tree(previous, "old")
        make_tree(recovery_stage, "new")
        publish_api_docs(recovery_stage, recovered, lock)
        if (recovered / "marker.txt").read_text() != "new" or previous.exists():
            failures.append("interrupted-backup recovery did not converge")

        regular_output = root / "regular-output"
        regular_output.write_text("sentinel")
        regular_stage = root / "regular-stage"
        make_tree(regular_stage, "new")
        try:
            publish_api_docs(regular_stage, regular_output, lock)
            failures.append("regular-file output was accepted")
        except RuntimeError as error:
            if "published API documentation must be a real directory" not in str(error):
                failures.append(f"regular-file output reported the wrong error: {error}")
        if regular_output.read_text() != "sentinel" or not regular_stage.is_dir():
            failures.append("regular-file refusal mutated publication state")

        symlink_target = root / "symlink-target"
        make_tree(symlink_target, "sentinel")
        symlink_output = root / "symlink-output"
        try:
            symlink_output.symlink_to(symlink_target, target_is_directory=True)
        except OSError:
            # Native Windows may deny symlink creation without developer mode.
            pass
        else:
            symlink_stage = root / "symlink-stage"
            make_tree(symlink_stage, "new")
            try:
                publish_api_docs(symlink_stage, symlink_output, lock)
                failures.append("symlink output was accepted")
            except RuntimeError as error:
                if "published API documentation must be a real directory" not in str(error):
                    failures.append(f"symlink output reported the wrong error: {error}")
            if (symlink_target / "marker.txt").read_text() != "sentinel" or not symlink_stage.is_dir():
                failures.append("symlink refusal mutated publication state")

    return failures


PUBLIC_HEADERS = {
    root: f"/src/{directory}/pulp/{root}/model.hpp" for root, (directory, _) in ROOTS.items()
}


def _compound_xml(
    name: str,
    root: str,
    *,
    documented_type: bool = True,
    members: list[dict[str, object]] | None = None,
    file: str | None = None,
    kind: str = "class",
    prot: str = "public",
) -> str:
    file = file or PUBLIC_HEADERS[root]
    brief = "<para>A contract.</para>" if documented_type else ""
    body = []
    for member in members or []:
        member_brief = "<para>A contract.</para>" if member.get("documented") else ""
        attributes = [
            f'kind="{member.get("kind", "function")}"',
            f'prot="{member.get("prot", "public")}"',
        ]
        for attribute in ("const", "volatile", "noexcept", "refqual"):
            if attribute in member:
                attributes.append(f'{attribute}="{member[attribute]}"')
        parameters = "".join(
            f'<param><type>{parameter["type"]}</type><array>{parameter.get("array", "")}</array></param>'
            if isinstance(parameter, dict)
            else f"<param><type>{parameter}</type></param>"
            for parameter in member.get("params", [])
        )
        template_parameters = "".join(
            f'<param><type>{parameter["type"]}</type><array>{parameter.get("array", "")}</array>'
            f'<typeconstraint>{parameter.get("constraint", "")}</typeconstraint></param>'
            if isinstance(parameter, dict)
            else f"<param><type>{parameter}</type></param>"
            for parameter in member.get("template_params", [])
        )
        template_list = (
            f"<templateparamlist>{template_parameters}</templateparamlist>"
            if template_parameters
            else ""
        )
        requires_clause = (
            f'<requiresclause>{member["requires"]}</requiresclause>'
            if member.get("requires")
            else ""
        )
        enum_values = "".join(
            f'<enumvalue prot="{value.get("prot", "public")}"><name>{value["name"]}</name>'
            f'<briefdescription>{"<para>A contract.</para>" if value.get("documented") else ""}'
            f"</briefdescription><detaileddescription/></enumvalue>"
            for value in member.get("enumvalues", [])
        )
        body.append(
            f'<memberdef {" ".join(attributes)}>'
            f'<type>{member.get("type", "bool")}</type><name>{member["name"]}</name>'
            f'<qualifiedname>{member["qualified"]}</qualifiedname>'
            f'<argsstring>{member.get("args", "() const noexcept")}</argsstring>'
            f"{template_list}{parameters}{requires_clause}{enum_values}"
            f'<briefdescription>{member_brief}</briefdescription><detaileddescription/>'
            f'<location file="{member.get("file", file)}" line="20"/></memberdef>'
        )
    return (
        f'<?xml version="1.0"?><doxygen><compounddef kind="{kind}" prot="{prot}" id="x">'
        f"<compoundname>{name}</compoundname><briefdescription>{brief}</briefdescription>"
        f'<detaileddescription/><location file="{file}" line="10"/>'
        f"<sectiondef>{''.join(body)}</sectiondef></compounddef></doxygen>"
    )


def _write_fixture(root: Path, extra: list[str], excluded_types: set[str] | None = None) -> Path:
    xml_dir = root / "xml"
    xml_dir.mkdir(parents=True)
    index = 0
    for api_root, names in EXPECTED_PUBLIC_TYPES.items():
        for name in sorted(names):
            if name in (excluded_types or set()):
                continue
            (xml_dir / f"base{index}.xml").write_text(_compound_xml(name, api_root))
            index += 1
    for offset, xml in enumerate(extra):
        (xml_dir / f"case{offset}.xml").write_text(xml)
    return xml_dir


def _write_configs(
    root: Path,
    *,
    strict_omit: str | None = None,
    html_omit: str | None = None,
) -> tuple[Path, Path]:
    strict = root / "strict"
    html = root / "html"
    strict_inputs = [
        f"../../{directory}" for name, (directory, _) in ROOTS.items() if name != strict_omit
    ]
    html_inputs = [
        f"../../{directory}" for name, (directory, _) in ROOTS.items() if name != html_omit
    ]
    strict.write_text("INPUT = " + " ".join(strict_inputs) + "\n")
    html.write_text("INPUT = " + " ".join(html_inputs) + "\n")
    return strict, html


def _write_baseline(root: Path, entries: list[Debt]) -> Path:
    path = root / "baseline.json"
    path.write_text(
        json.dumps(
            {"version": BASELINE_VERSION, "entries": [entry.json_value() for entry in sorted(entries)]},
            indent=2,
        )
        + "\n"
    )
    return path


def self_test() -> int:
    """Prove exemptions, coverage roots, and the debt ratchet in both directions."""
    failures = 0
    cases = 0

    def run(
        label: str,
        extra: list[str],
        baseline: list[Debt],
        expected: int,
        *,
        excluded_types: set[str] | None = None,
        strict_omit: str | None = None,
        html_omit: str | None = None,
        trusted: list[Debt] | None = None,
        missing_trusted: bool = False,
        trusted_content: str | None = None,
        allow_missing_trusted: bool = True,
        expected_messages: tuple[str, ...] = (),
    ) -> None:
        nonlocal cases, failures
        cases += 1
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            xml_dir = _write_fixture(root, extra, excluded_types)
            strict, html = _write_configs(root, strict_omit=strict_omit, html_omit=html_omit)
            candidate_path = _write_baseline(root, baseline)
            trusted_path = None
            if trusted is not None:
                trusted_root = root / "trusted"
                trusted_root.mkdir()
                trusted_path = _write_baseline(trusted_root, trusted)
                allow_missing_trusted = False
            elif missing_trusted:
                trusted_path = root / "absent-main-baseline.json"
                allow_missing_trusted = False
            elif trusted_content is not None:
                trusted_path = root / "trusted-invalid.json"
                trusted_path.write_text(trusted_content)
                allow_missing_trusted = False
            findings = check(
                xml_dir,
                candidate_path,
                strict,
                html,
                trusted_path,
                allow_missing_trusted,
            )
            messages = "\n".join(message for _, _, message in findings)
            messages_match = all(expected_message in messages for expected_message in expected_messages)
            found = len(findings)
        passed = found == expected and messages_match
        status = "ok" if passed else "FAIL"
        print(f"  [{status}] {label}: expected {expected} finding(s), got {found}")
        if not messages_match:
            print(f"    missing expected message content: {expected_messages}")
        failures += not passed

    def member(**overrides: object) -> dict[str, object]:
        return {
            "name": "valid",
            "qualified": "pulp::timeline::Widget::valid",
            "documented": False,
            "const": "yes",
            "noexcept": "yes",
            **overrides,
        }

    run("clean tree reports nothing", [], [], 0)
    run(
        "undocumented public callable is flagged",
        [_compound_xml("pulp::timeline::Widget", "timeline", members=[member()])],
        [],
        1,
    )
    run(
        "documented public callable is not flagged",
        [_compound_xml("pulp::timeline::Widget", "timeline", members=[member(documented=True)])],
        [],
        0,
    )
    run(
        "undocumented public type is flagged",
        [_compound_xml("pulp::timeline::Widget", "timeline", documented_type=False)],
        [],
        1,
    )
    run(
        "detail namespace is exempt",
        [
            _compound_xml(
                "pulp::timeline::detail::Widget",
                "timeline",
                documented_type=False,
                members=[member(qualified="pulp::timeline::detail::Widget::valid")],
            )
        ],
        [],
        0,
    )
    run(
        "detail namespace declarations are exempt",
        [
            _compound_xml(
                "pulp::timeline::detail::Widget",
                "timeline",
                documented_type=False,
                members=[
                    member(
                        kind=kind,
                        qualified=f"pulp::timeline::detail::Widget::Internal{kind.title()}",
                    )
                    for kind in sorted(PUBLIC_MEMBER_KINDS)
                ],
            )
        ],
        [],
        0,
    )
    run(
        "destructor is exempt",
        [
            _compound_xml(
                "pulp::timeline::Widget",
                "timeline",
                members=[member(name="~Widget", qualified="pulp::timeline::Widget::~Widget")],
            )
        ],
        [],
        0,
    )
    run(
        "defaulted and deleted members are exempt",
        [
            _compound_xml(
                "pulp::timeline::Widget",
                "timeline",
                members=[member(args="()=default"), member(args="()=delete")],
            )
        ],
        [],
        0,
    )
    run(
        "a header outside the public roots is ignored",
        [
            _compound_xml(
                "pulp::timeline::Widget",
                "timeline",
                documented_type=False,
                file="/src/core/host/src/signal_graph.cpp",
            )
        ],
        [],
        0,
    )
    run(
        "private members are ignored",
        [_compound_xml("pulp::timeline::Widget", "timeline", members=[member(prot="private")])],
        [],
        0,
    )
    run(
        "public fields of a private nested compound are ignored",
        [
            _compound_xml(
                "pulp::timeline::Widget::PrivateState",
                "timeline",
                prot="private",
                members=[
                    member(
                        kind="variable",
                        qualified="pulp::timeline::Widget::PrivateState::value",
                    )
                ],
            )
        ],
        [],
        0,
    )
    for declaration_kind in sorted(PUBLIC_MEMBER_KINDS):
        run(
            f"undocumented public {declaration_kind} is flagged",
            [
                _compound_xml(
                    "pulp::timeline::Widget",
                    "timeline",
                    members=[
                        member(
                            kind=declaration_kind,
                            qualified=f"pulp::timeline::Widget::Public{declaration_kind.title()}",
                        )
                    ],
                )
            ],
            [],
            1,
        )
    run(
        "undocumented public enum value is flagged",
        [
            _compound_xml(
                "pulp::timeline::Widget",
                "timeline",
                members=[
                    member(
                        kind="enum",
                        qualified="pulp::timeline::Widget::Mode",
                        documented=True,
                        enumvalues=[{"name": "active", "documented": False}],
                    )
                ],
            )
        ],
        [],
        1,
    )
    missing_type = sorted(EXPECTED_PUBLIC_TYPES["timeline"])[0]
    run("a missing representative public type is flagged", [], [], 1, excluded_types={missing_type})

    music_file = PUBLIC_HEADERS["music"]
    legacy = Debt("callable", "pulp::music::Widget::valid", "(int) const", relative_file(music_file))
    undocumented = _compound_xml(
        "pulp::music::Widget",
        "music",
        members=[
            {
                "name": "valid",
                "qualified": legacy.name,
                "args": "(int value) const",
                "params": ["int"],
                "const": "yes",
                "documented": False,
            }
        ],
    )
    run("reviewed legacy debt is accepted", [undocumented], [legacy], 0)
    portable_noexcept = Debt(
        "callable",
        "pulp::music::Widget::ready",
        "() const noexcept",
        relative_file(music_file),
    )
    run(
        "Doxygen 1.9 args preserve noexcept without the newer XML attribute",
        [
            _compound_xml(
                "pulp::music::Widget",
                "music",
                members=[
                    {
                        "name": "ready",
                        "qualified": portable_noexcept.name,
                        "args": "() const noexcept",
                        "const": "yes",
                        "documented": False,
                    }
                ],
            )
        ],
        [portable_noexcept],
        0,
    )
    overload_int = Debt("callable", "pulp::music::Widget::overloaded", "(int)", relative_file(music_file))
    overload_xml = _compound_xml(
        "pulp::music::Widget",
        "music",
        members=[
            {
                "name": "overloaded",
                "qualified": overload_int.name,
                "args": "(int value)",
                "params": ["int"],
                "documented": False,
            },
            {
                "name": "overloaded",
                "qualified": overload_int.name,
                "args": "(float value)",
                "params": ["float"],
                "documented": False,
            },
        ],
    )
    run(
        "overloads retain distinct structural identities",
        [overload_xml],
        [overload_int],
        1,
        expected_messages=("pulp::music::Widget::overloaded(float)",),
    )
    lvalue = Debt("callable", "pulp::music::Widget::access", "() &", relative_file(music_file))
    ref_xml = _compound_xml(
        "pulp::music::Widget",
        "music",
        members=[
            {
                "name": "access",
                "qualified": lvalue.name,
                "args": "()",
                "refqual": "lvalue",
                "documented": False,
            },
            {
                "name": "access",
                "qualified": lvalue.name,
                "args": "()",
                "refqual": "rvalue",
                "documented": False,
            },
        ],
    )
    run(
        "ref-qualified overloads retain distinct identities",
        [ref_xml],
        [lvalue],
        1,
        expected_messages=("pulp::music::Widget::access() &&",),
    )
    array_debt = Debt(
        "callable",
        "pulp::music::Widget::array",
        "(int[4])",
        relative_file(music_file),
    )
    run(
        "array extents participate in structural identity",
        [
            _compound_xml(
                "pulp::music::Widget",
                "music",
                members=[
                    {
                        "name": "array",
                        "qualified": array_debt.name,
                        "args": "(int value[4])",
                        "params": [{"type": "int", "array": "[4]"}],
                        "documented": False,
                    }
                ],
            )
        ],
        [array_debt],
        0,
    )
    template_debt = Debt(
        "callable",
        "pulp::music::Widget::convert",
        "template<typename>(T)",
        relative_file(music_file),
    )
    run(
        "template parameter structure participates in identity",
        [
            _compound_xml(
                "pulp::music::Widget",
                "music",
                members=[
                    {
                        "name": "convert",
                        "qualified": template_debt.name,
                        "args": "(T value)",
                        "template_params": ["typename"],
                        "params": ["T"],
                        "documented": False,
                    }
                ],
            )
        ],
        [template_debt],
        0,
    )
    constrained = Debt(
        "callable",
        "pulp::music::Widget::convert",
        "template<typename>(T) requires Alpha",
        relative_file(music_file),
    )
    constrained_xml = _compound_xml(
        "pulp::music::Widget",
        "music",
        members=[
            {
                "name": "convert",
                "qualified": constrained.name,
                "args": "(T value)",
                "template_params": ["typename"],
                "params": ["T"],
                "requires": "requires Alpha",
                "documented": False,
            },
            {
                "name": "convert",
                "qualified": constrained.name,
                "args": "(T value)",
                "template_params": ["typename"],
                "params": ["T"],
                "requires": "requires Beta",
                "documented": False,
            },
        ],
    )
    run(
        "requires clauses retain distinct structural identities",
        [constrained_xml],
        [constrained],
        1,
        expected_messages=(
            "pulp::music::Widget::converttemplate<typename>(T) requires Beta",
        ),
    )
    constrained_parameter = Debt(
        "callable",
        "pulp::music::Widget::transform",
        "template<Integral T>(T)",
        relative_file(music_file),
    )
    constrained_parameter_xml = _compound_xml(
        "pulp::music::Widget",
        "music",
        members=[
            {
                "name": "transform",
                "qualified": constrained_parameter.name,
                "args": "(T value)",
                "template_params": [{"type": "T", "constraint": "Integral"}],
                "params": ["T"],
                "documented": False,
            },
            {
                "name": "transform",
                "qualified": constrained_parameter.name,
                "args": "(T value)",
                "template_params": [{"type": "T", "constraint": "Floating"}],
                "params": ["T"],
                "documented": False,
            },
        ],
    )
    run(
        "constrained template parameters retain distinct identities",
        [constrained_parameter_xml],
        [constrained_parameter],
        1,
        expected_messages=(
            "pulp::music::Widget::transformtemplate<Floating T>(T)",
        ),
    )
    conditional_noexcept = Debt(
        "callable",
        "pulp::music::Widget::flush",
        "() noexcept(alpha())",
        relative_file(music_file),
    )
    conditional_noexcept_xml = _compound_xml(
        "pulp::music::Widget",
        "music",
        members=[
            {
                "name": "flush",
                "qualified": conditional_noexcept.name,
                "args": "() noexcept(alpha())",
                "documented": False,
            },
            {
                "name": "flush",
                "qualified": conditional_noexcept.name,
                "args": "() noexcept(beta())",
                "documented": False,
            },
        ],
    )
    run(
        "conditional noexcept expressions retain distinct identities",
        [conditional_noexcept_xml],
        [conditional_noexcept],
        1,
        expected_messages=(
            "pulp::music::Widget::flush() noexcept(beta())",
        ),
    )
    run(
        "new undocumented legacy callable is flagged",
        [undocumented],
        [],
        1,
        expected_messages=("public callable lacks an API contract",),
    )
    changed = undocumented.replace("(int value) const", "(float value) const").replace(
        "<type>int</type>", "<type>float</type>"
    )
    run("changed signature makes new and stale findings", [changed], [legacy], 2)
    run("stale baseline debt is flagged", [], [legacy], 1)
    windows_path = undocumented.replace("/src/core/", "C:\\checkout\\core\\")
    run("Windows source paths normalize to baseline keys", [windows_path], [legacy], 0)
    internal = undocumented.replace("pulp::music::Widget", "pulp::music::detail::Widget")
    run("internal callable is ignored", [internal], [], 0)
    run(
        "missing strict and HTML root is detected",
        [],
        [],
        2,
        strict_omit="timeline_view",
        html_omit="timeline_view",
    )
    run(
        "strict and HTML root disagreement is detected",
        [],
        [],
        2,
        strict_omit="timeline_view",
    )
    run(
        "candidate baseline growth is rejected",
        [undocumented],
        [legacy],
        1,
        trusted=[],
        expected_messages=("adds unreviewed API contract debt relative to trusted main",),
    )
    run("candidate baseline shrink is accepted", [], [], 0, trusted=[legacy])
    run(
        "missing configured trusted baseline fails closed",
        [undocumented],
        [legacy],
        1,
        missing_trusted=True,
        expected_messages=("configured trusted baseline is missing or unreadable",),
    )
    run(
        "verified first-baseline bootstrap is explicit",
        [undocumented],
        [legacy],
        0,
        allow_missing_trusted=True,
    )
    run(
        "implicit bootstrap is rejected",
        [undocumented],
        [legacy],
        1,
        allow_missing_trusted=False,
        expected_messages=("trusted baseline is required unless verified bootstrap mode is explicit",),
    )
    run(
        "corrupt configured trusted baseline fails closed",
        [undocumented],
        [legacy],
        1,
        trusted_content="not-json\n",
        expected_messages=("cannot read API contract baseline",),
    )

    publication_failures = publication_self_test()
    cases += 1
    if publication_failures:
        failures += 1
        print(f"  [FAIL] publication transaction controls: {'; '.join(publication_failures)}")
    else:
        print("  [ok] publication transaction controls")

    if failures:
        print(f"sequencer API docs checker self-test FAILED ({failures} case(s))")
        return 1
    print(f"sequencer API docs checker self-test passed ({cases} cases)")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("xml_dir", type=Path, nargs="?", help="Doxygen XML output directory")
    parser.add_argument("--baseline", type=Path, help="reviewed legacy contract-debt baseline")
    parser.add_argument("--strict-config", type=Path, help="strict XML Doxyfile")
    parser.add_argument("--html-config", type=Path, help="published HTML Doxyfile")
    trusted_group = parser.add_mutually_exclusive_group()
    trusted_group.add_argument(
        "--trusted-baseline",
        type=Path,
        help="baseline read from a verified trusted main-branch ref",
    )
    trusted_group.add_argument(
        "--allow-missing-trusted-baseline",
        action="store_true",
        help="allow bootstrap only after the caller verifies the trusted ref lacks the file",
    )
    parser.add_argument("--publish-staging", type=Path, help="validated API-doc staging directory")
    parser.add_argument("--publish-output", type=Path, help="stable API-doc output directory")
    parser.add_argument("--publish-lock", type=Path, help="cross-process publication lock file")
    parser.add_argument("--self-test", action="store_true", help="run checker controls without Doxygen")
    args = parser.parse_args()

    if args.self_test:
        return self_test()
    publish_values = (args.publish_staging, args.publish_output, args.publish_lock)
    if any(value is not None for value in publish_values):
        if not all(value is not None for value in publish_values):
            parser.error("--publish-staging, --publish-output, and --publish-lock are required together")
        try:
            publish_api_docs(args.publish_staging, args.publish_output, args.publish_lock)
        except (OSError, RuntimeError) as error:
            print(f"API documentation publication failed: {error}", file=sys.stderr)
            return 1
        return 0
    missing = [name for name in ("xml_dir", "baseline", "strict_config", "html_config") if getattr(args, name) is None]
    if missing:
        parser.error("required for a real check: " + ", ".join(missing))
    if not args.xml_dir.is_dir():
        parser.error(f"not a Doxygen XML directory: {args.xml_dir}")

    findings = check(
        args.xml_dir,
        args.baseline,
        args.strict_config,
        args.html_config,
        args.trusted_baseline,
        args.allow_missing_trusted_baseline,
    )
    if findings:
        for file, line, message in findings:
            print(f"{file}:{line}: error: {message}")
        print(f"Sequencer API documentation check failed with {len(findings)} issue(s).")
        return 1
    print("Sequencer API documentation contracts satisfy the exhaustive and reviewed baselines.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

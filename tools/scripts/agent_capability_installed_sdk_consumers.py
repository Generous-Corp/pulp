#!/usr/bin/env python3
"""Plan and materialize independent installed-SDK consumer proofs."""
from __future__ import annotations

import dataclasses
import pathlib


@dataclasses.dataclass(frozen=True)
class ConsumerProof:
    identity: tuple[str, ...]
    name: str
    source: str
    target: str


def binding_identity(row: dict, binding: dict) -> tuple[str, str, str]:
    return row["key"], binding["role"], binding["qualified_name"]


def binding_source(
    row: dict,
    binding: dict,
    addresses: dict[tuple[str, str, str], str],
    operational_probe: str | None = None,
) -> str:
    name = binding["qualified_name"]
    reference = (
        f"static_assert(sizeof({name}) > 0);"
        if binding["kind"] == "cpp_type"
        else (
            "auto volatile binding = "
            f"{addresses[binding_identity(row, binding)]}; (void)binding;"
        )
    )
    probe = f"\n    {operational_probe}" if operational_probe else ""
    return (
        f"#include <{binding['include']}>\n\n"
        f"int main() {{\n    // {row['key']} / {binding['role']}\n"
        f"    {reference}{probe}\n    return 0;\n}}\n"
    )


def capability_source(
    row: dict,
    probe: str,
    addresses: dict[tuple[str, str, str], str],
) -> str:
    includes = sorted({binding["include"] for binding in row["bindings"]})
    lines = [*(f"#include <{include}>" for include in includes), "", "int main() {"]
    for index, binding in enumerate(row["bindings"]):
        name = binding["qualified_name"]
        if binding["kind"] == "cpp_type":
            lines.append(f"    static_assert(sizeof({name}) > 0);")
        else:
            address = addresses[binding_identity(row, binding)]
            lines.append(
                f"    auto volatile binding_{index} = {address}; (void)binding_{index};"
            )
    lines.append(f"    {probe}")
    lines.extend(["    return 0;", "}", ""])
    return "\n".join(lines)


def positive_consumer_proofs(
    document: dict,
    probes: dict[str, str],
    binding_probes: dict[tuple[str, str, str], str],
    addresses: dict[tuple[str, str, str], str],
) -> list[ConsumerProof]:
    proofs: list[ConsumerProof] = []
    for row_index, row in enumerate(document["capabilities"]):
        targets_for_row = {binding["target"] for binding in row["bindings"]}
        if len(targets_for_row) != 1:
            raise RuntimeError(
                f"{row['key']} spans multiple minimal targets; split its capability contract"
            )
        proofs.append(
            ConsumerProof(
                identity=("capability", row["key"]),
                name=f"capability_{row_index}",
                source=capability_source(row, probes[row["key"]], addresses),
                target=next(iter(targets_for_row)),
            )
        )
        for binding_index, binding in enumerate(row["bindings"]):
            identity = binding_identity(row, binding)
            proofs.append(
                ConsumerProof(
                    identity=("binding", *identity),
                    name=f"binding_{row_index}_{binding_index}",
                    source=binding_source(
                        row, binding, addresses, binding_probes[identity]
                    ),
                    target=binding["target"],
                )
            )
    validate_positive_proof_coverage(document, proofs)
    return proofs


def validate_positive_proof_coverage(
    document: dict, proofs: list[ConsumerProof]
) -> None:
    expected: dict[tuple[str, ...], str] = {}
    for row in document["capabilities"]:
        targets = {binding["target"] for binding in row["bindings"]}
        if len(targets) != 1:
            raise RuntimeError(
                f"{row['key']} spans multiple minimal targets; split its capability contract"
            )
        expected[("capability", row["key"])] = next(iter(targets))
        for binding in row["bindings"]:
            expected[("binding", *binding_identity(row, binding))] = binding["target"]

    actual = [proof.identity for proof in proofs]
    if len(actual) != len(set(actual)):
        raise RuntimeError("positive installed-SDK proof identities are not unique")
    if set(actual) != set(expected):
        missing = sorted(set(expected) - set(actual))
        unexpected = sorted(set(actual) - set(expected))
        raise RuntimeError(
            "positive installed-SDK proof coverage mismatch: "
            f"missing={missing}, unexpected={unexpected}"
        )
    wrong_targets = [
        (proof.identity, proof.target, expected[proof.identity])
        for proof in proofs
        if proof.target != expected[proof.identity]
    ]
    if wrong_targets:
        raise RuntimeError(
            "positive installed-SDK proof target mismatch: " + repr(wrong_targets)
        )
    names = [proof.name for proof in proofs]
    if len(names) != len(set(names)):
        raise RuntimeError("positive installed-SDK proof target names are not unique")


def write_consumer_suite(
    project: pathlib.Path, proofs: list[ConsumerProof]
) -> None:
    project.mkdir(parents=True, exist_ok=True)
    cmake_lines = [
        "cmake_minimum_required(VERSION 3.20)",
        "project(pulp_agent_capability_consumers LANGUAGES CXX)",
        "find_package(Pulp CONFIG REQUIRED)",
    ]
    for proof in proofs:
        (project / f"{proof.name}.cpp").write_text(proof.source)
        cmake_lines.extend([
            f"if(NOT TARGET {proof.target})",
            f'  message(FATAL_ERROR "Declared capability target is absent: {proof.target}")',
            "endif()",
            f"add_executable({proof.name} {proof.name}.cpp)",
            f"target_link_libraries({proof.name} PRIVATE {proof.target})",
            (
                'file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/consumer-path-'
                f'{proof.name}-$<CONFIG>.txt" CONTENT "$<TARGET_FILE:{proof.name}>")'
            ),
        ])
    (project / "CMakeLists.txt").write_text("\n".join(cmake_lines) + "\n")

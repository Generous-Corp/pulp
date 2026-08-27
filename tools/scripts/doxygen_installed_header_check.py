#!/usr/bin/env python3
"""Check file-granular parity between installed Pulp headers and Doxygen."""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

HEADER_SUFFIXES = {".h", ".hpp", ".inc"}
GENERATED_TEMPLATE = "core/runtime/include/pulp/runtime/build_info.hpp.in"
GENERATED_HEADER = "core/runtime/include/pulp/runtime/build_info.hpp"
POLICY = Path("docs/doxygen/installed-public-header-policy.json")
DOXYFILE = Path("docs/doxygen/Doxyfile")
INSTALL_RULES = Path("tools/cmake/PulpInstallRules.cmake")


def _reject_symlink_components(root: Path, relative: Path) -> None:
    current = root
    for part in relative.parts:
        current /= part
        if current.is_symlink():
            raise ValueError(f"authority path contains a symlink: {current.relative_to(root)}")


def _headers(root: Path, relative_root: str,
             suffixes: set[str] = HEADER_SUFFIXES,
             doxygen_excludes: bool = False,
             pulp_only: bool = False,
             include_templates: bool = False) -> set[str]:
    relative = Path(relative_root)
    _reject_symlink_components(root, relative)
    directory = root / relative
    if not directory.is_dir():
        raise ValueError(f"authority root does not exist: {relative_root}")
    directory = directory / "pulp" if pulp_only else directory
    if not directory.is_dir():
        raise ValueError(f"authority root has no pulp directory: {relative_root}")
    for path in directory.rglob("*"):
        if path.is_symlink():
            raise ValueError(f"authority root contains a symlink: {path.relative_to(root)}")
    found = {
        path.relative_to(root).as_posix()
        for path in directory.rglob("*")
        if path.is_file() and (path.suffix in suffixes or
        (include_templates and path.name.endswith(".hpp.in"))) and
        (not doxygen_excludes or not ({"src", "detail"} & set(path.parts)))
    }
    return {
        GENERATED_HEADER if path == GENERATED_TEMPLATE else path
        for path in found
    }


def _doxy_assignments(text: str) -> dict[str, str]:
    clean = "\n".join(line.split("#", 1)[0] for line in text.splitlines())
    logical = clean.replace("\\\n", " ")
    assignments: dict[str, str] = {}
    for key, operator, value in re.findall(
        r"(?m)^([A-Z][A-Z0-9_]*)\s*(\+?=)\s*(.*)$", logical
    ):
        if operator != "=":
            raise ValueError(f"Doxyfile has unsupported {key} accumulation")
        if key in assignments:
            raise ValueError(f"Doxyfile has duplicate {key} assignments")
        assignments[key] = value.strip()
    return assignments


def _doxygen_roots(root: Path) -> list[str]:
    text = (root / DOXYFILE).read_text()
    assignments = _doxy_assignments(text)
    if "INPUT" not in assignments:
        raise ValueError("Doxyfile has no static INPUT assignment")
    if assignments.get("RECURSIVE") != "YES":
        raise ValueError("Doxyfile must set RECURSIVE = YES")
    if set(assignments.get("FILE_PATTERNS", "").split()) != {
        "*.h", "*.hpp", "*.inc", "*.hpp.in"
    }:
        raise ValueError(
            "Doxyfile FILE_PATTERNS must be exactly *.h *.hpp *.inc *.hpp.in"
        )
    if set(assignments.get("EXCLUDE_PATTERNS", "").split()) != {"*/src/*", "*/detail/*"}:
        raise ValueError("Doxyfile EXCLUDE_PATTERNS must be exactly */src/* */detail/*")
    if assignments.get("EXCLUDE", ""):
        raise ValueError("Doxyfile EXCLUDE must be empty; use the reviewed static roots")
    tokens = assignments["INPUT"].split()
    if not tokens or any("$" in token or "*" in token for token in tokens):
        raise ValueError("Doxyfile INPUT must contain only static directory roots")
    roots = []
    for token in tokens:
        candidate = root / "docs/doxygen" / token
        if candidate.is_symlink():
            raise ValueError(f"Doxygen INPUT root is a symlink: {token}")
        resolved = candidate.resolve().relative_to(root.resolve())
        _reject_symlink_components(root, resolved)
        roots.append(resolved.as_posix())
    duplicates = sorted({item for item in roots if roots.count(item) > 1})
    if duplicates:
        raise ValueError("duplicate Doxygen INPUT roots: " + ", ".join(duplicates))
    return roots


def _install_roots(root: Path) -> tuple[list[str], set[str], set[str]]:
    raw_text = (root / INSTALL_RULES).read_text()
    text = "\n".join(line.split("#", 1)[0] for line in raw_text.splitlines())
    consumers = re.findall(
        r"(?ms)foreach\(subsystem\s+IN\s+LISTS\s+_pulp_sdk_header_subsystems\)(.*?)endforeach\(\)",
        text,
    )
    if len(consumers) != 1:
        raise ValueError("install rules must have exactly one SDK header subsystem consumer")
    consumer = consumers[0]
    required_consumer_fragments = (
        'set(_inc_dir "${CMAKE_CURRENT_SOURCE_DIR}/core/${subsystem}/include")',
        'install(DIRECTORY "${_inc_dir}/pulp/"',
        'FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h"',
    )
    if any(fragment not in consumer for fragment in required_consumer_fragments):
        raise ValueError("SDK header subsystem consumer is not the canonical install loop")
    if text.count('install(DIRECTORY "${_inc_dir}/pulp/"') != 1:
        raise ValueError("install rules must have one canonical SDK header directory install")
    blocks = re.findall(
        r"(?ms)(?:set\(|list\(APPEND\s+)_pulp_sdk_header_subsystems\s+(.*?)\)", text
    )
    if not blocks:
        raise ValueError("install rules have no static SDK header subsystem authority")
    subsystems: list[str] = []
    for block in blocks:
        block = re.sub(r"#.*", "", block)
        tokens = block.split()
        if any(not re.fullmatch(r"[A-Za-z0-9_-]+", token) for token in tokens):
            raise ValueError("SDK header subsystem authority contains a dynamic construct")
        subsystems.extend(tokens)
    if len(subsystems) != len(set(subsystems)):
        raise ValueError("SDK header subsystem authority contains duplicates")
    install_blocks = re.findall(r"(?ms)install\((?:DIRECTORY|FILES)\s+(.*?)\)", text)
    for block in install_blocks:
        if not re.search(r"(?:include|_inc_dir\})/pulp", block):
            continue
        if ('"${_inc_dir}/pulp/"' in block or
                '"${CMAKE_CURRENT_SOURCE_DIR}/tools/audio/analysis/include/pulp/"' in block or
                '"${CMAKE_CURRENT_SOURCE_DIR}/inspect/include/pulp/' in block):
            continue
        raise ValueError("unsupported independent public-header install authority (possibly dynamic)")
    public_install_sources = set().union(*(
        set(re.findall(
            r'"\$\{CMAKE_CURRENT_SOURCE_DIR\}/([^"\n]*include/pulp(?:/[^"\n]*)?)"',
            block,
        ))
        for block in install_blocks
    ))
    for source in public_install_sources:
        if not (source.startswith("inspect/include/pulp/") or
                source == "tools/audio/analysis/include/pulp/"):
            raise ValueError(f"unsupported independent public-header install authority: {source}")
    roots = [f"core/{name}/include" for name in subsystems]
    if 'tools/audio/analysis/include/pulp/' in text:
        roots.append("tools/audio/analysis/include")
    roots.append("inspect/include")

    marker = "elseif(TARGET pulp-inspect-protocol)"
    if marker not in text:
        raise ValueError("inspector-off install authority is missing")
    tail = text.split(marker, 1)[1]
    pattern = r'"\$\{CMAKE_CURRENT_SOURCE_DIR\}/(inspect/include/pulp/inspect/[^"$]+\.(?:h|hpp|inc))"'
    off_allowed = set(re.findall(pattern, tail))
    mandatory_tail = tail.split("if(TARGET", 1)[0]
    off_required = set(re.findall(pattern, mandatory_tail))
    if not off_required:
        raise ValueError("inspector-off header authority is not statically readable")
    return roots, off_required, off_allowed


def check(root: Path, installed_prefix: Path | None = None,
          inspector_mode: str = "on") -> list[str]:
    errors: list[str] = []
    try:
        policy = json.loads((root / POLICY).read_text())
        if set(policy) != {"schema_version", "documented_source_only"} or policy["schema_version"] != 1:
            raise ValueError("policy must have exactly schema_version=1 and documented_source_only")
        exceptions = policy["documented_source_only"]
        if not isinstance(exceptions, list) or len(exceptions) != len(set(exceptions)) or not all(
            isinstance(item, str) for item in exceptions
        ):
            raise ValueError("documented_source_only must be a unique string list")
        doxy_roots = _doxygen_roots(root)
        install_roots, inspector_off_required, inspector_off_allowed = _install_roots(root)
        documented = set().union(*(
            _headers(root, item, doxygen_excludes=True, include_templates=True)
            for item in doxy_roots
        ))
        installed_by_root = {
            item: _headers(
                root, item,
                HEADER_SUFFIXES if item == "inspect/include" else {".h", ".hpp"},
                pulp_only=True,
            )
            for item in install_roots
        }
        installed_on = set().union(*installed_by_root.values())
        if (root / GENERATED_TEMPLATE).is_file():
            generated_cmake = (root / "core/runtime/CMakeLists.txt").read_text()
            if "build_info.hpp.in" not in generated_cmake or "pulp/runtime/build_info.hpp" not in generated_cmake:
                raise ValueError("generated build_info header authority is not statically readable")
            installed_on.add(GENERATED_HEADER)
    except (OSError, json.JSONDecodeError, ValueError) as error:
        return [str(error)]

    exception_set = set(exceptions)
    for path in sorted(exception_set):
        if path not in documented:
            errors.append(f"stale documented_source_only exception (not documented): {path}")
        if path in installed_on:
            errors.append(f"stale documented_source_only exception (now installed): {path}")
    installed_documentable = {
        path for path in installed_on
        if not ({"src", "detail"} & set(Path(path).parts))
    }
    for path in sorted(installed_documentable - documented):
        errors.append(f"installed public header missing from Doxygen: {path}")
    for path in sorted(documented - installed_documentable - exception_set):
        errors.append(f"Doxygen public header is not installed: {path}")
    if not inspector_off_allowed <= installed_on:
        errors.append("inspector-off authority contains headers absent from inspector-on authority")

    if installed_prefix is not None:
        include = installed_prefix / "include" / "pulp"
        if not include.is_dir():
            errors.append(f"installed prefix has no include/pulp directory: {installed_prefix}")
        else:
            symlinks = [path for path in include.rglob("*") if path.is_symlink()]
            if include.is_symlink() or symlinks:
                shown = include if include.is_symlink() else symlinks[0]
                errors.append(f"installed prefix contains a symlink: {shown}")
                return errors
            live = {
                "pulp/" + path.relative_to(include).as_posix()
                for path in include.rglob("*")
                if path.is_file() and path.suffix in HEADER_SUFFIXES
            }
            selected = set(installed_on)
            if inspector_mode == "off":
                selected -= installed_by_root["inspect/include"]
                required = set(selected) | inspector_off_required
                selected |= inspector_off_allowed
            else:
                required = set(selected)
            authority = {
                path.split("/include/pulp/", 1)[1]
                for path in selected if "/include/pulp/" in path
            }
            authority = {"pulp/" + path for path in authority}
            for path in sorted(live - authority):
                errors.append(f"installed prefix contains unsupported public header: {path}")
            required_authority = {
                "pulp/" + path.split("/include/pulp/", 1)[1]
                for path in required if "/include/pulp/" in path
            }
            for path in sorted(required_authority - live):
                errors.append(f"installed prefix is missing authority header: {path}")
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--installed-prefix", type=Path)
    parser.add_argument("--inspector-mode", choices=("on", "off"), default="on")
    parser.add_argument("--check", action="store_true",
                        help="compatibility no-op; checking is always performed")
    args = parser.parse_args(argv)
    errors = check(args.repo_root.resolve(), args.installed_prefix, args.inspector_mode)
    if errors:
        print("doxygen installed-header parity: FAILED", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return 1
    print("doxygen installed-header parity: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Canonical CTest inventory and build-target projection for changed surfaces.

CTest permits duplicate test names.  A name set therefore cannot describe the
suite that the authoritative command will execute.  This module retains every
filtered registration as a canonical composite, groups byte-identical
composites as a multiset, and makes ambiguity a fail-closed result.

The build-target projection at the end of the module is the developer-loop
side of the same machinery: it maps a working diff to the CMake targets that
own it (file-API codemodel), the test programs that exercise them, and the
CTest tests to run, following ``add_dependencies`` and CTest fixture edges.
Shipyard's exact-head plan stays the merge authority; ``pulp build``,
``pulp dev``, ``pulp loop``, ``pulp test``, and ``pulp affected`` consume the
projection through ``tools/scripts/affected_targets.py``.
"""

from __future__ import annotations

import functools
import hashlib
import json
import math
import os
import platform
import re
import subprocess
import unicodedata
from collections import Counter
from dataclasses import dataclass, field
from decimal import Decimal
from pathlib import Path
from shutil import which
from typing import Any, Iterable


INVENTORY_SCHEMA = "pulp.changed-surface-ctest-inventory/v1"
REGISTRATION_DOMAIN = b"pulp-ctest-registration-v1\0"
INVENTORY_DOMAIN = b"pulp-ctest-inventory-v1\0"
DIGEST_DOMAIN = b"pulp-changed-surface-contract-v1\0"
EXCLUDED_NAME_REGEX = "AudioWorkgroup"
EXCLUDED_LABEL_REGEX = "validation|slow|performance|bench|quality-lab|gpu-proof"
NOT_BUILT_PLACEHOLDER = re.compile(
    r"^(?P<target>[A-Za-z0-9_.+:-]+)_NOT_BUILT-(?P<suffix>[0-9a-f]{7,64})$"
)


class InventoryError(ValueError):
    """The inventory cannot safely authorize bounded selection."""


def split_proven_unbuilt_placeholders(
    tests: Iterable[dict[str, Any]], build_dir: Path
) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    """Separate only provenance-backed commandless CTest registrations.

    ``catch_discover_tests`` registers a commandless ``*_NOT_BUILT-*`` test
    until its producer target has run.  A name match alone is not authority:
    require CTest's exact one-property shape and the exact generated include
    file in that working directory.  A registration the authoritative filter
    excludes is skipped; every other missing or malformed command remains an
    inventory error.
    """

    build_root = build_dir.resolve(strict=True)
    ready: list[dict[str, Any]] = []
    placeholders: list[dict[str, Any]] = []
    cmake_artifacts: set[Path] | None = None
    ctest_files: list[str] | None = None
    for test in tests:
        command = test.get("command")
        if (
            isinstance(command, list)
            and command
            and all(isinstance(token, str) for token in command)
        ):
            ready.append(test)
            continue
        if excluded_from_inventory(test):
            # Never selected or compared, so its command is not the inventory's
            # to prove: a validator a host lacks lists no command at all.
            continue
        name = test.get("name")
        match = (
            NOT_BUILT_PLACEHOLDER.fullmatch(name) if isinstance(name, str) else None
        )
        properties = test.get("properties")
        if match is not None and set(test) == {"name", "properties"}:
            if not isinstance(properties, list) or len(properties) != 1:
                raise InventoryError(
                    f"CTest registration {name!r} has no unambiguous command"
                )
            working_property = properties[0]
            if (
                not isinstance(working_property, dict)
                or set(working_property) != {"name", "value"}
                or working_property.get("name") != "WORKING_DIRECTORY"
                or not isinstance(working_property.get("value"), str)
            ):
                raise InventoryError(
                    f"CTest registration {name!r} has no unambiguous command"
                )
            try:
                working_dir = Path(working_property["value"]).resolve(strict=True)
            except OSError as error:
                raise InventoryError(
                    f"CTest registration {name!r} has no unambiguous command"
                ) from error
            if not working_dir.is_relative_to(build_root):
                raise InventoryError(
                    f"CTest registration {name!r} has no unambiguous command"
                )
            stem = f"{match.group('target')}-{match.group('suffix')}"
            include_path = working_dir / f"{stem}_include.cmake"
            tests_path = working_dir / f"{stem}_tests.cmake"
            expected = (
                f'if(EXISTS "{tests_path}")\n'
                f'  include("{tests_path}")\n'
                "else()\n"
                f"  add_test({name} {name})\n"
                "endif()\n"
            )
            try:
                include_text = include_path.read_text(encoding="utf-8", errors="strict")
            except OSError as error:
                raise InventoryError(
                    f"CTest registration {name!r} has no unambiguous command"
                ) from error
            if tests_path.exists() or include_text != expected:
                raise InventoryError(
                    f"CTest registration {name!r} has no unambiguous command"
                )
            placeholders.append(test)
            continue

        # Plain add_test registrations also lose their command in CTest's JSON
        # until the CMake-produced executable exists.  Prove this second shape
        # against both generated CTestTestfile syntax and the exact codemodel.
        if (
            not isinstance(name, str)
            or set(test) != {"backtrace", "name", "properties"}
            or not isinstance(test.get("backtrace"), int)
        ):
            raise InventoryError(
                f"CTest registration {name!r} has no unambiguous command"
            )
        property_records = _property_records(test)
        working = [
            record["value"]
            for record in property_records
            if record["name"] == "WORKING_DIRECTORY"
        ]
        if len(working) != 1 or not isinstance(working[0], str):
            raise InventoryError(
                f"CTest registration {name!r} has no unambiguous command"
            )
        if cmake_artifacts is None:
            cmake_artifacts = _cmake_artifact_paths(build_root)
            ctest_files = [
                path.read_text(encoding="utf-8", errors="strict")
                for path in build_root.rglob("CTestTestfile.cmake")
            ]
        pattern = re.compile(
            r"^add_test\(\[=\["
            + re.escape(name)
            + r"\]=\] \"(?P<executable>[^\"\n]+)\"(?: .*)?\)$",
            re.MULTILINE,
        )
        matches = [
            executable
            for contents in ctest_files or []
            for executable in pattern.findall(contents)
        ]
        if len(matches) != 1:
            raise InventoryError(f"CTest registration {name!r} has no unambiguous command")
        executable = Path(matches[0]).resolve()
        if executable.exists() or executable not in cmake_artifacts:
            raise InventoryError(f"CTest registration {name!r} has no unambiguous command")
        placeholders.append(test)
    return ready, placeholders


def _cmake_artifact_paths(build_root: Path) -> set[Path]:
    try:
        model = load_codemodel_targets(build_root)
    except InventoryError as error:
        raise InventoryError(
            "CMake codemodel cannot prove commandless registrations"
        ) from error
    return {Path(path).resolve() for target in model.targets.values() for path in target.artifacts}


def _unicode_key(value: str) -> bytes:
    _validate_string(value)
    return value.encode("utf-16-be")


def _validate_string(value: str) -> None:
    if any(0xD800 <= ord(character) <= 0xDFFF for character in value):
        raise InventoryError("unpaired Unicode surrogate is not canonical JSON")


def _jcs_number(value: int | float) -> str:
    if isinstance(value, bool):
        raise TypeError("booleans are not numbers")
    if isinstance(value, int):
        return str(value)
    if not math.isfinite(value):
        raise InventoryError("non-finite JSON number")
    if value == 0:
        return "0"
    negative = value < 0
    absolute = -value if negative else value
    shortest = repr(absolute)
    if 1e-6 <= absolute < 1e21:
        rendered = format(Decimal(shortest), "f")
        if "." in rendered:
            rendered = rendered.rstrip("0").rstrip(".")
    else:
        mantissa, exponent = shortest.lower().split("e")
        if mantissa.endswith(".0"):
            mantissa = mantissa[:-2]
        exponent_value = int(exponent)
        rendered = f"{mantissa}e{'+' if exponent_value >= 0 else ''}{exponent_value}"
    return f"-{rendered}" if negative else rendered


def canonical_json(value: Any) -> bytes:
    """Return RFC 8785-style canonical JSON for the CTest value subset."""

    def encode(item: Any) -> str:
        if item is None:
            return "null"
        if item is True:
            return "true"
        if item is False:
            return "false"
        if isinstance(item, (int, float)):
            return _jcs_number(item)
        if isinstance(item, str):
            _validate_string(item)
            return json.dumps(item, ensure_ascii=False, separators=(",", ":"))
        if isinstance(item, list):
            return "[" + ",".join(encode(element) for element in item) + "]"
        if isinstance(item, dict):
            if not all(isinstance(key, str) for key in item):
                raise InventoryError("canonical JSON object keys must be strings")
            ordered = sorted(item, key=_unicode_key)
            return "{" + ",".join(f"{encode(key)}:{encode(item[key])}" for key in ordered) + "}"
        raise InventoryError(f"unsupported canonical JSON type: {type(item).__name__}")

    return encode(value).encode("utf-8")


def contract_digest(value: Any) -> str:
    return hashlib.sha256(DIGEST_DOMAIN + canonical_json(value)).hexdigest()


def authoritative_filter_contract() -> dict[str, Any]:
    return {
        "excluded_label_regex": EXCLUDED_LABEL_REGEX,
        "excluded_name_regex": EXCLUDED_NAME_REGEX,
        "label_property": "LABELS",
    }


def authoritative_filter_digest() -> str:
    return contract_digest(authoritative_filter_contract())


def _property_records(test: dict[str, Any]) -> list[dict[str, Any]]:
    properties = test.get("properties", [])
    if not isinstance(properties, list):
        raise InventoryError("CTest properties must be a list")
    records: list[dict[str, Any]] = []
    names: set[str] = set()
    for prop in properties:
        if not isinstance(prop, dict) or set(prop) != {"name", "value"}:
            raise InventoryError("CTest property must contain exactly name and value")
        name = prop["name"]
        if not isinstance(name, str) or not name:
            raise InventoryError("CTest property name must be a nonempty string")
        if name in names:
            raise InventoryError(f"duplicate CTest property {name!r}")
        names.add(name)
        canonical_json(prop["value"])
        records.append({"name": name, "value": prop["value"]})
    return records


def excluded_from_inventory(test: dict[str, Any]) -> bool:
    """Whether the authoritative filter drops this registration by name or label."""
    name = test.get("name")
    if not isinstance(name, str) or not name:
        raise InventoryError("CTest registration name must be a nonempty string")
    if "\n" in name or "\r" in name:
        raise InventoryError("CTest registration names cannot contain newlines")
    labels: list[str] = []
    for prop in _property_records(test):
        if prop["name"] != "LABELS":
            continue
        value = prop["value"]
        values = value if isinstance(value, list) else [value]
        if not all(isinstance(label, str) for label in values):
            raise InventoryError("CTest LABELS must contain strings")
        labels.extend(values)
    return bool(re.search(EXCLUDED_NAME_REGEX, name)) or any(
        re.search(EXCLUDED_LABEL_REGEX, label) for label in labels)


def authoritative_tests(tests: Iterable[dict[str, Any]]) -> list[dict[str, Any]]:
    return [test for test in tests if not excluded_from_inventory(test)]


def _path_anchor(path: str, source_root: Path, build_dir: Path) -> str | None:
    if not os.path.isabs(path) or any(character.isspace() for character in path):
        return None
    normalized = os.path.normpath(path)
    source = os.path.normpath(str(source_root))
    build = os.path.normpath(str(build_dir))

    def under(candidate: str, root: str) -> str | None:
        try:
            common = os.path.commonpath([candidate, root])
        except ValueError:
            return None
        if common != root:
            return None
        relative = os.path.relpath(candidate, root).replace(os.sep, "/")
        return "." if relative == "." else relative

    build_relative = under(normalized, build)
    if build_relative is not None:
        if build_relative.startswith("_deps/"):
            return f"external:{Path(normalized).name}"
        return f"build:{build_relative}"
    source_relative = under(normalized, source)
    if source_relative is not None:
        if source_relative.startswith("external/"):
            return f"external:{Path(normalized).name}"
        return f"source:{source_relative}"
    return f"external:{Path(normalized).name}"


def _normalize_scalar(value: str, source_root: Path, build_dir: Path) -> str:
    anchored = _path_anchor(value, source_root, build_dir)
    if anchored is not None:
        return anchored
    if "=" in value:
        key, possible_path = value.split("=", 1)
        anchored = _path_anchor(possible_path, source_root, build_dir)
        if anchored is not None:
            return f"{key}={anchored}"
    # Some CTest APIs intentionally carry a command as one argv value (for
    # example, an --emit-cmd argument). Normalize only whitespace-delimited
    # absolute path words. This is boundary-aware lexical anchoring, not a
    # substring replacement or a second shell-unescape pass.
    def replace_path_word(match: re.Match[str]) -> str:
        anchored_word = _path_anchor(match.group("path"), source_root, build_dir)
        if anchored_word is None:
            return match.group(0)
        return f"{match.group('prefix')}{anchored_word}"

    normalized = re.sub(
        r"(?P<prefix>(?<!\S)|=)(?P<path>/[^\s]+)", replace_path_word, value
    )
    if normalized != value:
        return normalized
    source_text = os.path.normpath(str(source_root))
    build_text = os.path.normpath(str(build_dir))
    if source_text in value or build_text in value:
        raise InventoryError("host path appears in a non-structural string")
    return value


def _normalize_value(value: Any, source_root: Path, build_dir: Path) -> Any:
    if isinstance(value, str):
        return _normalize_scalar(value, source_root, build_dir)
    if isinstance(value, list):
        return [_normalize_value(item, source_root, build_dir) for item in value]
    if isinstance(value, dict):
        return {
            key: _normalize_value(item, source_root, build_dir)
            for key, item in value.items()
        }
    canonical_json(value)
    return value


def registration_composite(
    test: dict[str, Any], source_root: Path, build_dir: Path
) -> dict[str, Any]:
    name = test.get("name")
    if not isinstance(name, str) or not name:
        raise InventoryError("CTest registration name must be a nonempty string")
    name = unicodedata.normalize("NFC", name)
    if "\n" in name or "\r" in name:
        raise InventoryError("CTest registration names cannot contain newlines")
    command = test.get("command")
    if not isinstance(command, list) or not command or not all(
        isinstance(token, str) for token in command
    ):
        raise InventoryError(f"CTest registration {name!r} has no unambiguous command")
    properties = _property_records(test)
    working = [prop["value"] for prop in properties if prop["name"] == "WORKING_DIRECTORY"]
    if len(working) != 1 or not isinstance(working[0], str):
        raise InventoryError(
            f"CTest registration {name!r} requires one string WORKING_DIRECTORY"
        )
    executable = _path_anchor(command[0], source_root, build_dir)
    if executable is None:
        if os.sep in command[0] or (os.altsep and os.altsep in command[0]):
            raise InventoryError(f"relative executable path is ambiguous for {name!r}")
        executable = f"external:{command[0]}"
    normalized_properties = [
        {
            "name": prop["name"],
            "value": _normalize_value(prop["value"], source_root, build_dir),
        }
        for prop in properties
    ]
    normalized_properties.sort(
        key=lambda prop: (prop["name"], canonical_json(prop))
    )
    return {
        "name": name,
        "executable": executable,
        "argv": [
            _normalize_scalar(token, source_root, build_dir) for token in command[1:]
        ],
        "working_directory": _normalize_scalar(working[0], source_root, build_dir),
        "properties": normalized_properties,
    }


def _fingerprint(composite: dict[str, Any]) -> str:
    return hashlib.sha256(REGISTRATION_DOMAIN + canonical_json(composite)).hexdigest()


def inventory_groups(
    tests: Iterable[dict[str, Any]], source_root: Path, build_dir: Path
) -> list[dict[str, Any]]:
    by_composite: dict[bytes, tuple[dict[str, Any], int]] = {}
    for test in authoritative_tests(tests):
        composite = registration_composite(test, source_root, build_dir)
        encoded = canonical_json(composite)
        prior = by_composite.get(encoded)
        by_composite[encoded] = (composite, 1 if prior is None else prior[1] + 1)
    groups = [
        {
            "fingerprint": _fingerprint(composite),
            "multiplicity": multiplicity,
            "composite": composite,
        }
        for composite, multiplicity in by_composite.values()
    ]
    groups.sort(key=lambda group: (group["fingerprint"], canonical_json(group["composite"])))
    return groups


def _git_value(source_root: Path, *args: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(source_root), *args],
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout.strip()


def _absolute_path_words(value: str) -> list[str]:
    if _path_like_absolute(value):
        return [value]
    paths: list[str] = []
    if "=" in value:
        possible_path = value.split("=", 1)[1]
        if _path_like_absolute(possible_path):
            paths.append(possible_path)
    paths.extend(
        match.group("path")
        for match in re.finditer(r"(?P<prefix>(?<!\S)|=)(?P<path>/[^\s]+)", value)
    )
    return paths


def _path_like_absolute(value: str) -> bool:
    return os.path.isabs(value) and not any(character.isspace() for character in value)


def _external_resolution_contract(
    tests: Iterable[dict[str, Any]], source_root: Path, build_dir: Path
) -> list[dict[str, Any]]:
    paths: set[str] = set()

    def collect(value: Any) -> None:
        if isinstance(value, str):
            for candidate in _absolute_path_words(value):
                anchored = _path_anchor(candidate, source_root, build_dir)
                if anchored is not None and anchored.startswith("external:"):
                    paths.add(os.path.normpath(candidate))
        elif isinstance(value, list):
            for item in value:
                collect(item)
        elif isinstance(value, dict):
            for item in value.values():
                collect(item)

    for test in tests:
        collect(test.get("command", []))
        collect(test.get("properties", []))

    records: list[dict[str, Any]] = []
    for raw_path in sorted(paths):
        record: dict[str, Any] = {
            "basename": Path(raw_path).name,
            "raw_path": raw_path,
            "resolved_path": os.path.realpath(raw_path),
        }
        try:
            stat = os.stat(raw_path)
        except OSError:
            record["state"] = "unavailable"
        else:
            record["size"] = stat.st_size
            record["state"] = "regular" if os.path.isfile(raw_path) else "other"
            if os.path.isfile(raw_path) and stat.st_size <= 32 * 1024 * 1024:
                digest = hashlib.sha256()
                with open(raw_path, "rb") as external_file:
                    for chunk in iter(lambda: external_file.read(1024 * 1024), b""):
                        digest.update(chunk)
                record["sha256"] = digest.hexdigest()
        records.append(record)
    return records


def _toolchain_contract(
    build_dir: Path,
    groups: list[dict[str, Any]],
    tests: Iterable[dict[str, Any]],
    source_root: Path,
) -> dict[str, Any]:
    cache: dict[str, str] = {}
    cache_path = build_dir / "CMakeCache.txt"
    if cache_path.is_file():
        wanted = {
            "CMAKE_BUILD_TYPE",
            "CMAKE_CXX_COMPILER",
            "CMAKE_GENERATOR",
            "CMAKE_OSX_ARCHITECTURES",
            "CMAKE_OSX_DEPLOYMENT_TARGET",
        }
        for line in cache_path.read_text(encoding="utf-8", errors="strict").splitlines():
            if ":" not in line or "=" not in line:
                continue
            key = line.split(":", 1)[0]
            if key in wanted:
                cache[key] = line.split("=", 1)[1]
    compiler = cache.get("CMAKE_CXX_COMPILER", "")
    if compiler:
        cache["CMAKE_CXX_COMPILER"] = f"external:{Path(compiler).name}"
    external_tools: set[str] = set()

    def collect_external(value: Any) -> None:
        if isinstance(value, str):
            external_tools.update(re.findall(r"(?:^|\s)(external:[^\s]+)", value))
        elif isinstance(value, list):
            for item in value:
                collect_external(item)
        elif isinstance(value, dict):
            for item in value.values():
                collect_external(item)

    for group in groups:
        collect_external(group["composite"])
    return {
        "cache": cache,
        "ctest_version": subprocess.run(
            ["ctest", "--version"], check=True, capture_output=True, text=True
        ).stdout.splitlines()[0],
        "external_resolutions": _external_resolution_contract(
            tests, source_root, build_dir
        ),
        "external_tools": sorted(external_tools),
        "machine": platform.machine(),
        "python_version": platform.python_version(),
        "system": platform.system(),
    }


def build_manifest(
    tests: list[dict[str, Any]],
    source_root: Path,
    build_dir: Path,
    policy: dict[str, Any],
    *,
    repository: str = "Generous-Corp/pulp",
    target: str = "mac",
) -> dict[str, Any]:
    filtered_tests = authoritative_tests(tests)
    groups = inventory_groups(filtered_tests, source_root, build_dir)
    registration_count = sum(group["multiplicity"] for group in groups)
    names = Counter(
        group["composite"]["name"]
        for group in groups
        for _ in range(group["multiplicity"])
    )
    duplicate_composites = [group for group in groups if group["multiplicity"] > 1]
    filter_digest = authoritative_filter_digest()
    digest_body = {"authoritative_filter_digest": filter_digest, "groups": groups}
    inventory_digest = hashlib.sha256(
        INVENTORY_DOMAIN + canonical_json(digest_body)
    ).hexdigest()
    target_contract = {
        "build_flags": policy["build_flags"],
        "build_type": policy["build_type"],
        "target": target,
    }
    toolchain = _toolchain_contract(build_dir, groups, filtered_tests, source_root)
    return {
        "schema": INVENTORY_SCHEMA,
        "schema_version": 1,
        "repository": repository,
        "target": target,
        "source_head_sha": _git_value(source_root, "rev-parse", "HEAD"),
        "source_tree_sha": _git_value(source_root, "rev-parse", "HEAD^{tree}"),
        "build_type": policy["build_type"],
        "target_contract_digest": contract_digest(target_contract),
        "toolchain_digest": contract_digest(toolchain),
        "authoritative_filter_digest": filter_digest,
        "registration_count": registration_count,
        "unique_name_count": len(names),
        "unique_composite_count": len(groups),
        "duplicate_name_group_count": sum(count > 1 for count in names.values()),
        "duplicate_name_excess_count": sum(count - 1 for count in names.values() if count > 1),
        "duplicate_composite_group_count": len(duplicate_composites),
        "inventory_digest": inventory_digest,
        "groups": groups,
    }


def require_unambiguous(manifest: dict[str, Any]) -> None:
    """Two registrations with one composite identity cannot be told apart, so
    no literal selection over them is safe."""
    if manifest["duplicate_composite_group_count"]:
        raise InventoryError("duplicate composite registration identity is ambiguous")


def expand_literal_selection(
    manifest: dict[str, Any], selected_names: Iterable[str]
) -> list[dict[str, Any]]:
    requested = list(selected_names)
    if len(requested) != len(set(requested)):
        raise InventoryError("literal selection contains duplicate requested names")
    if manifest.get("duplicate_composite_group_count"):
        raise InventoryError("ambiguous composite inventory requires full suite")
    selected = set(requested)
    expanded = [
        group for group in manifest["groups"] if group["composite"]["name"] in selected
    ]
    found = {group["composite"]["name"] for group in expanded}
    missing = sorted(selected - found)
    if missing:
        raise InventoryError(f"literal selection names absent from inventory: {missing}")
    if any(group["multiplicity"] != 1 for group in expanded):
        raise InventoryError("ambiguous selected registration requires full suite")
    return expanded


class CTestTests(list):
    """The ``tests`` array of a ctest listing, carrying the document's call-stack
    graph so a caller can ask which CMake file registered a test."""

    backtrace_graph: Any = None


def load_ctest_json(build_dir: Path) -> list[dict[str, Any]]:
    result = subprocess.run(
        ["ctest", "--test-dir", str(build_dir), "--show-only=json-v1"],
        check=True,
        capture_output=True,
        text=True,
    )
    payload = json.loads(result.stdout)
    tests = payload.get("tests")
    if not isinstance(tests, list):
        raise InventoryError("CTest JSON has no tests array")
    listing = CTestTests(tests)
    listing.backtrace_graph = payload.get("backtraceGraph")
    return listing


def source_root_for_build(build_dir: Path) -> Path:
    cache = build_dir / "CMakeCache.txt"
    if not cache.is_file():
        raise InventoryError(f"missing CMake cache: {cache}")
    for line in cache.read_text(encoding="utf-8", errors="strict").splitlines():
        if line.startswith("CMAKE_HOME_DIRECTORY:INTERNAL="):
            root = Path(line.split("=", 1)[1])
            if not root.is_absolute():
                raise InventoryError("CMAKE_HOME_DIRECTORY is not absolute")
            return root
    raise InventoryError("CMake cache has no CMAKE_HOME_DIRECTORY")


# ── build-target projection ─────────────────────────────────────────────────
#
# Everything below maps a working diff to CMake targets and CTest tests for the
# developer loop. It is deliberately best-effort where the exact-head plan
# above is fail-closed: an unmappable diff widens to ``all`` instead of
# refusing, because the caller is a build command, not a merge gate.

PROJECTION_SCHEMA = "pulp.affected-targets/v1"
DEFAULT_PROJECTION_THRESHOLD = 0.4
CODEMODEL_QUERY_RELATIVE = Path(".cmake") / "api" / "v1" / "query" / "codemodel-v2"
CODEMODEL_REPLY_RELATIVE = Path(".cmake") / "api" / "v1" / "reply"

SOURCE_EXTENSIONS = {".c", ".cc", ".cpp", ".cxx", ".m", ".mm", ".s", ".S"}
HEADER_EXTENSIONS = {".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp", ".tpp"}
BUILD_SYSTEM_NAMES = {"CMakeLists.txt", "CMakePresets.json", "CMakeUserPresets.json"}
LIBRARY_TARGET_TYPES = {"STATIC_LIBRARY", "OBJECT_LIBRARY", "INTERFACE_LIBRARY"}
TEST_SOURCE_RE = re.compile(r"^test_(?P<stem>.+)$")


@dataclass
class Target:
    """One codemodel target: enough of the file-API record to project a diff."""

    name: str
    type: str
    source_dir: str
    build_dir: str
    sources: list[str]
    dependencies: list[str] = field(default_factory=list)
    artifacts: list[str] = field(default_factory=list)
    # Every CMake file on the call stack that defined the target (absolute,
    # normalized): the file holding add_executable and the files that
    # include() or call into it.
    definition_files: frozenset[str] = frozenset()

    @property
    def is_test(self) -> bool:
        if self.type != "EXECUTABLE":
            return False
        if self.name.startswith("pulp-test-"):
            return True
        return any(s == "test" or s.startswith("test/") for s in self.sources)

    @property
    def is_library(self) -> bool:
        return self.type in LIBRARY_TARGET_TYPES


@dataclass
class CodeModel:
    targets: dict[str, Target]
    source_root: str
    build_root: str

    @property
    def total(self) -> int:
        return len(self.targets)


@dataclass
class CTestEntry:
    name: str
    command: list[str]
    properties: dict[str, Any]
    # Every CMake file on the call stack that registered the test (absolute,
    # normalized). Empty for a Catch2 case ctest reads from a discovery file.
    registered_files: frozenset[str] = frozenset()


# A test names a file in more forms than a bare argument: a CMake script takes
# it as -D<VAR>=<path> (a `|` or `;` list when it takes several), a tool as
# --flag=<path>, and `cmake -E env` as NAME=<path>.
_ASSIGNED_VALUE = re.compile(r"^(?:-D|--?)?[A-Za-z_][\w.-]*(?::[A-Za-z]+)?=(.+)$", re.DOTALL)
_PATH_LIST_SEPARATOR = re.compile(r"[|;]")
# A validator or link-floor probe often takes a bundle directory, while the
# file API records the binary inside it as the target's artifact.
BUNDLE_SUFFIXES = (".app", ".appex", ".aaxplugin", ".bundle", ".clap", ".component",
                   ".framework", ".lv2", ".vst3")


def command_path_refs(command: Iterable[str]) -> list[str]:
    """Every path a test command names: each argument, plus the value of each
    ``-DVAR=``, ``--flag=`` or ``NAME=`` assignment, split on list separators.

    Values are normalized; whether one is a build output or a source file is
    the caller's lookup."""
    refs: list[str] = []
    for token in command:
        if not token:
            continue
        refs.append(os.path.normpath(token))
        assigned = _ASSIGNED_VALUE.match(token)
        if assigned:
            refs.extend(os.path.normpath(value)
                        for value in _PATH_LIST_SEPARATOR.split(assigned.group(1)) if value)
    return refs


def bundle_dirs(artifact: str) -> list[str]:
    """The bundle directories an artifact sits inside, innermost first."""
    found: list[str] = []
    parent = os.path.dirname(artifact)
    while parent and parent != os.path.dirname(parent):
        if parent.endswith(BUNDLE_SUFFIXES):
            found.append(parent)
        parent = os.path.dirname(parent)
    return found


def artifact_index(model: "CodeModel") -> dict[str, set[str]]:
    """Path -> owning targets, for every artifact and every bundle directory
    that contains one."""
    index: dict[str, set[str]] = {}
    for target in model.targets.values():
        for artifact in target.artifacts:
            index.setdefault(artifact, set()).add(target.name)
            for bundle in bundle_dirs(artifact):
                index.setdefault(bundle, set()).add(target.name)
    return index


# A test that reads what CMake records about a target (a generated link or
# flags file, a boundary over its headers) names no artifact of it. Its
# registration declares the targets instead:
#   set_tests_properties(<test> PROPERTIES PULP_SELECT_WITH_TARGETS "a;b")
SELECT_WITH_TARGETS_PROPERTY = "PULP_SELECT_WITH_TARGETS"


def declared_targets(entry: "CTestEntry") -> set[str]:
    values: set[str] = set()
    for value in _as_list(entry.properties.get(SELECT_WITH_TARGETS_PROPERTY)):
        values.update(v for v in value.split(";") if v)
    return values


def command_owners(command: Iterable[str], index: dict[str, set[str]]) -> set[str]:
    """Targets whose build output (or its bundle) a test command names."""
    owners: set[str] = set()
    for ref in command_path_refs(command):
        owners |= index.get(ref, set())
    return owners


@dataclass
class Selection:
    mode: str
    reason: str
    changed_files: list[str]
    targets: list[str]
    tests: list[str]
    total_targets: int
    total_tests: int
    unmapped: list[str]
    threshold: float

    @property
    def banner(self) -> str:
        if self.mode == "focused" and not self.targets:
            return (
                f"FOCUSED: nothing to build for your diff ({len(self.tests)} tests selected) "
                "- run 'pulp build --all' before opening a PR"
            )
        if self.mode == "focused":
            return (
                f"FOCUSED: building {len(self.targets)}/{self.total_targets} targets "
                "affected by your diff - run 'pulp build --all' before opening a PR"
            )
        return f"FULL: building all {self.total_targets} targets ({self.reason})"

    def to_json(self) -> dict[str, Any]:
        return {
            "schema": PROJECTION_SCHEMA,
            "mode": self.mode,
            "reason": self.reason,
            "banner": self.banner,
            "changed_files": self.changed_files,
            "targets": self.targets,
            "tests": self.tests,
            "total_targets": self.total_targets,
            "total_tests": self.total_tests,
            "unmapped": self.unmapped,
            "threshold": self.threshold,
        }


def all_selection(reason: str, changed: Iterable[str], total_targets: int, total_tests: int,
                  threshold: float, unmapped: Iterable[str] = ()) -> Selection:
    return Selection(
        mode="all",
        reason=reason,
        changed_files=sorted(changed),
        targets=[],
        tests=[],
        total_targets=total_targets,
        total_tests=total_tests,
        unmapped=sorted(unmapped),
        threshold=threshold,
    )


# ── CMake file API ──


def ensure_codemodel_query(build_dir: Path) -> bool:
    """Write the stateless codemodel query. Returns True when newly created."""
    query = build_dir / CODEMODEL_QUERY_RELATIVE
    if query.exists():
        return False
    query.parent.mkdir(parents=True, exist_ok=True)
    query.touch()
    return True


def codemodel_reply_available(build_dir: Path) -> bool:
    reply = build_dir / CODEMODEL_REPLY_RELATIVE
    return reply.is_dir() and any(reply.glob("index-*.json"))


def codemodel_reply_mtime(build_dir: Path) -> float | None:
    """Modification time of the file-API reply index, or None."""
    indexes = sorted((build_dir / CODEMODEL_REPLY_RELATIVE).glob("index-*.json"))
    if not indexes:
        return None
    try:
        return indexes[-1].stat().st_mtime
    except OSError:
        return None


def backtrace_files(graph: Any, index: Any, root: str = "") -> frozenset[str]:
    """The files on the call stack a file-API or ctest ``backtrace`` index names.

    Both CMake JSON documents share one shape: ``nodes`` (``file`` index,
    ``parent`` index) and ``files``. A stack runs from the add_executable or
    add_test call outward through every include() and function call.
    """
    if not isinstance(graph, dict) or not isinstance(index, int):
        return frozenset()
    nodes, files = graph.get("nodes") or [], graph.get("files") or []
    found: set[str] = set()
    seen: set[int] = set()
    while isinstance(index, int) and 0 <= index < len(nodes) and index not in seen:
        seen.add(index)
        node = nodes[index]
        slot = node.get("file")
        if isinstance(slot, int) and 0 <= slot < len(files) and isinstance(files[slot], str):
            # The file API records paths relative to the source root; ctest, absolute.
            found.add(os.path.normpath(os.path.join(root, files[slot])))
        index = node.get("parent")
    return frozenset(found)


def load_codemodel_targets(build_dir: Path) -> CodeModel:
    """Parse the file-API reply into a name-keyed target graph.

    The reply must hold exactly one index: two indexes mean two configures
    raced, and neither can be proven current. Both the exact-head inventory
    (``_cmake_artifact_paths``) and the projection read through here.
    """
    reply = build_dir / CODEMODEL_REPLY_RELATIVE
    indexes = sorted(reply.glob("index-*.json"))
    if len(indexes) != 1:
        raise InventoryError(f"CMake file-API reply must contain one index, found {len(indexes)}")
    try:
        index = json.loads(indexes[0].read_text(encoding="utf-8"))
        codemodel_name = index["reply"]["codemodel-v2"]["jsonFile"]
        codemodel = json.loads((reply / codemodel_name).read_text(encoding="utf-8"))
        configurations = codemodel["configurations"]
        if not isinstance(configurations, list) or len(configurations) != 1:
            raise KeyError("configurations")
        references = configurations[0]["targets"]
        paths = codemodel.get("paths", {})
        source_root = os.path.normpath(paths.get("source", str(build_dir.parent)))
        build_root = os.path.normpath(paths.get("build", str(build_dir)))
        # Keyed by the file-API target id when present (dependencies refer to
        # it); a reference without one still contributes its target record.
        by_id: dict[str, dict[str, Any]] = {}
        for reference in references:
            key = reference.get("id") or reference["jsonFile"]
            by_id[key] = json.loads(
                (reply / reference["jsonFile"]).read_text(encoding="utf-8")
            )
    except (OSError, KeyError, TypeError, json.JSONDecodeError) as error:
        raise InventoryError(f"CMake file-API codemodel is unavailable: {error}") from error

    targets: dict[str, Target] = {}
    for raw in by_id.values():
        name = raw.get("name")
        if not isinstance(name, str) or not name:
            raise InventoryError("CMake file-API target has no canonical name")
        deps = []
        for dep in raw.get("dependencies", []):
            dep_raw = by_id.get(dep.get("id", "")) if isinstance(dep, dict) else None
            if dep_raw is not None:
                deps.append(dep_raw["name"])
        artifacts = []
        for artifact in raw.get("artifacts", []):
            path = artifact.get("path", "") if isinstance(artifact, dict) else ""
            if not isinstance(path, str) or not path:
                continue
            if not os.path.isabs(path):
                path = os.path.join(build_root, path)
            artifacts.append(os.path.normpath(path))
        targets[name] = Target(
            name=name,
            type=raw.get("type", ""),
            source_dir=os.path.normpath(raw.get("paths", {}).get("source", ".")),
            build_dir=os.path.normpath(raw.get("paths", {}).get("build", ".")),
            sources=[os.path.normpath(s["path"]) for s in raw.get("sources", [])],
            dependencies=deps,
            artifacts=artifacts,
            definition_files=backtrace_files(raw.get("backtraceGraph"), raw.get("backtrace"), source_root),
        )
    return CodeModel(targets=targets, source_root=source_root, build_root=build_root)


# ── working diff ──


def _git_lines(source_root: Path, *args: str) -> str | None:
    try:
        proc = subprocess.run(
            ["git", *args], cwd=source_root, capture_output=True, text=True, check=False
        )
    except OSError:
        return None
    return proc.stdout if proc.returncode == 0 else None


def changed_files(source_root: Path, base: str) -> tuple[list[str], list[str], str | None]:
    """Return (existing changed paths, deleted paths, warning) relative to the root.

    The set is the union of the branch diff against the merge-base with
    ``base``, staged and unstaged edits, and untracked files. Paths inside
    submodules are reported by git as the submodule directory and skipped.
    """
    warning = None
    merge_base = _git_lines(source_root, "merge-base", base, "HEAD")
    names: set[str] = set()
    if merge_base:
        committed = _git_lines(source_root, "diff", "--name-only", merge_base.strip(), "HEAD")
        if committed is not None:
            names.update(committed.split("\n"))
    else:
        warning = f"cannot resolve merge-base with {base}; using uncommitted changes only"
    for extra in (("diff", "--name-only", "HEAD"), ("ls-files", "--others", "--exclude-standard")):
        listing = _git_lines(source_root, *extra)
        if listing is not None:
            names.update(listing.split("\n"))
    existing: list[str] = []
    deleted: list[str] = []
    for name in sorted(n for n in names if n):
        path = source_root / name
        if path.is_file():
            existing.append(os.path.normpath(name))
        elif not path.is_dir():
            deleted.append(os.path.normpath(name))
    return existing, deleted, warning


def is_build_system_file(rel: str) -> bool:
    name = os.path.basename(rel)
    return name in BUILD_SYSTEM_NAMES or name.endswith(".cmake") or name.endswith(".cmake.in")


def stale_build_system_files(source_root: Path, changed: list[str], deleted: list[str],
                             reply_time: float | None) -> list[str]:
    """Build-system files whose change postdates the codemodel reply (or all of
    them when there is no reply to compare against). A deleted build file is
    always stale: nothing records when it went away."""
    stale = [f for f in deleted if is_build_system_file(f)]
    for rel in changed:
        if not is_build_system_file(rel):
            continue
        if reply_time is None:
            stale.append(rel)
            continue
        try:
            if (source_root / rel).stat().st_mtime > reply_time:
                stale.append(rel)
        except OSError:
            stale.append(rel)
    return stale


# ── dependency database (header -> compiled objects) ──


def _target_from_object_path(obj: str) -> str | None:
    marker = ".dir/"
    idx = obj.find(marker)
    if idx < 0:
        return None
    head = obj[:idx]
    return head.rsplit("/", 1)[-1] if "/" in head else head


def _relativize(path: str, source_root: str, base_dir: str) -> str | None:
    """Source-root-relative form of a dependency path, which the generator
    writes relative to ``base_dir`` (the object's build directory) or absolute."""
    if not os.path.isabs(path):
        path = os.path.join(base_dir, path)
    path = os.path.normpath(path)
    try:
        rel = os.path.relpath(path, source_root)
    except ValueError:
        return None
    if rel.startswith(".."):
        return None
    return rel


def parse_ninja_deps(text: str, source_root: str, build_dir: str,
                     wanted: set[str]) -> dict[str, set[str]]:
    """Map the ``wanted`` source-relative headers to target names from ``ninja -t deps``."""
    out: dict[str, set[str]] = {}
    basenames = {os.path.basename(h) for h in wanted}
    target: str | None = None
    for line in text.splitlines():
        if not line.strip():
            target = None
            continue
        if not line.startswith((" ", "\t")):
            obj = line.split(":", 1)[0]
            target = _target_from_object_path(obj.replace("\\", "/"))
            continue
        if target is None:
            continue
        dep = line.strip()
        if os.path.basename(dep) not in basenames:
            continue
        rel = _relativize(dep, source_root, build_dir)
        if rel in wanted:
            out.setdefault(rel, set()).add(target)
    return out


def parse_make_depfile(text: str, target: str, source_root: str, base_dir: str,
                       wanted: set[str], out: dict[str, set[str]]) -> None:
    """Fold one compiler-generated ``.o.d`` file into the header map."""
    body = text.replace("\\\n", " ")
    for rule in body.split("\n"):
        if ":" not in rule:
            continue
        _, deps = rule.split(":", 1)
        for token in deps.split():
            rel = _relativize(token, source_root, base_dir)
            if rel in wanted:
                out.setdefault(rel, set()).add(target)


def _depfile_roots(build_dir: Path, model: CodeModel) -> list[Path]:
    roots: set[Path] = set()
    for target in model.targets.values():
        head = target.build_dir.split(os.sep, 1)[0]
        if head == "_deps":
            continue
        roots.add(build_dir / head if head not in ("", ".") else build_dir / "CMakeFiles")
    return sorted(r for r in roots if r.is_dir())


def _grep_depfiles(patterns: list[str], roots: list[Path]) -> list[str] | None:
    """Depfiles under ``roots`` containing any pattern. ripgrep walks a large
    build tree in well under a second; the portable fallback enumerates the
    depfiles first because a recursive grep stats every object in the tree."""
    if which("rg"):
        args = ["rg", "-l", "-F", "--glob", "*.d"]
        for pattern in patterns:
            args += ["-e", pattern]
        args += [str(r) for r in roots]
        try:
            proc = subprocess.run(args, capture_output=True, text=True, check=False)
        except OSError:
            proc = None
        if proc is not None and proc.returncode in (0, 1):
            return proc.stdout.splitlines()
    depfiles = [str(p) for r in roots for p in r.rglob("*.d")]
    if not depfiles:
        return []
    hits: list[str] = []
    chunk = 500
    for i in range(0, len(depfiles), chunk):
        args = ["grep", "-lF"]
        for pattern in patterns:
            args += ["-e", pattern]
        args += depfiles[i:i + chunk]
        try:
            proc = subprocess.run(args, capture_output=True, text=True, check=False)
        except OSError:
            return None
        if proc.returncode not in (0, 1):
            return None
        hits.extend(proc.stdout.splitlines())
    return hits


def header_owners(build_dir: Path, model: CodeModel, headers: set[str]) -> dict[str, set[str]] | None:
    """Targets whose compiled objects include each header, from the generator's
    dependency database (``ninja -t deps`` or Makefile ``.o.d`` files). Returns
    None when no database is available."""
    if not headers:
        return {}
    if (build_dir / "build.ninja").is_file():
        try:
            proc = subprocess.run(
                ["ninja", "-C", str(build_dir), "-t", "deps"],
                capture_output=True, text=True, check=False,
            )
        except OSError:
            return None
        if proc.returncode != 0 or not proc.stdout.strip():
            return None
        return parse_ninja_deps(proc.stdout, model.source_root, str(build_dir), headers)
    roots = _depfile_roots(build_dir, model)
    if not roots:
        return None
    matches = _grep_depfiles(sorted(headers), roots)
    if matches is None:
        return None
    out: dict[str, set[str]] = {}
    for line in matches:
        depfile = Path(line.strip())
        target = _target_from_object_path(str(depfile).replace("\\", "/"))
        if target is None or target not in model.targets:
            continue
        base_dir = str(build_dir / model.targets[target].build_dir)
        try:
            parse_make_depfile(depfile.read_text(encoding="utf-8", errors="replace"),
                               target, model.source_root, base_dir, headers, out)
        except OSError:
            continue
    if not matches and not any(next(r.rglob("*.d"), None) for r in roots):
        return None
    return out


# ── ctest inventory for projection ──


def projection_ctest_entries(tests: Iterable[dict[str, Any]],
                             graph: Any = None) -> list[CTestEntry]:
    """The authoritative corpus (same name/label exclusions as the exact-head
    plan) as lightweight entries; malformed registrations are skipped rather
    than refused, because a build command must not fail on inventory shape."""
    entries: list[CTestEntry] = []
    for test in tests:
        try:
            kept = authoritative_tests([test])
        except InventoryError:
            continue
        if not kept:
            continue
        props: dict[str, Any] = {}
        for prop in test.get("properties", []) or []:
            props[prop.get("name", "")] = prop.get("value")
        entries.append(CTestEntry(
            name=test.get("name", ""),
            command=[str(c) for c in (test.get("command") or [])],
            properties=props,
            registered_files=backtrace_files(graph, test.get("backtrace")),
        ))
    return entries


def load_projection_ctest_entries(build_dir: Path) -> list[CTestEntry] | None:
    try:
        tests = load_ctest_json(build_dir)
        return projection_ctest_entries(tests, getattr(tests, "backtrace_graph", None))
    except (OSError, subprocess.CalledProcessError, json.JSONDecodeError, InventoryError):
        return None


def _as_list(value: Any) -> list[str]:
    if value is None:
        return []
    if isinstance(value, list):
        return [str(v) for v in value]
    return [str(value)]


# ── selection rules ──


def file_stem(rel: str) -> str:
    return os.path.basename(rel).split(".", 1)[0]


def companion_test_targets(stem: str, model: CodeModel) -> set[str]:
    """Test programs whose test source is ``test_<stem>.cpp`` or ``test_<stem>_*.cpp``."""
    hits: set[str] = set()
    if not stem:
        return hits
    for target in model.targets.values():
        if not target.is_test:
            continue
        for source in target.sources:
            match = TEST_SOURCE_RE.match(file_stem(source))
            if not match:
                continue
            test_stem = match.group("stem")
            if test_stem == stem or test_stem.startswith(stem + "_"):
                hits.add(target.name)
                break
    return hits


def directory_owners(rel: str, model: CodeModel) -> set[str]:
    """Non-test targets defined in the longest source directory prefix of ``rel``."""
    best_len = -1
    owners: set[str] = set()
    directory = os.path.dirname(rel)
    for target in model.targets.values():
        if target.is_test:
            continue
        src = target.source_dir
        if src == ".":
            match, length = True, 0
        else:
            match = directory == src or directory.startswith(src + os.sep)
            length = len(src)
        if not match:
            continue
        if length > best_len:
            best_len = length
            owners = {target.name}
        elif length == best_len:
            owners.add(target.name)
    return owners


def reverse_dependencies(model: CodeModel) -> dict[str, set[str]]:
    rev: dict[str, set[str]] = {}
    for target in model.targets.values():
        for dep in target.dependencies:
            rev.setdefault(dep, set()).add(target.name)
    return rev


def test_dependents(name: str, rev: dict[str, set[str]], model: CodeModel) -> set[str]:
    """Test programs that depend on ``name`` directly (the ``add_dependencies``
    edge a shell-out test declares on the binary it runs)."""
    return {d for d in rev.get(name, set()) if d in model.targets and model.targets[d].is_test}


# ── test registration fragments ──
#
# A change to a CMake file is global by default: it may alter any target. The
# files under test/cmake/ are mostly test registrations, and CMake records, in
# both the file-API target records and the ctest listing, the call stack that
# defined each target and registered each test. A registration file that only
# defines its own targets and tests therefore selects exactly those, plus what
# depends on the targets it defines. Anything the call stacks cannot show stays
# global: a file that defines a function or macro another file calls, sets a
# variable another file reads, or touches directory, cache or global state.

REGISTRATION_FRAGMENT_RE = re.compile(r"^test/cmake/(?:fragments/)?[^/]+\.cmake$")
# The aggregator that include()s every fragment: it sits on every fragment's
# call stack, so a change to it reaches all of them.
SHARED_FRAGMENT_NAMES = frozenset({"fragments.cmake"})
_CMAKE_COMMAND_NAME = r"[^\s()#\"\[\]\\;]+"
_CMAKE_BRACKET_OPEN = re.compile(r"\[(=*)\[")
_CMAKE_UNQUOTED_END = frozenset(' \t\r\n\f\v()#"')

# Commands that only describe the registration owned by this file. Anything
# else is conservatively treated as shared: an unrecognized command may be a
# project macro that mutates a target, directory, or cache outside its stack,
# and list() or cmake_parse_arguments() can rewrite a variable a later fragment
# reads, while cmake_minimum_required() resets policy for the whole directory.
_CMAKE_LOCAL_COMMANDS = frozenset({
    "add_executable", "add_library", "add_test", "pulp_add_test_suite",
    "pulp_add_test_group", "pulp_dsp_series_catalog_suite", "pulp_dsp_series_signal_suite",
    "catch_discover_tests", "message", "set", "unset", "function", "macro",
    "endfunction", "endmacro",
    "if", "elseif", "else", "endif", "foreach", "endforeach", "while", "endwhile",
    "break", "continue", "return",
})
_CMAKE_GLOBAL_COMMANDS = frozenset({
    "include", "configure_file", "file", "execute_process", "add_subdirectory",
    "include_directories", "add_compile_options", "add_compile_definitions", "add_definitions",
    "link_libraries", "link_directories", "option", "set_property", "set_tests_properties",
    "set_target_properties", "set_source_files_properties", "add_custom_command",
    "add_custom_target",
})


class _CMakeSyntaxError(ValueError):
    """CMake text this lexer cannot read with certainty."""


def _cmake_skip_comment(text: str, index: int) -> int:
    """Index after the comment starting at text[index] == '#'."""
    bracket = _CMAKE_BRACKET_OPEN.match(text, index + 1)
    if bracket:
        close = "]" + bracket.group(1) + "]"
        end = text.find(close, bracket.end())
        if end < 0:
            raise _CMakeSyntaxError("unterminated bracket comment")
        return end + len(close)
    end = text.find("\n", index)
    return len(text) if end < 0 else end


@functools.lru_cache(maxsize=1024)
def _cmake_commands(text: str) -> list[tuple[str, list[str]]]:
    """Lex CMake text into (lower-case command, arguments) invocations.

    Comments are dropped only where CMake itself would drop them, quoted and
    bracket arguments are returned without their delimiters, and any text the
    lexer cannot read with certainty (an unterminated string, a stray token, an
    apostrophe in an unquoted argument) raises _CMakeSyntaxError so callers
    treat the file as shared."""
    commands: list[tuple[str, list[str]]] = []
    length = len(text)
    index = 0
    while index < length:
        char = text[index]
        if char.isspace():
            index += 1
            continue
        if char == "#":
            index = _cmake_skip_comment(text, index)
            continue
        name = re.compile(_CMAKE_COMMAND_NAME).match(text, index)
        if not name:
            raise _CMakeSyntaxError(f"unexpected {char!r}")
        index = name.end()
        while index < length and text[index].isspace():
            index += 1
        if index >= length or text[index] != "(":
            raise _CMakeSyntaxError(f"{name.group(0)} is not followed by arguments")
        index += 1
        depth = 1
        args: list[str] = []
        while depth:
            if index >= length:
                raise _CMakeSyntaxError("unterminated command")
            char = text[index]
            if char.isspace() or char == ";":
                index += 1
            elif char == "#":
                index = _cmake_skip_comment(text, index)
            elif char == "(":
                depth += 1
                args.append("(")
                index += 1
            elif char == ")":
                depth -= 1
                args.append(")")
                index += 1
            elif char == '"':
                index += 1
                start = index
                while index < length and text[index] != '"':
                    index += 2 if text[index] == "\\" else 1
                if index >= length:
                    raise _CMakeSyntaxError("unterminated quoted argument")
                args.append(text[start:index])
                index += 1
            elif char == "[" and _CMAKE_BRACKET_OPEN.match(text, index):
                bracket = _CMAKE_BRACKET_OPEN.match(text, index)
                close = "]" + bracket.group(1) + "]"
                end = text.find(close, bracket.end())
                if end < 0:
                    raise _CMakeSyntaxError("unterminated bracket argument")
                args.append(text[bracket.end():end])
                index = end + len(close)
            else:
                start = index
                while index < length and text[index] not in _CMAKE_UNQUOTED_END:
                    if text[index] == "'":
                        raise _CMakeSyntaxError("apostrophe in unquoted argument")
                    index += 2 if text[index] == "\\" else 1
                if index == start:
                    raise _CMakeSyntaxError("unreadable argument")
                args.append(text[start:index])
        args.pop()  # the closing parenthesis
        commands.append((name.group(0).lower(), args))
    return commands


def _cmake_scoped_set(args: list[str]) -> bool:
    """Whether set/unset changes state outside this registration fragment."""
    if not args:
        return False
    first = args[0]
    upper = first.upper()
    options = {arg.upper() for arg in args[1:]}
    return (
        upper.startswith("CMAKE_")
        or upper.startswith("ENV{")
        or "${" in first
        or "@" in first
        or "CACHE" in options
        or "PARENT_SCOPE" in options
    )


def _test_cmake_texts(source_root: str, ref: str | None = None) -> dict[str, str]:
    """Text of every CMake file under test/, by relative path."""
    root = Path(source_root) / "test"
    if ref is not None:
        try:
            listed = subprocess.run(
                ["git", "ls-tree", "-r", "--name-only", ref, "--", "test/CMakeLists.txt", "test/cmake"],
                cwd=source_root, capture_output=True, text=True, encoding="utf-8", check=True,
            ).stdout.splitlines()
        except (OSError, subprocess.CalledProcessError):
            return {}
        paths = [Path(source_root) / rel for rel in listed if rel.endswith(".cmake") or rel.endswith("CMakeLists.txt")]
    else:
        paths = [root / "CMakeLists.txt", *sorted(root.glob("cmake/*.cmake")),
                 *sorted(root.glob("cmake/fragments/*.cmake"))]
    texts: dict[str, str] = {}
    for path in paths:
        try:
            rel = path.relative_to(source_root).as_posix()
            if ref is None:
                text = path.read_text(encoding="utf-8", errors="replace")
            else:
                text = subprocess.run(
                    ["git", "show", f"{ref}:{rel}"], cwd=source_root,
                    capture_output=True, text=True, encoding="utf-8", check=True,
                ).stdout
            texts[rel] = text
        except OSError:
            continue
        except subprocess.CalledProcessError:
            continue
    return texts


def _shared_fragment_reason_current(rel: str, texts: dict[str, str]) -> str | None:
    """Why a registration file cannot be treated as owning only its own tests,
    or None when nothing outside its own call stacks can depend on it."""
    text = texts.get(rel)
    if text is None:
        return "unreadable"
    if os.path.basename(rel) in SHARED_FRAGMENT_NAMES:
        return "aggregates every fragment"
    try:
        invocations = _cmake_commands(text)
        others = {name: _cmake_commands(body) for name, body in texts.items() if name != rel}
    except _CMakeSyntaxError as error:
        return f"CMake syntax not readable with certainty ({error})"
    commands = {name for name, _args in invocations}
    if any(name in _CMAKE_GLOBAL_COMMANDS or name.startswith("target_")
           for name in commands):
        return "touches directory, cache or global state"
    if any(name in {"set", "unset"} and _cmake_scoped_set(args)
           for name, args in invocations):
        return "touches directory, cache or global state"
    unknown = sorted(commands - _CMAKE_LOCAL_COMMANDS)
    if unknown:
        return f"uses unrecognized CMake command {unknown[0]}"
    defined = {
        args[0].lower()
        for name, args in invocations
        if name in {"function", "macro"} and args
    }
    for name in defined:
        if any(name in {command for command, _args in other} for other in others.values()):
            return f"defines {name}(), called from another file"
    # set() and unset() both change a variable the other fragments share the
    # directory scope with, so either one is checked against every other file.
    written_variables = {
        args[0]
        for name, args in invocations
        if name in {"set", "unset"} and args
    }
    other_text = [
        "\n".join(arg for _name, args in other for arg in args)
        for other in others.values()
    ]
    for name in written_variables:
        word = re.compile(r"\b" + re.escape(name) + r"\b")
        if any(word.search(body) for body in other_text):
            return f"sets {name}, read by another file"
    return None


def shared_fragment_reason(rel: str, texts: dict[str, str],
                           base_texts: dict[str, str] | None = None) -> str | None:
    """Return a shared-state reason for either side of the changed file.

    A removal can be unsafe even when the current file looks local, so inspect
    the merge-base text as well whenever it is available.
    """
    reason = _shared_fragment_reason_current(rel, texts)
    if reason is not None:
        return reason
    if base_texts is not None and rel in base_texts:
        reason = _shared_fragment_reason_current(rel, base_texts)
        if reason is not None:
            return f"base version {reason}"
    return None


def fragment_projection(files: Iterable[str], model: "CodeModel",
                        inventory: list["CTestEntry"],
                        index: dict[str, set[str]],
                        base_texts: dict[str, str] | None = None
                        ) -> dict[str, tuple[set[str], set[str]]]:
    """Changed CMake files whose reach is fully known: path -> (tests, targets).

    A file absent from the result is global."""
    texts: dict[str, str] | None = None
    rev: dict[str, set[str]] | None = None
    result: dict[str, tuple[set[str], set[str]]] = {}
    for rel in files:
        if not REGISTRATION_FRAGMENT_RE.match(rel):
            continue
        absolute = os.path.normpath(os.path.join(model.source_root, rel))
        registered = [e for e in inventory if absolute in e.registered_files]
        defined = {t.name for t in model.targets.values() if absolute in t.definition_files}
        scripted = {e.name for e in inventory if absolute in command_path_refs(e.command)}
        if texts is None:
            texts = _test_cmake_texts(model.source_root)
        if shared_fragment_reason(rel, texts, base_texts) is not None:
            continue
        if not registered and not defined:
            if scripted:  # a `cmake -P` script: only the tests that run it read it
                result[rel] = (scripted, set())
            continue
        if rev is None:
            rev = reverse_dependencies(model)
        targets = set(defined)
        pending = list(defined)
        while pending:  # whatever links a target this file defines is rebuilt with it
            for dependent in rev.get(pending.pop(), ()):
                if dependent not in targets:
                    targets.add(dependent)
                    pending.append(dependent)
        for entry in registered:
            targets |= command_owners(entry.command, index) | declared_targets(entry)
        result[rel] = ({e.name for e in registered} | scripted, targets)
    return result


def family_projection(families: Iterable[dict[str, Any]], changed: Iterable[str]
                      ) -> tuple[set[str], set[str]]:
    """Tests and build targets the Shipyard policy families declare for the
    changed paths. Families are the hand-reviewed mapping the exact-head plan
    uses; the projection honours them on top of what it derives."""
    import fnmatch

    tests: set[str] = set()
    targets: set[str] = set()
    changed = list(changed)
    for family in families:
        patterns = family.get("paths", [])
        if not any(fnmatch.fnmatch(rel, pattern) for rel in changed for pattern in patterns):
            continue
        tests.update(family.get("tests", []))
        tests.update(family.get("extended_tests", []))
        targets.update(family.get("build_targets", []))
    return tests, targets


def follow_fixtures(tests: set[str], inventory: list[CTestEntry],
                    index: dict[str, set[str]]) -> tuple[set[str], set[str]]:
    """Close ``tests`` over FIXTURES_REQUIRED; return (tests, targets building fixtures)."""
    by_name: dict[str, list[CTestEntry]] = {}
    setups: dict[str, list[CTestEntry]] = {}
    for entry in inventory:
        by_name.setdefault(entry.name, []).append(entry)
        for fixture in _as_list(entry.properties.get("FIXTURES_SETUP")):
            setups.setdefault(fixture, []).append(entry)
    result = set(tests)
    fixture_targets: set[str] = set()
    pending = list(tests)
    while pending:
        name = pending.pop()
        for entry in by_name.get(name, []):
            for fixture in _as_list(entry.properties.get("FIXTURES_REQUIRED")):
                for setup in setups.get(fixture, []):
                    fixture_targets |= command_owners(setup.command, index)
                    if setup.name not in result:
                        result.add(setup.name)
                        pending.append(setup.name)
    return result, fixture_targets


def project_affected(model: CodeModel, changed: list[str], deleted: list[str],
                     deps_db: dict[str, set[str]] | None, inventory: list[CTestEntry] | None,
                     threshold: float = DEFAULT_PROJECTION_THRESHOLD,
                     stale_build_system: list[str] | None = None,
                     families: Iterable[dict[str, Any]] = (),
                     base_texts: dict[str, str] | None = None) -> Selection:
    """Map the diff to a Selection.

    ``deps_db`` is the header -> targets map from :func:`header_owners` (None
    when no dependency database exists). ``stale_build_system`` names the
    build-system files that postdate the codemodel reply; None treats every
    changed build-system file as stale. ``families`` are Shipyard policy
    families whose declared tests and build targets are honoured as well.
    """
    total_tests = len(inventory) if inventory is not None else 0
    total = model.total
    if not changed and not deleted:
        return all_selection("no changes relative to the base", [], total, total_tests, threshold)

    if stale_build_system is None:
        build_system = [f for f in changed + deleted if is_build_system_file(f)]
    else:
        build_system = list(stale_build_system)
    # A registration fragment that provably reaches only its own tests and
    # targets is not a build-system change. Without an inventory nothing can
    # prove that, so the caller's staleness verdict stands.
    fragments: dict[str, tuple[set[str], set[str]]] = {}
    if inventory is not None:
        fragments = fragment_projection(
            [f for f in changed if is_build_system_file(f)], model, inventory,
            artifact_index(model), base_texts)
    build_system = [f for f in build_system if f not in fragments]
    if build_system:
        return all_selection(f"build system changed: {build_system[0]}", changed, total,
                             total_tests, threshold)
    deleted_sources = [f for f in deleted
                       if os.path.splitext(f)[1] in SOURCE_EXTENSIONS | HEADER_EXTENSIONS]
    if deleted_sources:
        return all_selection(f"deleted source: {deleted_sources[0]}", changed, total,
                             total_tests, threshold)

    source_owners: dict[str, set[str]] = {}
    for target in model.targets.values():
        for source in target.sources:
            source_owners.setdefault(source, set()).add(target.name)
    rev = reverse_dependencies(model)

    selected: set[str] = set()
    unmapped: list[str] = []
    resolved = 0
    file_selected_tests: set[str] = set()
    fragment_selected_tests: set[str] = set()
    for rel in changed:
        if rel in fragments:
            resolved += 1
            fragment_selected_tests |= fragments[rel][0]
            selected |= fragments[rel][1]
            continue
        ext = os.path.splitext(rel)[1]
        owners = set(source_owners.get(rel, ()))
        if not owners and deps_db is not None:
            owners = set(deps_db.get(rel, ()))
        if owners:
            resolved += 1
            selected |= owners
            for owner in owners:
                if model.targets[owner].type == "OBJECT_LIBRARY":
                    # Its objects are linked into every dependent, so each
                    # one must relink (a static library's consumers pull a
                    # rebuilt archive only when they themselves are built).
                    selected |= rev.get(owner, set())
                elif not model.targets[owner].is_library:
                    selected |= test_dependents(owner, rev, model)
            selected |= companion_test_targets(file_stem(rel), model)
            continue
        if ext in HEADER_EXTENSIONS:
            if deps_db is not None:
                # A header no compiled object includes is inert; still relink
                # its companion tests so a stale include list is noticed.
                resolved += 1
                selected |= companion_test_targets(file_stem(rel), model)
                continue
            owners = directory_owners(rel, model)
            if not owners:
                return all_selection(f"header without an owning target: {rel}", changed, total,
                                     total_tests, threshold)
            resolved += 1
            selected |= owners
            for owner in owners:
                selected |= rev.get(owner, set())
            selected |= companion_test_targets(file_stem(rel), model)
            continue
        if ext in SOURCE_EXTENSIONS:
            return all_selection(f"source not owned by any configured target: {rel}", changed,
                                 total, total_tests, threshold)
        unmapped.append(rel)

    family_tests, family_targets = family_projection(families, changed)
    selected |= {t for t in family_targets if t in model.targets}
    selected = {t for t in selected if t in model.targets}
    if inventory is not None:
        index = artifact_index(model)
        changed_abs = {os.path.normpath(os.path.join(model.source_root, f)) for f in changed}
        by_test_target: dict[str, set[str]] = {}
        known_names = {entry.name for entry in inventory}
        for entry in inventory:
            refs = command_path_refs(entry.command)
            for ref in refs:
                for owner in index.get(ref, ()):
                    by_test_target.setdefault(owner, set()).add(entry.name)
            for owner in declared_targets(entry):
                by_test_target.setdefault(owner, set()).add(entry.name)
            if any(ref in changed_abs for ref in refs):
                file_selected_tests.add(entry.name)
        tests: set[str] = (set(file_selected_tests) | (family_tests & known_names)
                           | (fragment_selected_tests & known_names))
        for name in selected:
            tests |= by_test_target.get(name, set())
        tests, fixture_targets = follow_fixtures(tests, inventory, index)
        selected |= {t for t in fixture_targets if t in model.targets}
    else:
        tests = set()

    if not selected and not tests and (unmapped or resolved != len(changed)):
        return all_selection("no build targets or tests map to the diff", changed, total,
                             total_tests, threshold, unmapped)
    if total and len(selected) > threshold * total:
        return all_selection(
            f"{len(selected)} affected targets exceed {int(threshold * 100)}% of {total}",
            changed, total, total_tests, threshold, unmapped)
    return Selection(
        mode="focused",
        reason="targets owning the diff plus their tests",
        changed_files=sorted(changed),
        targets=sorted(selected),
        tests=sorted(tests),
        total_targets=total,
        total_tests=total_tests,
        unmapped=sorted(unmapped),
        threshold=threshold,
    )


def project_working_diff(build_dir: Path, source_root: Path | None, base: str,
                         threshold: float = DEFAULT_PROJECTION_THRESHOLD,
                         files: list[str] | None = None, with_tests: bool = True,
                         families: Iterable[dict[str, Any]] = ()) -> Selection:
    """Project the working diff of a configured build directory end to end."""
    ensure_codemodel_query(build_dir)
    if not codemodel_reply_available(build_dir):
        return all_selection(
            "codemodel reply missing; the query is written and the next configure produces it",
            [], 0, 0, threshold)
    try:
        model = load_codemodel_targets(build_dir)
    except InventoryError as error:
        return all_selection(f"codemodel unreadable: {error}", [], 0, 0, threshold)
    root = source_root or Path(model.source_root)
    if files is None:
        changed, deleted, _warning = changed_files(root, base)
    else:
        changed = [os.path.normpath(f) for f in files if (root / f).is_file()]
        deleted = [os.path.normpath(f) for f in files if not (root / f).exists()]
    if not changed and not deleted:
        return all_selection("no changes relative to the base", [], model.total, 0, threshold)
    stale = stale_build_system_files(root, changed, deleted, codemodel_reply_mtime(build_dir))
    if stale:
        return project_affected(model, changed, deleted, None, None, threshold, stale, families)
    headers = {f for f in changed if os.path.splitext(f)[1] in HEADER_EXTENSIONS}
    deps_db = header_owners(build_dir, model, headers)
    inventory = load_projection_ctest_entries(build_dir) if with_tests else None
    base_texts = _test_cmake_texts(str(root), base)
    return project_affected(model, changed, deleted, deps_db, inventory, threshold, stale,
                            families, base_texts)


# ── Configuration-neutral registration projection ──
#
# The same tree configured for the required gate (Release, examples OFF) and
# for the Shipyard lane (Debug, examples ON) registers the same tests, but
# their ctest listings differ in build paths, the build type, the Python
# interpreter and the examples' own registrations. This projection removes
# exactly those differences, so a base inventory recorded by one job can be
# compared with another's. Discovered Catch2 cases are one row per
# executable: the case list is a function of the executable's bytes, which a
# binary identity key covers, not of the registration.

REGISTRATION_PROJECTION_SCHEMA = "pulp.changed-surface-registration-projection/v1"
BUILD_TYPE_TOKEN = "${CMAKE_BUILD_TYPE}"
SOURCE_DIR_TOKEN = "${CMAKE_SOURCE_DIR}"
PYTHON_TOKEN = "${PYTHON}"
PYTHON_PATH = re.compile(r"(?:/[^\s=:;,'\"]*)?/python3(?:\.\d+)?(?![\w.])")
PYTHON_NAME = re.compile(r"^python3(?:\.\d+)?$")
BUILD_TYPES = ("Debug", "Release", "RelWithDebInfo", "MinSizeRel")
IGNORED_PROJECTION_PROPERTIES = frozenset({"WORKING_DIRECTORY", "_BACKTRACE_TRIPLES"})
# Build-tree directories holding example executables and plugin bundles, which
# exist only when examples are configured.
EXAMPLE_PRODUCT_DIRS = ("examples", "AU", "AUv3", "CLAP", "VST3", "LV2")
# Registrations whose arguments depend on PULP_CHANGED_SURFACE_INVENTORY_TARGET,
# which only the Shipyard lane enables: the selector's own inventory check.
INVENTORY_TARGET_GATED = frozenset({"changed-surface-policy-inventory"})


def load_ctest_payload(build_dir: Path) -> dict[str, Any]:
    """The whole ``--show-only=json-v1`` document, backtrace graph included."""
    result = subprocess.run(
        ["ctest", "--test-dir", str(build_dir), "--show-only=json-v1"],
        check=True,
        capture_output=True,
        text=True,
    )
    payload = json.loads(result.stdout)
    if not isinstance(payload.get("tests"), list):
        raise InventoryError("CTest JSON has no tests array")
    return payload


def _cache_value(build_dir: Path, key: str) -> str:
    cache = build_dir / "CMakeCache.txt"
    if not cache.is_file():
        return ""
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith(f"{key}:") and "=" in line:
            return line.split("=", 1)[1]
    return ""


def _neutral(value: Any, roots: list[tuple[str, str]], build_type: str) -> Any:
    if isinstance(value, list):
        return [_neutral(item, roots, build_type) for item in value]
    if not isinstance(value, str):
        return value
    for root, token in roots:
        value = value.replace(root, token)
    value = PYTHON_PATH.sub(PYTHON_TOKEN, value)
    if build_type:
        # Only a whole value or an `=`-assigned value names the build type;
        # a test called "Release notes" keeps its name.
        if value == build_type:
            value = BUILD_TYPE_TOKEN
        value = re.sub(rf"(?<==){re.escape(build_type)}(?![\w-])", BUILD_TYPE_TOKEN, value)
    return value


def _build_relative(test: dict[str, Any], command: list[str], build: str) -> str:
    """Where in the build tree a registration lives: its working directory,
    else its program's directory; "" when outside the build tree."""
    candidates = [
        prop.get("value")
        for prop in test.get("properties") or []
        if prop.get("name") == "WORKING_DIRECTORY"
    ]
    if command:
        candidates.append(os.path.dirname(command[0]))
    for candidate in candidates:
        if isinstance(candidate, str) and candidate:
            path = os.path.realpath(candidate)
            if path.startswith(build + os.sep):
                return os.path.relpath(path, build)
    return ""


def project_registrations(
    payload: dict[str, Any], source_root: Path, build_dir: Path, shape: str = "listing"
) -> dict[str, Any]:
    """The configuration-neutral multiset of a ctest listing.

    ``incomplete`` lists registrations whose command is missing: an
    unbuilt target, or an unbuilt Catch2 executable's placeholder. A
    projection with any is not ``recordable`` as a base inventory, so a
    half-built tree can never pin one.

    ``shape="configure"`` folds a built tree to what a configure alone
    lists, so trees in different build states compare: a discovered case,
    which ctest lists without a backtrace from the executable's generated
    include file, collapses to its executable's row like a placeholder, and
    a registration whose program is in the build tree lists no command, as
    an unbuilt target does."""
    if shape not in ("listing", "configure"):
        raise ValueError(f"unknown projection shape {shape!r}")
    import script_test_inputs as sti  # tools/scripts: the gate-profile filter

    source = os.path.realpath(source_root)
    build = os.path.realpath(build_dir)
    # Both spellings of each root: ctest records the configured path, which
    # may run through a symlink (/var -> /private/var on macOS).
    roots = sorted(
        {
            (path, token)
            for root, token in ((build_dir, sti.BINARY_DIR_TOKEN), (source_root, SOURCE_DIR_TOKEN))
            for path in (os.path.abspath(root), os.path.realpath(root))
        },
        key=lambda item: len(item[0]),
        reverse=True,
    )
    build_type = _cache_value(build_dir, "CMAKE_BUILD_TYPE")
    outside = sti.outside_gate_profile(payload, Path(source))
    rows: Counter[str] = Counter()
    incomplete: list[str] = []
    config_gated: list[str] = []
    discovered: dict[str, dict[str, Any]] = {}
    for test in payload.get("tests", []):
        name = test.get("name", "")
        if name in outside:
            continue
        command = [str(part) for part in test.get("command") or []]
        registered = sti.registered_from(test, payload) or ""
        placeholder = NOT_BUILT_PLACEHOLDER.match(name)
        generated = registered and os.path.realpath(registered).startswith(build + os.sep)
        location = _build_relative(test, command, build)
        in_build = bool(command) and os.path.realpath(command[0]).startswith(build + os.sep)
        # ctest gives every add_test a backtrace; a case it read from a
        # discovery include file at test time has none.
        if shape == "configure" and "backtrace" not in test and in_build:
            generated = True
        if location.split("/", 1)[0] == "examples":
            continue  # an example's own suite: registered only with examples ON
        if placeholder or generated:
            # A discovered case, or the placeholder that stands for an unbuilt
            # executable's cases: one row per executable.
            executable = (
                placeholder.group("target") if placeholder else os.path.basename(command[0] if command else "")
            )
            if placeholder or not command:
                incomplete.append(name)
            discovered.setdefault(executable, {"kind": "discovered", "executable": executable})
            continue
        if not command or not command[0]:
            incomplete.append(name)
        if shape == "configure" and in_build:
            command = [""]
        program = _neutral(command[0], roots, build_type) if command else ""
        if not program.startswith((sti.BINARY_DIR_TOKEN + "/", SOURCE_DIR_TOKEN + "/")):
            program = os.path.basename(program)
        if PYTHON_NAME.match(program):
            program = PYTHON_TOKEN
        properties = {
            prop.get("name"): _neutral(prop.get("value"), roots, build_type)
            for prop in test.get("properties") or []
            if prop.get("name") not in IGNORED_PROJECTION_PROPERTIES
        }
        row = {
            "kind": "registration",
            "name": name,
            "command": [program] + [_neutral(part, roots, build_type) for part in command[1:]],
            "properties": dict(sorted(properties.items())),
        }
        encoded = json.dumps(row, sort_keys=True, separators=(",", ":"))
        example_product = any(
            f"{sti.BINARY_DIR_TOKEN}/{directory}/" in encoded for directory in EXAMPLE_PRODUCT_DIRS
        )
        if example_product or name in INVENTORY_TARGET_GATED:
            # Recorded, never compared: its shape follows a configure switch.
            config_gated.append(name)
            continue
        rows[encoded] += 1
    for row in discovered.values():
        rows[json.dumps(row, sort_keys=True, separators=(",", ":"))] += 1
    ordered = sorted(rows.items())
    return {
        "schema": REGISTRATION_PROJECTION_SCHEMA,
        "shape": shape,
        "build_type": build_type,
        "rows": [{"row": json.loads(key), "count": count} for key, count in ordered],
        "row_count": sum(count for _, count in ordered),
        "incomplete": sorted(incomplete),
        "config_gated": sorted(config_gated),
        "recordable": not incomplete,
        "digest": hashlib.sha256(
            "\n".join(f"{count}\t{key}" for key, count in ordered).encode()
        ).hexdigest(),
    }


BUILD_PRODUCT_TOKEN = "${BUILD_PRODUCT}"


def comparable_rows(projection: dict[str, Any]) -> Counter[str]:
    """The projection's multiset with every build-tree program folded to one
    token. A configure-only tree lists an unbuilt target's program as an empty
    command, and a built one as its path; both name the same registration, and
    the program's bytes are a binary identity concern, not the inventory's."""
    import script_test_inputs as sti

    rows: Counter[str] = Counter()
    for entry in projection.get("rows", []):
        row = dict(entry["row"])
        if row.get("kind") == "registration":
            command = list(row.get("command") or [""])
            if command[0] == "" or command[0].startswith(sti.BINARY_DIR_TOKEN + "/"):
                command[0] = BUILD_PRODUCT_TOKEN
            row["command"] = command
        rows[json.dumps(row, sort_keys=True, separators=(",", ":"))] += int(entry["count"])
    return rows


def projection_differences(base: dict[str, Any], live: dict[str, Any], limit: int = 12) -> list[str]:
    """Rows one projection has and the other lacks, base first; [] when equal."""
    if base.get("schema") != live.get("schema"):
        return [f"projection schema {base.get('schema')!r} != {live.get('schema')!r}"]
    a, b = comparable_rows(base), comparable_rows(live)
    lines = [f"only in base: {key}" for key in sorted((a - b).elements())]
    lines += [f"only in this tree: {key}" for key in sorted((b - a).elements())]
    return lines[:limit] + ([f"... {len(lines) - limit} more"] if len(lines) > limit else [])


def name_only_rows(projection: dict[str, Any]) -> int:
    """Registrations a projection lists without a program: compared on name,
    arguments and properties alone."""
    return sum(
        int(entry["count"])
        for entry in projection.get("rows", [])
        if entry["row"].get("kind") == "registration" and not (entry["row"].get("command") or [""])[0]
    )

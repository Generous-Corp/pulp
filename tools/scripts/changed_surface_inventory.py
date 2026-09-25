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
EXCLUDED_LABEL_REGEX = "validation|slow|performance|bench|quality-lab"
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
    file in that working directory.  Every other missing or malformed command
    remains an inventory error.
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


def authoritative_tests(tests: Iterable[dict[str, Any]]) -> list[dict[str, Any]]:
    excluded_names = re.compile(EXCLUDED_NAME_REGEX)
    excluded_labels = re.compile(EXCLUDED_LABEL_REGEX)
    kept: list[dict[str, Any]] = []
    for test in tests:
        name = test.get("name")
        if not isinstance(name, str) or not name:
            raise InventoryError("CTest registration name must be a nonempty string")
        if "\n" in name or "\r" in name:
            raise InventoryError("CTest registration names cannot contain newlines")
        properties = _property_records(test)
        labels: list[str] = []
        for prop in properties:
            if prop["name"] != "LABELS":
                continue
            value = prop["value"]
            values = value if isinstance(value, list) else [value]
            if not all(isinstance(label, str) for label in values):
                raise InventoryError("CTest LABELS must contain strings")
            labels.extend(values)
        if excluded_names.search(name):
            continue
        if any(excluded_labels.search(label) for label in labels):
            continue
        kept.append(test)
    return kept


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


def validate_manifest(manifest: dict[str, Any], contract: dict[str, Any]) -> None:
    if manifest["duplicate_composite_group_count"]:
        raise InventoryError("duplicate composite registration identity is ambiguous")
    fields = (
        "registration_count",
        "unique_name_count",
        "unique_composite_count",
        "duplicate_name_group_count",
        "duplicate_name_excess_count",
        "duplicate_composite_group_count",
        "target_contract_digest",
        "authoritative_filter_digest",
        "inventory_digest",
    )
    mismatches = [
        f"{field}: expected {contract.get(field)!r}, observed {manifest.get(field)!r}"
        for field in fields
        if contract.get(field) != manifest.get(field)
    ]
    if mismatches:
        raise InventoryError("inventory contract drift; require full suite: " + "; ".join(mismatches))


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
    return tests


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


def projection_ctest_entries(tests: Iterable[dict[str, Any]]) -> list[CTestEntry]:
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
        ))
    return entries


def load_projection_ctest_entries(build_dir: Path) -> list[CTestEntry] | None:
    try:
        return projection_ctest_entries(load_ctest_json(build_dir))
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
                    artifact_owner: dict[str, str]) -> tuple[set[str], set[str]]:
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
                    for path in setup.command:
                        owner = artifact_owner.get(os.path.normpath(path))
                        if owner is not None:
                            fixture_targets.add(owner)
                    if setup.name not in result:
                        result.add(setup.name)
                        pending.append(setup.name)
    return result, fixture_targets


def project_affected(model: CodeModel, changed: list[str], deleted: list[str],
                     deps_db: dict[str, set[str]] | None, inventory: list[CTestEntry] | None,
                     threshold: float = DEFAULT_PROJECTION_THRESHOLD,
                     stale_build_system: list[str] | None = None,
                     families: Iterable[dict[str, Any]] = ()) -> Selection:
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
    for rel in changed:
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
        artifact_owner: dict[str, str] = {}
        for target in model.targets.values():
            for artifact in target.artifacts:
                artifact_owner[artifact] = target.name
        changed_abs = {os.path.normpath(os.path.join(model.source_root, f)) for f in changed}
        by_test_target: dict[str, set[str]] = {}
        known_names = {entry.name for entry in inventory}
        for entry in inventory:
            cmd_paths = [os.path.normpath(c) for c in entry.command]
            for path in cmd_paths:
                owner = artifact_owner.get(path)
                if owner is not None:
                    by_test_target.setdefault(owner, set()).add(entry.name)
            if any(p in changed_abs for p in cmd_paths):
                file_selected_tests.add(entry.name)
        tests: set[str] = set(file_selected_tests) | (family_tests & known_names)
        for name in selected:
            tests |= by_test_target.get(name, set())
        tests, fixture_targets = follow_fixtures(tests, inventory, artifact_owner)
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
    return project_affected(model, changed, deleted, deps_db, inventory, threshold, stale,
                            families)

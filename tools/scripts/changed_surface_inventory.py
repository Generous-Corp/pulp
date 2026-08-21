#!/usr/bin/env python3
"""Canonical CTest inventory for changed-surface planning.

CTest permits duplicate test names.  A name set therefore cannot describe the
suite that the authoritative command will execute.  This module retains every
filtered registration as a canonical composite, groups byte-identical
composites as a multiset, and makes ambiguity a fail-closed result.
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
from decimal import Decimal
from pathlib import Path
from typing import Any, Iterable


INVENTORY_SCHEMA = "pulp.changed-surface-ctest-inventory/v1"
REGISTRATION_DOMAIN = b"pulp-ctest-registration-v1\0"
INVENTORY_DOMAIN = b"pulp-ctest-inventory-v1\0"
DIGEST_DOMAIN = b"pulp-changed-surface-contract-v1\0"
EXCLUDED_NAME_REGEX = "AudioWorkgroup"
EXCLUDED_LABEL_REGEX = "validation|slow|performance|bench|quality-lab"


class InventoryError(ValueError):
    """The inventory cannot safely authorize bounded selection."""


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


def _toolchain_contract(build_dir: Path, groups: list[dict[str, Any]]) -> dict[str, Any]:
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
    groups = inventory_groups(tests, source_root, build_dir)
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
    toolchain = _toolchain_contract(build_dir, groups)
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

#!/usr/bin/env python3
"""Validate the COFF members of a Windows ARM64EC GPU provider archive.

A provider manifest can claim ``arm64ec`` while a stale archive still contains
only ARM64 objects.  The linker then reports missing ARM64EC STL thunks much
later, in the plugin consumer.  This checker reads the archive itself so the
configure gate is bound to the bytes that will be linked.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import struct
import sys
from dataclasses import dataclass

ARCHIVE_MAGIC = b"!<arch>\n"
MACHINE_NAMES = {
    0x8664: "x64",
    0xA641: "arm64ec",
    0xA64E: "arm64x",
    0xAA64: "arm64",
}
EC_MACHINES = frozenset({0xA641, 0xA64E})
ARM64X_MACHINES = frozenset({0xA641, 0xA64E, 0xAA64})


@dataclass(frozen=True)
class ArchiveMember:
    name: str
    machine: int


def _version(value: object, *, label: str) -> tuple[int, int, int]:
    """Parse a dotted MSVC compiler version from provider metadata or CMake."""
    match = re.search(r"(?<!\d)(\d+)\.(\d+)\.(\d+)(?!\d)", str(value))
    if not match:
        raise ValueError(f"{label} does not contain a compiler version: {value!r}")
    return tuple(int(part) for part in match.groups())


def _provider_compiler_version(document: dict[str, object]) -> tuple[int, int, int]:
    """Return the compiler version used to produce the static provider.

    Skia and Dawn record their compiler independently.  If both records are
    present they must agree; silently choosing one would allow a mixed
    provider to pass the consumer ABI gate and fail later at link time.
    """
    producer = document.get("producer")
    if not isinstance(producer, dict):
        raise ValueError("provider metadata is missing producer compiler records")
    versions: list[tuple[int, int, int]] = []
    for key in ("skia_gn_compiler", "dawn_cmake_compiler"):
        record = producer.get(key)
        if record is None:
            continue
        if not isinstance(record, dict) or "version" not in record:
            raise ValueError(f"provider metadata has an invalid {key} record")
        versions.append(_version(record["version"], label=f"provider {key}"))
    if not versions:
        raise ValueError("provider metadata has no producer compiler version")
    if any(candidate != versions[0] for candidate in versions[1:]):
        rendered = ", ".join(".".join(str(part) for part in candidate) for candidate in versions)
        raise ValueError(f"provider compiler versions disagree: {rendered}")
    return versions[0]


def _member_name(raw: bytes, longnames: bytes) -> str | None:
    name = raw.decode("ascii", errors="replace").rstrip()
    if name.startswith("/") and not name[1:].isdigit():
        # Linker/EC symbol members (/, //, /SYM64/, /<ECSYMBOLS>, ...)
        # are archive metadata rather than COFF objects.
        return None
    if name.startswith("/") and name[1:].isdigit():
        offset = int(name[1:])
        if offset >= len(longnames):
            raise ValueError(f"long-name offset {offset} is outside archive table")
        end = longnames.find(b"/\n", offset)
        if end < 0:
            end = longnames.find(b"\n", offset)
        if end < 0:
            end = len(longnames)
        name = longnames[offset:end].decode("ascii", errors="replace")
    elif name.endswith("/"):
        name = name[:-1]
    return name or None


def _object_machine(payload: bytes) -> int:
    if len(payload) < 20:
        raise ValueError("COFF member is shorter than a file header")
    # BigObj replaces the normal machine field with a signature and stores the
    # machine at offset 6.  MSVC emits BigObj for some large C++ translation
    # units, so rejecting it as an unknown object would be a false failure.
    if payload[:2] == b"\x00\x00" and payload[2:4] == b"\xff\xff":
        if len(payload) < 8:
            raise ValueError("truncated BigObj header")
        return struct.unpack_from("<H", payload, 6)[0]
    return struct.unpack_from("<H", payload, 0)[0]


def read_coff_archive(path: pathlib.Path) -> list[ArchiveMember]:
    data = path.read_bytes()
    if not data.startswith(ARCHIVE_MAGIC):
        raise ValueError(f"{path} is not a COFF archive")
    offset = len(ARCHIVE_MAGIC)
    longnames = b""
    members: list[ArchiveMember] = []
    while offset < len(data):
        if offset + 60 > len(data):
            raise ValueError(f"{path} has a truncated archive member header")
        header = data[offset : offset + 60]
        offset += 60
        try:
            size = int(header[48:58].decode("ascii").strip() or "0")
        except ValueError as exc:
            raise ValueError(f"{path} has an invalid member size") from exc
        end = offset + size
        if end > len(data):
            raise ValueError(f"{path} has a member outside the archive")
        name = _member_name(header[:16], longnames)
        payload = data[offset:end]
        offset = end + (size & 1)
        if name is None:
            # The // member contains the long-name table.  It must be retained
            # before the following /NN object names are decoded.
            raw_name = header[:16].decode("ascii", errors="replace").rstrip()
            if raw_name == "//":
                longnames = payload
            continue
        members.append(ArchiveMember(name=name, machine=_object_machine(payload)))
    if not members:
        raise ValueError(f"{path} contains no COFF object members")
    return members


def validate_provider(
    archives: list[pathlib.Path],
    target: str,
    metadata: pathlib.Path | None = None,
    consumer_compiler_version: str | None = None,
) -> list[ArchiveMember]:
    target = target.lower().replace("-win", "")
    if target not in {"arm64ec", "arm64x"}:
        raise ValueError(f"unsupported provider target {target!r}")
    if metadata is not None:
        try:
            document = json.loads(metadata.read_text(encoding="utf-8"))
            declared = str(document["target"]["architecture"]).lower()
        except (OSError, KeyError, TypeError, ValueError) as exc:
            raise ValueError(f"invalid provider metadata {metadata}: {exc}") from exc
        if declared not in {"arm64ec", "arm64x"}:
            raise ValueError(
                f"provider metadata declares {declared!r}; expected arm64ec/arm64x"
            )
        if target == "arm64ec" and declared not in {"arm64ec", "arm64x"}:
            raise ValueError(f"provider metadata target {declared!r} is not EC-compatible")
        if consumer_compiler_version is not None:
            required = _provider_compiler_version(document)
            consumer = _version(consumer_compiler_version, label="consumer compiler")
            if consumer < required:
                required_text = ".".join(str(part) for part in required)
                consumer_text = ".".join(str(part) for part in consumer)
                raise ValueError(
                    "consumer MSVC compiler "
                    f"{consumer_text} is older than the provider compiler "
                    f"{required_text}; use a matching or newer MSVC STL/CRT "
                    "toolset instead of adding thunk shims"
                )
    elif consumer_compiler_version is not None:
        raise ValueError("consumer compiler version cannot be checked without provider metadata")
    allowed = EC_MACHINES if target == "arm64ec" else ARM64X_MACHINES
    all_members: list[ArchiveMember] = []
    for archive in archives:
        members = read_coff_archive(archive)
        bad = [m for m in members if m.machine not in allowed]
        if bad:
            details = ", ".join(
                f"{m.name}={MACHINE_NAMES.get(m.machine, hex(m.machine))}" for m in bad[:8]
            )
            raise ValueError(
                f"{archive} contains incompatible {target} provider members: {details}"
            )
        if not any(m.machine in EC_MACHINES for m in members):
            raise ValueError(f"{archive} contains no ARM64EC/ARM64X object members")
        all_members.extend(members)
    return all_members


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--target", required=True, choices=("arm64ec", "arm64x"))
    parser.add_argument("--metadata", type=pathlib.Path)
    parser.add_argument("--consumer-compiler-version")
    parser.add_argument("--archive", action="append", type=pathlib.Path, required=True)
    args = parser.parse_args(argv)
    try:
        members = validate_provider(
            args.archive,
            args.target,
            args.metadata,
            args.consumer_compiler_version,
        )
    except (OSError, ValueError) as exc:
        print(f"ARM64EC provider validation failed: {exc}", file=sys.stderr)
        return 2
    machines = sorted({MACHINE_NAMES.get(m.machine, hex(m.machine)) for m in members})
    print(f"ARM64EC provider validation passed: {len(members)} members ({', '.join(machines)})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

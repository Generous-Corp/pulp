#!/usr/bin/env python3
"""Trim the ICU common data that Skia's prebuilt libskunicode_icu.a embeds.

The skia-builder archive carries a full Chromium ICU data package (about
10.9 MB) as one object, ``libicu.icudtl_dat.o``, which defines the
``icudtNN_dat`` symbol. Every Pulp binary that shapes text links it, and it
is the single largest contributor to a plug-in bundle's size.

Pulp's text stack only reaches the ICU code for break iteration, bidi,
normalization, character properties and locale resolution. The collation,
time-zone, currency, display-name, unit, region, transliteration, number
spell-out, converter and spoof-check items are read only by ICU services that
are not linked into a Pulp binary, so they are removed here. Everything else,
including every break-iterator rule file and dictionary, is kept byte for
byte, and items whose category this policy does not name are kept, so a newer
ICU drop cannot lose data silently.

The script rewrites the data object inside a copy of the archive (per
architecture slice of a universal archive) and leaves the source archive
untouched. It needs ``lipo`` and ``ar`` (Xcode command line tools) and nothing
else; it never invokes a compiler, so the object keeps its original
LC_BUILD_VERSION.

Usage::

    icu_data_subset.py --archive libskunicode_icu.a --output out/libskunicode_icu.a \
        [--work-dir DIR] [--report report.json]

Exit status is 0 on success and non-zero on any structural surprise; callers
fall back to the untouched archive in that case.
"""

from __future__ import annotations

import argparse
import json
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

# Bump when the keep/drop policy or the rewrite changes, so cached outputs
# from an older policy are not reused.
POLICY_VERSION = 2

# Item path prefixes (after the "icudtNNl/" package name) that only ICU
# services absent from a Pulp binary read.
DROPPED_TREES = (
    "coll/",      # collation (ucol)
    "zone/",      # time-zone display names
    "curr/",      # currency display names
    "lang/",      # language display names (uloc_getDisplayLanguage)
    "region/",    # region display names
    "unit/",      # measure-unit formatting
    "translit/",  # transliterators
    "rbnf/",      # rule-based number spell-out
)
# Root-level items read only by time-zone services, and the IDNA (UTS #46)
# mapping, which only domain-name processing reads.
DROPPED_ROOT_ITEMS = frozenset({
    "zoneinfo64.res",
    "metaZones.res",
    "timezoneTypes.res",
    "windowsZones.res",
    "uts46.nrm",
})
# Root-level suffixes: codepage converter tables (the converter alias table,
# cnvalias.icu, is kept) and spoof-check confusables.
DROPPED_ROOT_SUFFIXES = (".cnv", ".cfu")

_CMND_MAGIC = (0xDA, 0x27)
_ITEM_ALIGN = 16


class SubsetError(RuntimeError):
    """The input does not have the structure this tool knows how to rewrite."""


def is_dropped(item_name: str) -> bool:
    """Policy for one TOC item name, e.g. ``icudt78l/coll/root.res``."""
    _, sep, rel = item_name.partition("/")
    if not sep:
        return False
    if rel.startswith(DROPPED_TREES):
        return True
    if "/" in rel:
        return False
    return rel in DROPPED_ROOT_ITEMS or rel.endswith(DROPPED_ROOT_SUFFIXES)


def read_common_data(data: bytes) -> tuple[int, list[tuple[str, bytes]]]:
    """Parse an ICU common-data (CmnD, offset-TOC) package.

    Returns the header size and the ordered (name, payload) items. A payload
    runs to the next item's offset, so it carries the original alignment
    padding.
    """
    if len(data) < 32:
        raise SubsetError("ICU data shorter than a header")
    header_size = struct.unpack_from("<H", data, 0)[0]
    if (data[2], data[3]) != _CMND_MAGIC:
        raise SubsetError("ICU data magic mismatch")
    # UDataInfo starts at byte 4: size, reservedWord, isBigEndian, ...
    if data[8] != 0:
        raise SubsetError("big-endian ICU data is not supported")
    if data[12:16] != b"CmnD":
        raise SubsetError("ICU data is not an offset-TOC common data package")
    if header_size >= len(data):
        raise SubsetError("ICU header size out of range")
    count = struct.unpack_from("<I", data, header_size)[0]
    toc_end = header_size + 4 + 8 * count
    if toc_end > len(data):
        raise SubsetError("ICU table of contents out of range")
    entries = [struct.unpack_from("<II", data, header_size + 4 + 8 * i) for i in range(count)]
    items: list[tuple[str, int]] = []
    for name_off, data_off in entries:
        start = header_size + name_off
        end = data.find(b"\0", start)
        if end < 0:
            raise SubsetError("unterminated ICU item name")
        items.append((data[start:end].decode("ascii"), header_size + data_off))
    out: list[tuple[str, bytes]] = []
    for i, (name, offset) in enumerate(items):
        end = items[i + 1][1] if i + 1 < len(items) else len(data)
        if not toc_end <= offset <= end <= len(data):
            raise SubsetError(f"ICU item {name} out of order or out of range")
        out.append((name, data[offset:end]))
    names = [n for n, _ in out]
    if names != sorted(names):
        raise SubsetError("ICU table of contents is not sorted")
    return header_size, out


def write_common_data(header: bytes, items: list[tuple[str, bytes]]) -> bytes:
    """Serialize a CmnD package: header, TOC, names, 16-byte-aligned items."""
    header_size = len(header)
    toc = bytearray(struct.pack("<I", len(items)))
    names = bytearray()
    name_base = 4 + 8 * len(items)
    name_offsets = []
    for name, _ in items:
        name_offsets.append(name_base + len(names))
        names += name.encode("ascii") + b"\0"
    data_start = header_size + name_base + len(names)
    data_start += (-data_start) % _ITEM_ALIGN
    body = bytearray()
    data_offsets = []
    for _, payload in items:
        data_offsets.append(data_start + len(body) - header_size)
        body += payload
        body += b"\0" * ((-len(body)) % _ITEM_ALIGN)
    for name_off, data_off in zip(name_offsets, data_offsets):
        toc += struct.pack("<II", name_off, data_off)
    out = bytearray(header) + toc + names
    out += b"\0" * (data_start - len(out))
    out += body
    return bytes(out)


def subset_common_data(data: bytes, dropped=is_dropped) -> tuple[bytes, dict]:
    header_size, items = read_common_data(data)
    kept = [(n, p) for n, p in items if not dropped(n)]
    removed = [(n, p) for n, p in items if dropped(n)]
    out = write_common_data(data[:header_size], kept)
    stats = {
        "items_in": len(items),
        "items_kept": len(kept),
        "bytes_in": len(data),
        "bytes_out": len(out),
        "dropped_bytes_by_group": {},
    }
    for name, payload in removed:
        rel = name.partition("/")[2]
        group = rel.split("/")[0] + "/" if "/" in rel else rel.rsplit(".", 1)[-1]
        stats["dropped_bytes_by_group"][group] = (
            stats["dropped_bytes_by_group"].get(group, 0) + len(payload))
    return out, stats


# ── Mach-O relocatable object rewrite ──────────────────────────────────────

_MH_MAGIC_64 = 0xFEEDFACF
_MH_OBJECT = 0x1
_LC_SEGMENT_64 = 0x19
_LC_SYMTAB = 0x2
_LC_DYSYMTAB = 0xB
_LC_BUILD_VERSION = 0x32
_LC_VERSION_MIN = {0x24, 0x25, 0x2F, 0x30}


def _cstr16(raw: bytes) -> str:
    return raw.split(b"\0", 1)[0].decode("ascii")


def extract_macho_payload(obj: bytes) -> bytes:
    """Return the bytes of the object's single non-empty section."""
    return _macho_layout(obj)["payload"]


def _macho_layout(obj: bytes) -> dict:
    if len(obj) < 32:
        raise SubsetError("object shorter than a Mach-O header")
    magic, _cpu, _sub, filetype, ncmds, sizeofcmds, _flags, _res = struct.unpack_from(
        "<IiiIIIII", obj, 0)
    if magic != _MH_MAGIC_64 or filetype != _MH_OBJECT:
        raise SubsetError("not a 64-bit little-endian Mach-O relocatable object")
    off = 32
    seg_cmd = None
    data_sections = []
    symtab = None
    dysymtab = None
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", obj, off)
        if cmd == _LC_SEGMENT_64:
            if seg_cmd is not None:
                raise SubsetError("more than one segment command")
            seg_cmd = off
            nsects = struct.unpack_from("<I", obj, off + 64)[0]
            for s in range(nsects):
                so = off + 72 + 80 * s
                size, offset = struct.unpack_from("<QI", obj, so + 40)
                nreloc = struct.unpack_from("<I", obj, so + 60)[0]
                if nreloc:
                    raise SubsetError("ICU data object carries relocations")
                if size:
                    data_sections.append((so, offset, size))
        elif cmd == _LC_SYMTAB:
            symtab = off
        elif cmd == _LC_DYSYMTAB:
            dysymtab = off
        elif cmd != _LC_BUILD_VERSION and cmd not in _LC_VERSION_MIN:
            raise SubsetError(f"unexpected load command 0x{cmd:x}")
        off += cmdsize
    if off != 32 + sizeofcmds:
        raise SubsetError("load command sizes do not add up")
    if seg_cmd is None or symtab is None or len(data_sections) != 1:
        raise SubsetError("expected exactly one non-empty section and a symbol table")
    sect_off, data_off, data_size = data_sections[0]
    symoff, nsyms, stroff, strsize = struct.unpack_from("<IIII", obj, symtab + 8)
    data_end = data_off + data_size
    if not (data_end <= symoff and symoff + 16 * nsyms <= stroff and stroff + strsize <= len(obj)):
        raise SubsetError("symbol table does not follow the data section")
    return {
        "seg_cmd": seg_cmd, "sect_off": sect_off, "data_off": data_off,
        "data_size": data_size, "symtab": symtab, "dysymtab": dysymtab,
        "payload": obj[data_off:data_end],
    }


def replace_macho_payload(obj: bytes, payload: bytes) -> bytes:
    """Return ``obj`` with its single data section replaced by ``payload``.

    The symbol and string tables move by the size difference; symbol values
    are section-relative in this object (the section address is 0), so they
    need no change.
    """
    lay = _macho_layout(obj)
    if len(payload) % 8:
        payload += b"\0" * ((-len(payload)) % 8)
    delta = len(payload) - lay["data_size"]
    out = bytearray(obj[:lay["data_off"]]) + payload + obj[lay["data_off"] + lay["data_size"]:]
    seg = lay["seg_cmd"]
    vmsize, fileoff, filesize = struct.unpack_from("<QQQ", out, seg + 32)
    struct.pack_into("<Q", out, seg + 32, vmsize + delta)
    struct.pack_into("<Q", out, seg + 48, filesize + delta)
    struct.pack_into("<Q", out, lay["sect_off"] + 40, len(payload))
    symoff, nsyms, stroff, strsize = struct.unpack_from("<IIII", out, lay["symtab"] + 8)
    struct.pack_into("<II", out, lay["symtab"] + 8, symoff + delta, nsyms)
    struct.pack_into("<I", out, lay["symtab"] + 16, stroff + delta)
    if lay["dysymtab"] is not None:
        # tocoff, modtaboff, extrefsymoff, indirectsymoff, extreloff, locreloff
        for field in (32, 40, 48, 56, 64, 72):
            value = struct.unpack_from("<I", out, lay["dysymtab"] + field)[0]
            if value:
                struct.pack_into("<I", out, lay["dysymtab"] + field, value + delta)
    return bytes(out)


# ── Archive handling ───────────────────────────────────────────────────────


def _run(*args: str, cwd: Path | None = None) -> str:
    res = subprocess.run(args, cwd=cwd, capture_output=True, text=True, encoding="utf-8")
    if res.returncode != 0:
        raise SubsetError(f"{' '.join(args)} failed: {res.stderr.strip()}")
    return res.stdout


def _archs(archive: Path) -> list[str]:
    out = _run("lipo", "-archs", str(archive)).split()
    if not out:
        raise SubsetError("lipo reported no architectures")
    return out


def subset_thin_archive(thin: Path, out: Path, work: Path, dropped=is_dropped) -> dict:
    members = [m for m in _run("ar", "-t", str(thin)).splitlines() if "icudtl_dat" in m]
    if len(members) != 1:
        raise SubsetError(f"expected one ICU data member, found {members}")
    member = members[0]
    work.mkdir(parents=True, exist_ok=True)
    _run("ar", "-x", str(thin), member, cwd=work)
    obj_path = work / member
    obj = obj_path.read_bytes()
    payload = extract_macho_payload(obj)
    subset, stats = subset_common_data(payload, dropped)
    # Round-trip: every kept item must survive byte for byte.
    _, before = read_common_data(payload)
    _, after = read_common_data(subset)
    kept_before = [(n, p.rstrip(b"\0")) for n, p in before if not dropped(n)]
    if [(n, p.rstrip(b"\0")) for n, p in after] != kept_before:
        raise SubsetError("subset round-trip changed a kept item")
    obj_path.write_bytes(replace_macho_payload(obj, subset))
    shutil.copyfile(thin, out)
    _run("ar", "-r", str(out), member, cwd=work)
    _run("ranlib", str(out))
    if _run("ar", "-t", str(out)).splitlines() != _run("ar", "-t", str(thin)).splitlines():
        raise SubsetError("rewritten archive member list differs from the source")
    stats["member"] = member
    return stats


def subset_archive(archive: Path, output: Path, work: Path, dropped=is_dropped) -> dict:
    archs = _archs(archive)
    work.mkdir(parents=True, exist_ok=True)
    report = {"policy_version": POLICY_VERSION, "archive": str(archive), "slices": {}}
    if len(archs) == 1 and "fat" not in _run("lipo", "-info", str(archive)).lower():
        tmp_out = work / "out.a"
        report["slices"][archs[0]] = subset_thin_archive(archive, tmp_out, work / archs[0], dropped)
        output.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(str(tmp_out), output)
        return report
    thin_outs = []
    for arch in archs:
        thin = work / f"{arch}.a"
        _run("lipo", str(archive), "-thin", arch, "-output", str(thin))
        thin_out = work / f"{arch}.subset.a"
        report["slices"][arch] = subset_thin_archive(thin, thin_out, work / arch, dropped)
        thin_outs.append(str(thin_out))
    output.parent.mkdir(parents=True, exist_ok=True)
    tmp = output.with_suffix(".tmp.a")
    _run("lipo", "-create", *thin_outs, "-output", str(tmp))
    tmp.replace(output)
    return report


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--archive", type=Path, required=True)
    ap.add_argument("--output", type=Path, required=True)
    ap.add_argument("--work-dir", type=Path)
    ap.add_argument("--report", type=Path)
    args = ap.parse_args(argv)
    try:
        if args.work_dir:
            shutil.rmtree(args.work_dir, ignore_errors=True)
            report = subset_archive(args.archive, args.output, args.work_dir)
        else:
            with tempfile.TemporaryDirectory() as tmp:
                report = subset_archive(args.archive, args.output, Path(tmp))
    except SubsetError as exc:
        print(f"icu_data_subset: {exc}", file=sys.stderr)
        return 1
    if args.report:
        args.report.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    for arch, stats in report["slices"].items():
        print(f"icu_data_subset: {arch}: {stats['bytes_in']} -> {stats['bytes_out']} bytes "
              f"({stats['items_kept']}/{stats['items_in']} items)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Check that the native libraries export every JNI method the Android app declares.

    check_android_jni_exports.py --sources android/app/src/main --apk app-debug.apk [--require-abi arm64-v8a ...]
    check_android_jni_exports.py --sources android/app/src/main --lib libpulp.so [--lib ...]

Every Kotlin `external fun` and Java `native` method under --sources binds by
name to `Java_<package>_<Class>_<method>` in a loaded library (Pulp registers
no natives at run time). A static link keeps an object file only when
something references it, so a JNI function in a static library that nothing
in the .so calls is dropped, the build stays green, and Java's first call
throws UnsatisfiedLinkError. This check reads each library's ELF dynamic
symbol table directly (no NDK tools) and fails on any declared native that no
library exports. With --apk, every `lib/<abi>/libpulp.so` in the APK is
checked on its own: each ABI must carry the full set, and each --require-abi
must be present (an APK that silently lost an ABI would otherwise check fewer
libraries and still pass).

Exit status: 0 when every native is exported, 1 when one is missing, 2 on a
usage or input error (no natives found, no library, unreadable ELF).
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import struct
import sys
import zipfile


def mangle(part: str) -> str:
    # JNI short-name escaping for the characters Kotlin/Java identifiers use.
    return part.replace("_", "_1").replace("$", "_00024")


def declared_natives(path: Path) -> list[str]:
    """JNI names of the natives in one Kotlin or Java file. A companion
    object's methods bind to its outer class, and a Kotlin file's top-level
    functions to <File>Kt."""
    text = path.read_text(encoding="utf-8")
    # Block comments go first (newlines kept, so line structure survives): a
    # commented-out `external fun` declares nothing.
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)
    package = re.search(r"^\s*package\s+([\w.]+)", text, re.M)
    prefix = "Java_" + ("_".join(mangle(p) for p in package.group(1).split(".")) + "_" if package else "")
    out: list[str] = []
    stack: list[tuple[str, int]] = []
    depth = 0
    pending: str | None = None
    for line in text.splitlines():
        code = re.sub(r"//.*", "", line)
        decl = re.search(r"\b(companion\s+object|class|object|interface)\b\s*(\w+)?", code)
        if decl and not re.search(r"\bfun\b", code[:decl.start()]):
            pending = "<companion>" if decl.group(1).startswith("companion") else decl.group(2)
        native = re.search(r"\bexternal\s+fun\s+(\w+)|\bnative\s+[\w<>\[\]]+\s+(\w+)\s*\(", code)
        if native:
            classes = [name for name, _ in stack if name != "<companion>"]
            owner = "$".join(classes) if classes else (path.stem + "Kt" if path.suffix == ".kt" else path.stem)
            out.append(prefix + mangle(owner) + "_" + mangle(native.group(1) or native.group(2)))
        for ch in code:
            if ch == "{":
                depth += 1
                if pending is not None:
                    stack.append((pending, depth))
                    pending = None
            elif ch == "}":
                if stack and stack[-1][1] == depth:
                    stack.pop()
                depth -= 1
    return out


def elf_exports(data: bytes) -> set[str]:
    """Defined GLOBAL/WEAK, default- or protected-visibility names in .dynsym."""
    if data[:4] != b"\x7fELF":
        raise ValueError("not an ELF image")
    wide, e = data[4] == 2, "<" if data[5] == 1 else ">"
    if wide:
        shoff, = struct.unpack_from(e + "Q", data, 0x28)
        shentsize, shnum = struct.unpack_from(e + "HH", data, 0x3A)
        layout, symsize = e + "IIQQQQIIQQ", 24
    else:
        shoff, = struct.unpack_from(e + "I", data, 0x20)
        shentsize, shnum = struct.unpack_from(e + "HH", data, 0x2E)
        layout, symsize = e + "IIIIIIIIII", 16
    sections = [struct.unpack_from(layout, data, shoff + i * shentsize) for i in range(shnum)]
    names: set[str] = set()
    for sec in sections:
        if sec[1] != 11:  # SHT_DYNSYM
            continue
        strtab = sections[sec[6]][4]
        for i in range(sec[5] // symsize):
            at = sec[4] + i * symsize
            if wide:
                name, info, other, shndx = struct.unpack_from(e + "IBBH", data, at)
            else:
                name, = struct.unpack_from(e + "I", data, at)
                info, other, shndx = struct.unpack_from(e + "BBH", data, at + 12)
            if shndx != 0 and info >> 4 in (1, 2) and other & 3 in (0, 3):
                end = data.index(b"\0", strtab + name)
                names.add(data[strtab + name:end].decode("utf-8", "replace"))
    return names


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--sources", type=Path, required=True, help="the app's src/main directory")
    parser.add_argument("--apk", type=Path, help="check every lib/<abi>/libpulp.so in this APK")
    parser.add_argument("--lib", type=Path, action="append", default=[], help="a native library (repeatable)")
    parser.add_argument("--require-abi", action="append", default=[],
                        help="with --apk: an ABI whose lib/<abi>/libpulp.so must be present (repeatable)")
    args = parser.parse_args()
    if not args.apk and not args.lib:
        parser.error("give --apk or at least one --lib")
    if args.require_abi and not args.apk:
        parser.error("--require-abi applies to --apk")
    files = sorted(p for p in args.sources.rglob("*") if p.suffix in (".kt", ".java"))
    natives = sorted({n for f in files for n in declared_natives(f)})
    if not natives:
        print(f"error: no native method declared under {args.sources} (wrong --sources?)", file=sys.stderr)
        return 2
    groups: dict[str, set[str]] = {}
    try:
        if args.apk:
            with zipfile.ZipFile(args.apk) as apk:
                entries = sorted(n for n in apk.namelist() if re.fullmatch(r"lib/[^/]+/libpulp\.so", n))
                for entry in entries:
                    groups[entry] = elf_exports(apk.read(entry))
            if not entries:
                print(f"error: {args.apk} carries no lib/<abi>/libpulp.so", file=sys.stderr)
                return 2
            missing_abis = [abi for abi in args.require_abi if f"lib/{abi}/libpulp.so" not in entries]
            if missing_abis:
                print(f"FAIL {args.apk}: no libpulp.so for required ABI(s) {', '.join(missing_abis)}")
                return 1
        if args.lib:
            groups[" + ".join(str(p) for p in args.lib)] = set().union(*(elf_exports(p.read_bytes())
                                                                       for p in args.lib))
    except (OSError, ValueError, struct.error, zipfile.BadZipFile) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    failed = False
    for label, exported in groups.items():
        missing = [n for n in natives if n not in exported]
        if missing:
            failed = True
            print(f"FAIL {label}: {len(missing)} of {len(natives)} declared natives are not exported:")
            for name in missing:
                print(f"  {name}")
        else:
            print(f"ok   {label}: all {len(natives)} declared natives are exported")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

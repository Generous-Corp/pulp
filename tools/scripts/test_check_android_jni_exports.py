#!/usr/bin/env python3
"""Tests for check_android_jni_exports.py over ELF images assembled here.

Each fixture is a little-endian ELF64 shared object holding only .dynstr,
.dynsym and .shstrtab, so the test states exactly which symbols are exported
and runs on any host without the NDK. The app sources are small Kotlin and
Java files covering the name-binding rules the checker must get right.
"""

from __future__ import annotations

from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile

SCRIPT = Path(__file__).resolve().parent / "check_android_jni_exports.py"
sys.path.insert(0, str(SCRIPT.parent))

import check_android_jni_exports as checker  # noqa: E402

KOTLIN = """package com.pulp.midi

class PulpMidiManager {
    inner class Listener {
        fun onDeviceRemoved(id: Int) { nativeOnDeviceRemoved(id) } // not a declaration
    }
    companion object {
        @JvmStatic external fun nativeStatic(): Int
    }
    private external fun nativeOnDeviceRemoved(id: Int)
    /* A retired native, kept for reference:
       private external fun nativeRetired(): Int */
    // private external fun nativeLineCommented(): Int
}

private external fun nativeTopLevel(): Int

class Outer {
    class Nested_Type {
        external fun nativeNested()
    }
}
"""

JAVA = """package com.pulp.audio;

public class PulpEngine {
    private static native int nativeStart(int rate);
    /* private static native int nativeOld(int rate); */
}
"""

EXPECTED = [
    "Java_com_pulp_audio_PulpEngine_nativeStart",
    "Java_com_pulp_midi_MidiKt_nativeTopLevel",
    "Java_com_pulp_midi_Outer_00024Nested_1Type_nativeNested",
    "Java_com_pulp_midi_PulpMidiManager_nativeOnDeviceRemoved",
    "Java_com_pulp_midi_PulpMidiManager_nativeStatic",
]


def elf_so(exports: list[str], hidden: list[str] = (), undefined: list[str] = (),
           local: list[str] = ()) -> bytes:
    names = [*exports, *hidden, *undefined, *local]
    dynstr, offsets = b"\0", {}
    for n in names:
        offsets[n] = len(dynstr)
        dynstr += n.encode() + b"\0"
    syms = struct.pack("<IBBHQQ", 0, 0, 0, 0, 0, 0)
    syms += b"".join(struct.pack("<IBBHQQ", offsets[n], 0x12, 0, 1, 0x1000, 4) for n in exports)
    syms += b"".join(struct.pack("<IBBHQQ", offsets[n], 0x12, 2, 1, 0x1000, 4) for n in hidden)
    # An undefined reference: GLOBAL, default visibility, section index 0.
    syms += b"".join(struct.pack("<IBBHQQ", offsets[n], 0x12, 0, 0, 0, 0) for n in undefined)
    # A defined LOCAL-binding symbol (st_info bind 0), default visibility.
    syms += b"".join(struct.pack("<IBBHQQ", offsets[n], 0x02, 0, 1, 0x1000, 4) for n in local)
    shstr = b"\0.dynstr\0.dynsym\0.shstrtab\0"
    body = dynstr + b"\0" * (-len(dynstr) % 8)
    sym_at = 64 + len(body)
    body += syms
    shstr_at = 64 + len(body)
    body += shstr + b"\0" * (-len(shstr) % 8)
    shoff = 64 + len(body)
    header = b"\x7fELF" + bytes([2, 1, 1, 0]) + b"\0" * 8
    header += struct.pack("<HHIQQQIHHHHHH", 3, 183, 1, 0, 0, shoff, 0, 64, 0, 0, 64, 4, 3)
    sections = struct.pack("<IIQQQQIIQQ", *[0] * 10)
    sections += struct.pack("<IIQQQQIIQQ", 1, 3, 0, 0, 64, len(dynstr), 0, 0, 1, 0)
    sections += struct.pack("<IIQQQQIIQQ", 9, 11, 0, 0, sym_at, len(syms), 1, 1, 8, 24)
    sections += struct.pack("<IIQQQQIIQQ", 17, 3, 0, 0, shstr_at, len(shstr), 0, 0, 1, 0)
    return header + body + sections


def elf32_so(exports: list[str]) -> bytes:
    """A little-endian ELF32 ARM shared object (armeabi-v7a layout)."""
    dynstr, offsets = b"\0", {}
    for n in exports:
        offsets[n] = len(dynstr)
        dynstr += n.encode() + b"\0"
    syms = struct.pack("<IIIBBH", 0, 0, 0, 0, 0, 0)
    syms += b"".join(struct.pack("<IIIBBH", offsets[n], 0x1000, 4, 0x12, 0, 1) for n in exports)
    shstr = b"\0.dynstr\0.dynsym\0.shstrtab\0"
    body = dynstr + b"\0" * (-len(dynstr) % 4)
    sym_at = 52 + len(body)
    body += syms
    shstr_at = 52 + len(body)
    body += shstr + b"\0" * (-len(shstr) % 4)
    shoff = 52 + len(body)
    header = b"\x7fELF" + bytes([1, 1, 1, 0]) + b"\0" * 8
    header += struct.pack("<HHIIIIIHHHHHH", 3, 40, 1, 0, 0, shoff, 0, 52, 0, 0, 40, 4, 3)
    sections = struct.pack("<IIIIIIIIII", *[0] * 10)
    sections += struct.pack("<IIIIIIIIII", 1, 3, 0, 0, 52, len(dynstr), 0, 0, 1, 0)
    sections += struct.pack("<IIIIIIIIII", 9, 11, 0, 0, sym_at, len(syms), 1, 1, 4, 16)
    sections += struct.pack("<IIIIIIIIII", 17, 3, 0, 0, shstr_at, len(shstr), 0, 0, 1, 0)
    return header + body + sections


class CheckAndroidJniExportsTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        src = self.root / "src" / "main"
        (src / "kotlin" / "com" / "pulp" / "midi").mkdir(parents=True)
        (src / "java" / "com" / "pulp" / "audio").mkdir(parents=True)
        (src / "kotlin" / "com" / "pulp" / "midi" / "Midi.kt").write_text(KOTLIN, encoding="utf-8")
        (src / "java" / "com" / "pulp" / "audio" / "PulpEngine.java").write_text(JAVA, encoding="utf-8")
        self.sources = src

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def run_checker(self, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run([sys.executable, str(SCRIPT), "--sources", str(self.sources), *args],
                              capture_output=True, text=True, encoding="utf-8")

    def lib(self, name: str, exports: list[str], hidden: list[str] = (), undefined: list[str] = (),
            local: list[str] = ()) -> Path:
        path = self.root / name
        path.write_bytes(elf_so(exports, hidden, undefined, local))
        return path

    def test_declarations_bind_to_the_jni_names_the_vm_looks_up(self):
        names = sorted(n for f in sorted(self.sources.rglob("*")) if f.is_file()
                       for n in checker.declared_natives(f))
        self.assertEqual(names, EXPECTED)

    def test_every_native_exported_passes(self):
        res = self.run_checker("--lib", str(self.lib("libpulp.so", [*EXPECTED, "JNI_OnLoad"])))
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn("all 5 declared natives are exported", res.stdout)

    def test_a_dropped_native_fails_and_is_named(self):
        res = self.run_checker("--lib", str(self.lib("libpulp.so", EXPECTED[:3] + EXPECTED[4:])))
        self.assertEqual(res.returncode, 1)
        self.assertIn("Java_com_pulp_midi_PulpMidiManager_nativeOnDeviceRemoved", res.stdout)

    def test_a_hidden_definition_is_not_an_export(self):
        lib = self.lib("libpulp.so", EXPECTED[1:], hidden=EXPECTED[:1])
        res = self.run_checker("--lib", str(lib))
        self.assertEqual(res.returncode, 1)
        self.assertIn(EXPECTED[0], res.stdout)

    def test_an_undefined_reference_is_not_an_export(self):
        # A library that only references a JNI name (another library defines
        # it) lists it in .dynsym with section index 0; that is not an export.
        lib = self.lib("libpulp.so", EXPECTED[1:], undefined=EXPECTED[:1])
        res = self.run_checker("--lib", str(lib))
        self.assertEqual(res.returncode, 1)
        self.assertIn(EXPECTED[0], res.stdout)

    def test_a_local_binding_is_not_an_export(self):
        # A LOCAL symbol in .dynsym is defined but invisible to the VM's lookup.
        lib = self.lib("libpulp.so", EXPECTED[1:], local=EXPECTED[:1])
        res = self.run_checker("--lib", str(lib))
        self.assertEqual(res.returncode, 1)
        self.assertIn(EXPECTED[0], res.stdout)

    def test_a_32_bit_library_is_read(self):
        # armeabi-v7a images are ELF32; their exports count, and a missing one fails.
        good = self.root / "libpulp32.so"
        good.write_bytes(elf32_so(EXPECTED))
        self.assertEqual(self.run_checker("--lib", str(good)).returncode, 0)
        bad = self.root / "libpulp32-missing.so"
        bad.write_bytes(elf32_so(EXPECTED[1:]))
        res = self.run_checker("--lib", str(bad))
        self.assertEqual(res.returncode, 1)
        self.assertIn(EXPECTED[0], res.stdout)

    def test_a_required_abi_missing_from_the_apk_fails(self):
        apk = self.root / "app.apk"
        with zipfile.ZipFile(apk, "w") as z:
            z.writestr("lib/arm64-v8a/libpulp.so", elf_so(EXPECTED))
        res = self.run_checker("--apk", str(apk), "--require-abi", "arm64-v8a", "--require-abi", "x86_64")
        self.assertEqual(res.returncode, 1)
        self.assertIn("required ABI(s) x86_64", res.stdout)
        self.assertEqual(self.run_checker("--apk", str(apk), "--require-abi", "arm64-v8a").returncode, 0)

    def test_natives_may_be_split_across_libraries(self):
        res = self.run_checker("--lib", str(self.lib("a.so", EXPECTED[:2])),
                               "--lib", str(self.lib("b.so", EXPECTED[2:])))
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)

    def test_apk_checks_each_abi_on_its_own(self):
        apk = self.root / "app.apk"
        with zipfile.ZipFile(apk, "w") as z:
            z.writestr("lib/arm64-v8a/libpulp.so", elf_so(EXPECTED))
            z.writestr("lib/x86_64/libpulp.so", elf_so(EXPECTED[1:]))
        res = self.run_checker("--apk", str(apk))
        self.assertEqual(res.returncode, 1)
        self.assertIn("ok   lib/arm64-v8a/libpulp.so", res.stdout)
        self.assertIn("FAIL lib/x86_64/libpulp.so", res.stdout)

    def test_no_declared_natives_is_an_input_error_not_a_pass(self):
        res = subprocess.run([sys.executable, str(SCRIPT), "--sources", str(self.root / "empty"),
                              "--lib", str(self.lib("libpulp.so", EXPECTED))], capture_output=True, text=True, encoding="utf-8")
        self.assertEqual(res.returncode, 2)

    def test_an_apk_without_libpulp_is_an_input_error(self):
        apk = self.root / "app.apk"
        with zipfile.ZipFile(apk, "w") as z:
            z.writestr("classes.dex", b"dex")
        self.assertEqual(self.run_checker("--apk", str(apk)).returncode, 2)


if __name__ == "__main__":
    unittest.main()

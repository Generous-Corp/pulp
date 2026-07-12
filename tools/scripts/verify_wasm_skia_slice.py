#!/usr/bin/env python3
"""Assert the invariants Pulp's Emscripten build relies on in the Skia wasm slice.

The published skia-builder wasm asset (`skia-build-wasm-wasm32-gpu-release.zip`)
is a **Ganesh/WebGL2** slice, not Graphite/Dawn. Two consequences are load-bearing
for `tools/cmake/FindSkia.cmake`:

  * `libskia.a` defines the Ganesh GL entry points Pulp's web surface calls
    (`GrGLInterfaces::MakeWebGL`, `GrDirectContexts::MakeGL`) and NO wgpu/Dawn
    symbols, so the Emscripten arm defines `SK_GANESH;SK_GL` and never
    `SK_GRAPHITE;SK_DAWN`.
  * `libskottie.a` references ~360 `skjson::*` symbols that nothing in the zip
    defines (no jsonreader / skresources archive is packaged), so FindSkia drops
    libskottie.a + libsksg.a from the link. This script fails loudly the day
    skia-builder starts packaging those archives — that is the signal to delete
    the exclusion and re-enable Lottie on wasm.

Usage:
    python3 tools/scripts/verify_wasm_skia_slice.py [SKIA_DIR]
    python3 tools/scripts/verify_wasm_skia_slice.py --self-test

Exit codes: 0 pass, 1 assertion failed, 2 usage error, 77 skipped (no wasm slice
at SKIA_DIR, or no llvm-nm on PATH) — 77 is CTest's SKIP_RETURN_CODE.
"""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path

SKIP = 77

REQUIRED_LIBS = (
    "libskia.a",
    "libskparagraph.a",
    "libskshaper.a",
    "libskunicode_core.a",
    "libskunicode_icu.a",
)

# Substrings matched against `llvm-nm --defined-only` output. The C++ names are
# Itanium-mangled in the archive, so match on the mangled fragment.
REQUIRED_SKIA_SYMBOLS = (
    "SkFontMgr_New_Custom_Empty",
    "GrGLInterfaces9MakeWebGL",
    "GrDirectContexts6MakeGL",
)


def _find_llvm_nm() -> str | None:
    override = os.environ.get("LLVM_NM")
    if override and shutil.which(override):
        return override
    found = shutil.which("llvm-nm")
    if found:
        return found
    if shutil.which("xcrun"):
        proc = subprocess.run(
            ["xcrun", "-f", "llvm-nm"], capture_output=True, text=True, check=False
        )
        if proc.returncode == 0 and proc.stdout.strip():
            return proc.stdout.strip()
    return None


def _nm(nm: str, archive: Path, *flags: str) -> str:
    proc = subprocess.run(
        [nm, *flags, str(archive)], capture_output=True, text=True, check=False
    )
    # llvm-nm exits non-zero on "no symbols" members while still printing the
    # useful output, so tolerate a non-zero rc when stdout is populated.
    if proc.returncode != 0 and not proc.stdout:
        raise SystemExit(f"ERROR: {nm} failed on {archive}: {proc.stderr.strip()}")
    return proc.stdout


def _has_wasm_slice(root: Path) -> bool:
    return (root / "build" / "wasm-gpu" / "lib" / "Release").is_dir()


def _resolve_skia_dir(argv_dir: str | None) -> Path | None:
    # An explicit directory is authoritative: never silently fall back to a
    # different slice than the caller named.
    if argv_dir:
        root = Path(argv_dir)
        return root if _has_wasm_slice(root) else None
    for candidate in (
        os.environ.get("SKIA_DIR"),
        os.path.expanduser("~/.cache/pulp/skia-build-wasm"),
        "external/skia-build",
    ):
        if not candidate:
            continue
        root = Path(candidate)
        if _has_wasm_slice(root):
            return root
    return None


def verify(skia_dir: Path) -> int:
    lib_dir = skia_dir / "build" / "wasm-gpu" / "lib" / "Release"
    failures: list[str] = []

    for name in REQUIRED_LIBS:
        if not (lib_dir / name).is_file():
            failures.append(f"missing required archive: {lib_dir / name}")

    icudtl = skia_dir / "build" / "share" / "icudtl.dat"
    if not icudtl.is_file():
        failures.append(f"missing ICU data file: {icudtl}")

    nm = _find_llvm_nm()
    if nm is None:
        print("SKIP: llvm-nm not available — cannot inspect archive symbols")
        return SKIP

    skia_lib = lib_dir / "libskia.a"
    if skia_lib.is_file():
        defined = _nm(nm, skia_lib, "--defined-only")
        for sym in REQUIRED_SKIA_SYMBOLS:
            if sym not in defined:
                failures.append(
                    f"libskia.a does not define {sym} — the slice is not the "
                    f"expected Ganesh/WebGL2 build"
                )
        all_syms = _nm(nm, skia_lib).lower()
        dawn_hits = [
            line
            for line in all_syms.splitlines()
            if "wgpu" in line or "dawn" in line
        ]
        if dawn_hits:
            failures.append(
                f"libskia.a contains {len(dawn_hits)} wgpu/Dawn symbol(s); the "
                f"wasm slice is expected to have none. If skia-builder now ships "
                f"a Dawn-backed wasm slice, revisit the SK_GANESH/SK_GL choice in "
                f"tools/cmake/FindSkia.cmake."
            )

    skottie = lib_dir / "libskottie.a"
    if skottie.is_file():
        undefined = _nm(nm, skottie, "--undefined-only")
        skjson_undefined = sum(1 for line in undefined.splitlines() if "skjson" in line)
        if skjson_undefined == 0:
            failures.append(
                "libskottie.a no longer has undefined skjson::* symbols — the "
                "published slice appears fixed. Remove the libskottie/libsksg "
                "exclusion from tools/cmake/FindSkia.cmake (EMSCRIPTEN arm) and "
                "update this assertion."
            )

    if failures:
        print("FAIL: wasm Skia slice does not match Pulp's expectations:")
        for f in failures:
            print(f"  - {f}")
        return 1

    print(f"OK: wasm Skia slice at {skia_dir} matches Pulp's expectations")
    return 0


def self_test() -> int:
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from fetch_skia_for_release import MATRIX_TO_MANIFEST, expected_library_path

    assert MATRIX_TO_MANIFEST["wasm"] == "wasm", MATRIX_TO_MANIFEST.get("wasm")
    got = expected_library_path("wasm")
    want = Path("external/skia-build/build/wasm-gpu/lib/Release/libskia.a")
    assert got == want, f"expected_library_path('wasm') = {got}, want {want}"
    got_dest = expected_library_path("wasm", "/tmp/skia")
    want_dest = Path("/tmp/skia/build/wasm-gpu/lib/Release/libskia.a")
    assert got_dest == want_dest, f"{got_dest} != {want_dest}"
    print("OK: fetch_skia_for_release wasm mapping self-test passed")
    return 0


def main(argv: list[str]) -> int:
    args = argv[1:]
    if "--self-test" in args:
        args.remove("--self-test")
        if args:
            print(f"usage: {argv[0]} --self-test", file=sys.stderr)
            return 2
        return self_test()
    if len(args) > 1:
        print(f"usage: {argv[0]} [SKIA_DIR] | --self-test", file=sys.stderr)
        return 2

    skia_dir = _resolve_skia_dir(args[0] if args else None)
    if skia_dir is None:
        print(
            "SKIP: no build/wasm-gpu/lib/Release under the requested SKIA_DIR "
            "(fetch it with `python3 tools/scripts/fetch_skia_for_release.py "
            "wasm --dest ~/.cache/pulp/skia-build-wasm`)"
        )
        return SKIP
    return verify(skia_dir)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
